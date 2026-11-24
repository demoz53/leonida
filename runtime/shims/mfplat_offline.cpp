// mfplat_offline: Media Foundation (mfplat.dll) offline source-resolver proxy for mapped titles.
//
// Nothing in Leonida reaches the network. While winhttp_offline.cpp covers WinHTTP and host mfplat.dll provides local
// media decoding, Media Foundation's built-in IMFSourceResolver can fetch http/https/rtsp/mms URLs and UNC paths on
// its own. mfplat.dll therefore stays a partially overridden host DLL in rt::shims::owns_dll: MFStartup, media types,
// transforms, and byte-stream decoders run natively from System32, while MFCreateSourceResolver returns a COM proxy
// that:
//   - refuses non-local URL schemes, UNC shares, and non-local file:// hosts with MF_E_UNSUPPORTED_SCHEME, logging
//     each (scheme, host) pair once;
//   - default-denies everything else: only GAME: paths, absolute drive paths (X:\ or X:/), and file:/// or
//     file://localhost/ URLs whose percent-decoded path is one of those pass. Relative, drive-relative, and UNC
//     forms are refused, and so is a host drive path on a network drive;
//   - rewrites console paths (GAME:, G:, T:) through rt::fs::resolve_read_path so movies in the package or overlay
//     open through Media Foundation without touching the original package on write;
//   - answers QueryInterface only for IUnknown and IMFSourceResolver so the unguarded host resolver cannot escape.
// Only the IMFSourceResolver URL methods are gated. Byte-stream methods and MFCreateSourceReaderFromURL
// (mfreadwrite.dll) are not.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <unknwn.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <utility>

#include "file_system.h"
#include "mfplat_offline.h"
#include "shims.h"

namespace rt::mf {
namespace {

constexpr GUID kIidIUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}};

bool is_ascii_alpha(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }

// "X:" followed by a separator. Drive-relative "X:name" is not a drive path.
bool is_drive_prefix(const std::wstring& s, size_t pos = 0) {
    return pos + 3 <= s.size() && is_ascii_alpha(s[pos]) && s[pos + 1] == L':' &&
           (s[pos + 2] == L'\\' || s[pos + 2] == L'/');
}

bool is_game_prefix(const std::wstring& s, size_t pos = 0) {
    if (pos + 5 > s.size()) return false;
    return std::towlower(s[pos]) == L'g' && std::towlower(s[pos + 1]) == L'a' && std::towlower(s[pos + 2]) == L'm' &&
           std::towlower(s[pos + 3]) == L'e' && s[pos + 4] == L':';
}

bool is_scheme_char(wchar_t c) {
    return is_ascii_alpha(c) || (c >= L'0' && c <= L'9') || c == L'+' || c == L'-' || c == L'.';
}

std::string narrow_ascii_lower(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c >= L'A' && c <= L'Z') {
            out.push_back(static_cast<char>(c + 32));
        } else if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::string narrow_host(const std::wstring& rest) {
    size_t end = rest.find_first_of(L"/\\");
    const std::wstring host = (end == std::wstring::npos) ? rest : rest.substr(0, end);
    return narrow_ascii_lower(host);
}

int hex_digit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
    if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
    return -1;
}

// Percent-decodes UTF-8 octets in a file:// URL path into UTF-16 while preserving non-ASCII code units verbatim.
std::wstring percent_decode_wide(const std::wstring& in) {
    std::string utf8;
    utf8.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] == L'%' && i + 2 < in.size()) {
            const int hi = hex_digit(in[i + 1]);
            const int lo = hex_digit(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                utf8.push_back(static_cast<char>((hi << 4) | lo));
                i += 3;
                continue;
            }
        }
        const uint32_t cp = static_cast<uint32_t>(in[i++]);
        if (cp < 0x80) {
            utf8.push_back(static_cast<char>(cp));
        } else {
            // Flush UTF-8 buffer via MultiByteToWideChar if we encounter wide chars directly.
            char mb[4] = {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(&in[i - 1]), 1, mb,
                                              static_cast<int>(sizeof(mb)), nullptr, nullptr);
            if (n > 0) utf8.append(mb, static_cast<size_t>(n));
        }
    }
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return in;
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

void log_refused_once(const std::string& scheme, const std::string& host) {
    // Intentionally immortal: COM teardown can run during process shutdown.
    static auto* mutex = new std::mutex;
    static auto* seen = new std::set<std::pair<std::string, std::string>>;
    std::lock_guard lock(*mutex);
    if (seen->emplace(scheme, host).second) {
        std::fprintf(stderr, "[rt] mfplat: offline, refused non-local URL (scheme=%s host=%s)\n", scheme.c_str(),
                     host.empty() ? "<none>" : host.c_str());
    }
}

HRESULT check_and_log_url(const wchar_t* url, std::wstring& out) {
    std::string scheme;
    std::string host;
    const HRESULT hr = resolve_media_url(url, out, &scheme, &host);
    if (hr == kUnsupportedScheme) {
        log_refused_once(scheme, host);
    }
    return hr;
}

// ---- Offline IMFSourceResolver COM Proxy ----

struct ResolverProxy;

struct ResolverVtable {
    HRESULT(STDMETHODCALLTYPE* query_interface)(ResolverProxy*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* add_ref)(ResolverProxy*);
    ULONG(STDMETHODCALLTYPE* release)(ResolverProxy*);
    HRESULT(STDMETHODCALLTYPE* create_object_from_url)(ResolverProxy*, LPCWSTR, DWORD, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* create_object_from_byte_stream)(ResolverProxy*, IUnknown*, LPCWSTR, DWORD, IUnknown*,
                                                               DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* begin_create_object_from_url)(ResolverProxy*, LPCWSTR, DWORD, IUnknown*, IUnknown**,
                                                             IUnknown*, IUnknown*);
    HRESULT(STDMETHODCALLTYPE* end_create_object_from_url)(ResolverProxy*, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* begin_create_object_from_byte_stream)(ResolverProxy*, IUnknown*, LPCWSTR, DWORD,
                                                                     IUnknown*, IUnknown**, IUnknown*, IUnknown*);
    HRESULT(STDMETHODCALLTYPE* end_create_object_from_byte_stream)(ResolverProxy*, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* cancel_object_creation)(ResolverProxy*, IUnknown*);
};

struct ResolverProxy {
    const ResolverVtable* vtable;
    IUnknown* host;
    std::atomic<ULONG> refs;
};

template <typename Fn>
Fn host_slot(IUnknown* host, size_t index) {
    void** vtbl = *reinterpret_cast<void***>(host);
    return reinterpret_cast<Fn>(vtbl[index]);
}

HRESULT STDMETHODCALLTYPE proxy_query_interface(ResolverProxy* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (std::memcmp(&riid, &kIidIUnknown, sizeof(GUID)) == 0 ||
        std::memcmp(&riid, &kIidSourceResolver, sizeof(GUID)) == 0) {
        self->refs.fetch_add(1, std::memory_order_relaxed);
        *out = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE proxy_add_ref(ResolverProxy* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE proxy_release(ResolverProxy* self) {
    const ULONG left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        IUnknown* host = self->host;
        delete self;
        if (host) host->Release();
    }
    return left;
}

HRESULT STDMETHODCALLTYPE proxy_create_from_url(ResolverProxy* self, LPCWSTR url, DWORD flags, IUnknown* props,
                                                DWORD* object_type, IUnknown** object) {
    std::wstring resolved;
    const HRESULT check = check_and_log_url(url, resolved);
    if (FAILED(check)) {
        if (object) *object = nullptr;
        return check;
    }
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, LPCWSTR, DWORD, IUnknown*, DWORD*, IUnknown**);
    return host_slot<Fn>(self->host, 3)(self->host, resolved.c_str(), flags, props, object_type, object);
}

HRESULT STDMETHODCALLTYPE proxy_create_from_stream(ResolverProxy* self, IUnknown* stream, LPCWSTR url_hint, DWORD flags,
                                                   IUnknown* props, DWORD* object_type, IUnknown** object) {
    // URL is only a MIME/extension hint for byte-stream handler selection.
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IUnknown*, LPCWSTR, DWORD, IUnknown*, DWORD*, IUnknown**);
    return host_slot<Fn>(self->host, 4)(self->host, stream, url_hint, flags, props, object_type, object);
}

HRESULT STDMETHODCALLTYPE proxy_begin_from_url(ResolverProxy* self, LPCWSTR url, DWORD flags, IUnknown* props,
                                               IUnknown** cancel_cookie, IUnknown* callback, IUnknown* state) {
    std::wstring resolved;
    const HRESULT check = check_and_log_url(url, resolved);
    if (FAILED(check)) {
        if (cancel_cookie) *cancel_cookie = nullptr;
        return check;
    }
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, LPCWSTR, DWORD, IUnknown*, IUnknown**, IUnknown*, IUnknown*);
    return host_slot<Fn>(self->host, 5)(self->host, resolved.c_str(), flags, props, cancel_cookie, callback, state);
}

HRESULT STDMETHODCALLTYPE proxy_end_from_url(ResolverProxy* self, IUnknown* result, DWORD* object_type,
                                             IUnknown** object) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IUnknown*, DWORD*, IUnknown**);
    return host_slot<Fn>(self->host, 6)(self->host, result, object_type, object);
}

HRESULT STDMETHODCALLTYPE proxy_begin_from_stream(ResolverProxy* self, IUnknown* stream, LPCWSTR url_hint, DWORD flags,
                                                  IUnknown* props, IUnknown** cancel_cookie, IUnknown* callback,
                                                  IUnknown* state) {
    using Fn =
        HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IUnknown*, LPCWSTR, DWORD, IUnknown*, IUnknown**, IUnknown*, IUnknown*);
    return host_slot<Fn>(self->host, 7)(self->host, stream, url_hint, flags, props, cancel_cookie, callback, state);
}

HRESULT STDMETHODCALLTYPE proxy_end_from_stream(ResolverProxy* self, IUnknown* result, DWORD* object_type,
                                                IUnknown** object) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IUnknown*, DWORD*, IUnknown**);
    return host_slot<Fn>(self->host, 8)(self->host, result, object_type, object);
}

HRESULT STDMETHODCALLTYPE proxy_cancel(ResolverProxy* self, IUnknown* cancel_cookie) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IUnknown*);
    return host_slot<Fn>(self->host, 9)(self->host, cancel_cookie);
}

const ResolverVtable kResolverVtable = {
    &proxy_query_interface, &proxy_add_ref,        &proxy_release,          &proxy_create_from_url,
    &proxy_create_from_stream, &proxy_begin_from_url, &proxy_end_from_url,  &proxy_begin_from_stream,
    &proxy_end_from_stream, &proxy_cancel,
};

}  // namespace

HRESULT resolve_media_url(const wchar_t* url, std::wstring& out, std::string* refused_scheme,
                          std::string* refused_host) {
    out.clear();
    if (refused_scheme) refused_scheme->clear();
    if (refused_host) refused_host->clear();
    if (!url) return E_POINTER;

    const std::wstring text(url);
    auto refuse = [&](const std::string& scheme, const std::string& host) {
        if (refused_scheme) *refused_scheme = scheme;
        if (refused_host) *refused_host = host;
        return kUnsupportedScheme;
    };

    // Default deny: only GAME: paths, absolute drive paths, and local file URLs that name one of those get through.
    std::wstring candidate;
    if (text.starts_with(L"\\??\\") || text.starts_with(L"\\\\.\\")) {
        return refuse("unc", "");
    }
    if (text.starts_with(L"\\\\?\\")) {
        if (!is_drive_prefix(text, 4)) {
            std::wstring rest = text.substr(4);
            if (rest.size() >= 4 && std::towlower(rest[0]) == L'u' && std::towlower(rest[1]) == L'n' &&
                std::towlower(rest[2]) == L'c' && rest[3] == L'\\') {
                rest.erase(0, 4);
            }
            return refuse("unc", narrow_host(rest));
        }
        candidate = text;
    } else if (text.starts_with(L"\\\\") || text.starts_with(L"//")) {
        return refuse("unc", narrow_host(text.substr(2)));
    } else if (is_game_prefix(text) || is_drive_prefix(text)) {
        candidate = text;
    } else {
        const size_t colon = text.find(L':');
        bool has_scheme = colon != std::wstring::npos && colon > 1;
        if (has_scheme) {
            for (size_t i = 0; i < colon; ++i) {
                if (!is_scheme_char(text[i])) {
                    has_scheme = false;
                    break;
                }
            }
        }
        if (has_scheme) {
            const std::string scheme = narrow_ascii_lower(text.substr(0, colon));
            const std::wstring rest = text.substr(colon + 1);
            const bool has_authority = rest.starts_with(L"//");
            if (scheme != "file") {
                return refuse(scheme, has_authority ? narrow_host(rest.substr(2)) : std::string{});
            }
            std::wstring path_part;
            if (has_authority) {
                const std::wstring after_slashes = rest.substr(2);
                const size_t sep = after_slashes.find_first_of(L"/\\");
                const std::wstring host_w = (sep == std::wstring::npos) ? after_slashes : after_slashes.substr(0, sep);
                const std::string host = narrow_ascii_lower(host_w);
                if (!host.empty() && host != "localhost") {
                    return refuse("file", host);
                }
                path_part = (sep == std::wstring::npos) ? std::wstring{} : after_slashes.substr(sep);
            } else {
                path_part = rest;
            }
            size_t trim = 0;
            while (trim < path_part.size() && (path_part[trim] == L'/' || path_part[trim] == L'\\')) ++trim;
            std::wstring decoded = percent_decode_wide(path_part.substr(trim));
            for (wchar_t& ch : decoded) {
                if (ch == L'/') ch = L'\\';
            }
            if (!is_game_prefix(decoded) && !is_drive_prefix(decoded)) {
                // File URL without a console or drive path (no drive, or a relative path): refused.
                return refuse("file", "");
            }
            candidate = std::move(decoded);
        } else {
            // No scheme: relative path, drive-relative path ("C:name"), or rooted path. Refused.
            return refuse("relative", "");
        }
    }

    std::uint32_t win32_err = 0;
    if (!rt::fs::resolve_read_path(candidate, out, &win32_err)) {
        return win32_err ? HRESULT_FROM_WIN32(win32_err) : E_ACCESSDENIED;
    }
    // A host drive the title names itself (resolve_read_path returns it unchanged) that is mapped to a network share is
    // a remote host, so it is refused. Console paths resolve under the configured game/overlay folders, which every
    // other file shim reads from wherever they live.
    const std::wstring host_path = out.starts_with(L"\\\\?\\") ? out.substr(4) : out;
    if (out == candidate && is_drive_prefix(host_path)) {
        const std::wstring root = host_path.substr(0, 2) + L"\\";
        if (GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) {
            return refuse("remote-drive", narrow_ascii_lower(root));
        }
    }
    return S_OK;
}

HRESULT wrap_source_resolver(IUnknown* host_resolver, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!host_resolver) return E_INVALIDARG;
    auto* proxy = new (std::nothrow) ResolverProxy{&kResolverVtable, host_resolver, {1}};
    if (!proxy) return E_OUTOFMEMORY;
    *out = proxy;
    return S_OK;
}

}  // namespace rt::mf

namespace {

HRESULT STDAPICALLTYPE mf_create_source_resolver_shim(void** out) {
    // Creates the host resolver from System32 mfplat.dll and wraps it in the offline proxy.
    if (!out) return E_POINTER;
    *out = nullptr;
    using CreateFn = HRESULT(STDAPICALLTYPE*)(IUnknown**);
    // Loaded once and never freed, so calls do not leak LoadLibraryExW references. A failure is remembered with its
    // error code, so later calls report the same one.
    static DWORD failure = 0;
    static const HMODULE mod = [] {
        HMODULE m = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) failure = GetLastError();
        return m;
    }();
    static const CreateFn real_create = [] {
        if (!mod) return static_cast<CreateFn>(nullptr);
        CreateFn fn = reinterpret_cast<CreateFn>(GetProcAddress(mod, "MFCreateSourceResolver"));
        if (!fn) failure = GetLastError();
        return fn;
    }();
    if (!real_create) return failure ? HRESULT_FROM_WIN32(failure) : E_FAIL;
    IUnknown* host = nullptr;
    const HRESULT hr = real_create(&host);
    if (FAILED(hr) || !host) return FAILED(hr) ? hr : E_FAIL;
    const HRESULT wrap_hr = rt::mf::wrap_source_resolver(host, out);
    if (FAILED(wrap_hr)) host->Release();
    return wrap_hr;
}

}  // namespace

SHIM_TABLE(mfplat_offline,
    {"mfplat.dll", "MFCreateSourceResolver", reinterpret_cast<void*>(&mf_create_source_resolver_shim)},
);
