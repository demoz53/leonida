// Checks the offline Media Foundation (mfplat.dll) source-resolver proxy (runtime/shims/mfplat_offline.cpp).
//
// Verifies URL filtering (network schemes, UNC shares, and remote file:// hosts are refused with
// MF_E_UNSUPPORTED_SCHEME), console-path translation (GAME:, G:, T: and file:/// URLs into the game/overlay roots),
// and COM proxy isolation against a synthetic 10-slot mock IMFSourceResolver. No network or game data is touched.
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
#include <filesystem>
#include <fstream>
#include <string>

#include "file_system.h"
#include "mfplat_offline.h"
#include "shims.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

struct MockResolver;

struct MockVtable {
    HRESULT(STDMETHODCALLTYPE* qi)(MockResolver*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* add_ref)(MockResolver*);
    ULONG(STDMETHODCALLTYPE* release)(MockResolver*);
    HRESULT(STDMETHODCALLTYPE* create_url)(MockResolver*, LPCWSTR, DWORD, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* create_stream)(MockResolver*, IUnknown*, LPCWSTR, DWORD, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* begin_url)(MockResolver*, LPCWSTR, DWORD, IUnknown*, IUnknown**, IUnknown*, IUnknown*);
    HRESULT(STDMETHODCALLTYPE* end_url)(MockResolver*, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* begin_stream)(MockResolver*, IUnknown*, LPCWSTR, DWORD, IUnknown*, IUnknown**, IUnknown*,
                                             IUnknown*);
    HRESULT(STDMETHODCALLTYPE* end_stream)(MockResolver*, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* cancel)(MockResolver*, IUnknown*);
};

struct MockResolver {
    const MockVtable* vtable;
    std::atomic<ULONG> refs{1};
    int create_url_calls = 0;
    int begin_url_calls = 0;
    int create_stream_calls = 0;
    int end_url_calls = 0;
    int begin_stream_calls = 0;
    int end_stream_calls = 0;
    int cancel_calls = 0;
    std::wstring last_url;
};

HRESULT STDMETHODCALLTYPE mock_qi(MockResolver*, REFIID, void** out) {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE mock_add_ref(MockResolver* m) { return ++m->refs; }
ULONG STDMETHODCALLTYPE mock_release(MockResolver* m) { return --m->refs; }
HRESULT STDMETHODCALLTYPE mock_create_url(MockResolver* m, LPCWSTR url, DWORD, IUnknown*, DWORD* kind, IUnknown** obj) {
    ++m->create_url_calls;
    m->last_url = url ? url : L"";
    if (kind) *kind = 1;
    if (obj) *obj = reinterpret_cast<IUnknown*>(0x1234);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE mock_create_stream(MockResolver* m, IUnknown*, LPCWSTR url, DWORD, IUnknown*, DWORD*,
                                             IUnknown**) {
    ++m->create_stream_calls;
    m->last_url = url ? url : L"";
    return S_OK;
}
HRESULT STDMETHODCALLTYPE mock_begin_url(MockResolver* m, LPCWSTR url, DWORD, IUnknown*, IUnknown** cookie, IUnknown*,
                                         IUnknown*) {
    ++m->begin_url_calls;
    m->last_url = url ? url : L"";
    if (cookie) *cookie = reinterpret_cast<IUnknown*>(0x5678);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE mock_end_url(MockResolver* m, IUnknown*, DWORD*, IUnknown**) {
    ++m->end_url_calls;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE mock_begin_stream(MockResolver* m, IUnknown*, LPCWSTR, DWORD, IUnknown*, IUnknown**,
                                            IUnknown*, IUnknown*) {
    ++m->begin_stream_calls;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE mock_end_stream(MockResolver* m, IUnknown*, DWORD*, IUnknown**) {
    ++m->end_stream_calls;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE mock_cancel(MockResolver* m, IUnknown*) {
    ++m->cancel_calls;
    return S_OK;
}

const MockVtable kMockVtable = {
    &mock_qi,      &mock_add_ref,      &mock_release,    &mock_create_url, &mock_create_stream,
    &mock_begin_url, &mock_end_url,    &mock_begin_stream, &mock_end_stream, &mock_cancel,
};

struct ProxyVtable {
    HRESULT(STDMETHODCALLTYPE* qi)(void*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* add_ref)(void*);
    ULONG(STDMETHODCALLTYPE* release)(void*);
    HRESULT(STDMETHODCALLTYPE* create_url)(void*, LPCWSTR, DWORD, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* create_stream)(void*, IUnknown*, LPCWSTR, DWORD, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* begin_url)(void*, LPCWSTR, DWORD, IUnknown*, IUnknown**, IUnknown*, IUnknown*);
    HRESULT(STDMETHODCALLTYPE* end_url)(void*, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* begin_stream)(void*, IUnknown*, LPCWSTR, DWORD, IUnknown*, IUnknown**, IUnknown*,
                                             IUnknown*);
    HRESULT(STDMETHODCALLTYPE* end_stream)(void*, IUnknown*, DWORD*, IUnknown**);
    HRESULT(STDMETHODCALLTYPE* cancel)(void*, IUnknown*);
};

std::string read_file_text(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    // 1. Registry presence and partial-override ownership.
    check(rt::shims::find("mfplat.dll", "MFCreateSourceResolver") != nullptr,
          "MFCreateSourceResolver is registered in shim registry");
    check(!rt::shims::owns_dll("mfplat.dll"),
          "mfplat.dll stays partially overridden so native MF codecs remain available");

    // 2. Temp package + overlay directories for console path resolution checks.
    const auto base = std::filesystem::temp_directory_path() /
                      (L"leonida_mfplat_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    const auto game_dir = base / L"game";
    const auto overlay_dir = base / L"overlay";
    std::filesystem::create_directories(game_dir / L"movies", ec);
    std::filesystem::create_directories(overlay_dir / L"movies", ec);
    {
        std::ofstream(game_dir / L"movies" / L"intro.mp4", std::ios::binary) << "pkg-intro";
        std::ofstream(game_dir / L"movies" / L"intro a.mp4", std::ios::binary) << "pkg-space";
        std::ofstream(overlay_dir / L"movies" / L"patched.mp4", std::ios::binary) << "ovl-patched";
    }
    rt::fs::configure(game_dir, overlay_dir);

    // 3. Network schemes, UNC shares, and remote file:// hosts are refused with MF_E_UNSUPPORTED_SCHEME.
    std::wstring out;
    std::string scheme, host;
    check(rt::mf::resolve_media_url(nullptr, out) == E_POINTER, "null URL returns E_POINTER");

    check(rt::mf::resolve_media_url(L"https://cdn.example.invalid/a.mp4", out, &scheme, &host) ==
                  rt::mf::kUnsupportedScheme &&
              scheme == "https" && host == "cdn.example.invalid",
          "https URL refused with scheme and host");
    check(rt::mf::resolve_media_url(L"RTSP://192.0.2.1:554/stream", out, &scheme, &host) ==
                  rt::mf::kUnsupportedScheme &&
              scheme == "rtsp",
          "RTSP URL refused case-insensitively");
    check(rt::mf::resolve_media_url(L"mms://host.invalid/video", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "mms",
          "mms URL refused");
    check(rt::mf::resolve_media_url(L"\\\\server\\share\\a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "unc" && host == "server",
          "UNC backslash share refused");
    check(rt::mf::resolve_media_url(L"//server/share/a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "unc" && host == "server",
          "UNC forward-slash share refused");
    check(rt::mf::resolve_media_url(L"\\\\?\\UNC\\server\\share\\a.mp4", out, &scheme, &host) ==
                  rt::mf::kUnsupportedScheme &&
              scheme == "unc" && host == "server",
          "extended UNC share refused");
    check(rt::mf::resolve_media_url(L"file://server/share/a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "file" && host == "server",
          "remote file:// host refused");

    // 4. Local paths, local file:// URLs (with percent-decoding), and console drives resolve to host files.
    check(rt::mf::resolve_media_url(L"GAME:\\movies\\intro.mp4", out) == S_OK && read_file_text(out) == "pkg-intro",
          "GAME:\\ path resolves to package movie");
    check(rt::mf::resolve_media_url(L"G:\\movies\\intro.mp4", out) == S_OK && read_file_text(out) == "pkg-intro",
          "G:\\ path resolves to package movie");
    check(rt::mf::resolve_media_url(L"file:///G:/movies/intro%20a.mp4", out) == S_OK &&
              read_file_text(out) == "pkg-space",
          "file:///G:/ URL percent-decodes and resolves to package movie");
    check(rt::mf::resolve_media_url(L"file://localhost/GAME:/movies/patched.mp4", out) == S_OK &&
              read_file_text(out) == "ovl-patched",
          "file://localhost/GAME:/ prefers overlay copy");
    check(rt::mf::resolve_media_url(L"file:///C:/Windows/a.wav", out) == S_OK && out == L"C:\\Windows\\a.wav",
          "file:///C:/ normalizes to local drive path");
    check(FAILED(rt::mf::resolve_media_url(L"GAME:\\..\\outside.mp4", out)),
          "console path traversal outside GAME: is refused");

    // 4b. Default deny: anything but a console path, an absolute drive path, or a file URL naming one is refused.
    check(rt::mf::resolve_media_url(L"clip.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "relative",
          "relative local path refused");
    check(rt::mf::resolve_media_url(L"relative\\movie.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "relative",
          "relative backslash path refused");
    check(rt::mf::resolve_media_url(L"C:relative.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "relative",
          "drive-relative C:relative.mp4 refused");
    check(rt::mf::resolve_media_url(L"\\movies\\a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme,
          "rooted single-backslash path refused");
    check(rt::mf::resolve_media_url(L"/movies/a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme,
          "rooted single-slash path refused");
    check(rt::mf::resolve_media_url(L" http://example.com/a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme,
          "leading space before http scheme refused");
    check(rt::mf::resolve_media_url(L"\thttp://x/a", out, &scheme, &host) == rt::mf::kUnsupportedScheme,
          "leading tab before http scheme refused");
    check(rt::mf::resolve_media_url(L"\\??\\C:\\movies\\a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "unc",
          "NT \\??\\ prefix refused");
    check(rt::mf::resolve_media_url(L"\\\\.\\C:\\movies\\a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "unc",
          "device \\\\.\\ prefix refused");
    check(rt::mf::resolve_media_url(L"\\\\?\\C:movies\\a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme,
          "extended-prefix drive-relative path refused");
    check(rt::mf::resolve_media_url(L"file:///movies/a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme &&
              scheme == "file",
          "driveless file:/// URL refused");
    check(rt::mf::resolve_media_url(L"file:\\\\host\\share\\a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme,
          "file:\\\\host backslash UNC refused");
    check(rt::mf::resolve_media_url(L"file:////host/share/a.mp4", out, &scheme, &host) == rt::mf::kUnsupportedScheme,
          "file:////host forward-slash UNC refused");
    check(rt::mf::resolve_media_url(L"file:///%5C%5Chost/share/a.mp4", out, &scheme, &host) ==
                  rt::mf::kUnsupportedScheme,
          "file:/// percent-encoded UNC refused");
    check(rt::mf::resolve_media_url(L"file:///\\\\host\\share\\a.mp4", out, &scheme, &host) ==
                  rt::mf::kUnsupportedScheme,
          "file:/// with backslash UNC refused");

    // 5. COM proxy isolation and forwarding against mock IMFSourceResolver.
    MockResolver mock{};
    mock.vtable = &kMockVtable;
    void* proxy = nullptr;
    check(rt::mf::wrap_source_resolver(reinterpret_cast<IUnknown*>(&mock), &proxy) == S_OK && proxy != nullptr,
          "wrap_source_resolver wraps host resolver");
    if (proxy) {
        const auto* vt = *static_cast<const ProxyVtable* const*>(proxy);
        void* qi_out = nullptr;
        const GUID unknown_iid = {0x11111111u, 0x2222u, 0x3333u, {0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb}};
        check(vt->qi(proxy, rt::mf::kIidSourceResolver, &qi_out) == S_OK && qi_out == proxy,
              "QueryInterface answers IMFSourceResolver with proxy identity");
        if (qi_out) vt->release(qi_out);
        qi_out = reinterpret_cast<void*>(1);
        check(vt->qi(proxy, unknown_iid, &qi_out) == E_NOINTERFACE && qi_out == nullptr,
              "QueryInterface refuses unknown IID so raw host resolver cannot leak");

        // Blocked network URL must never reach mock host resolver and must null out pointer.
        DWORD kind = 99;
        IUnknown* created = reinterpret_cast<IUnknown*>(1);
        check(vt->create_url(proxy, L"https://example.invalid/v.mp4", 0, nullptr, &kind, &created) ==
                      rt::mf::kUnsupportedScheme &&
                  created == nullptr && mock.create_url_calls == 0,
              "CreateObjectFromURL blocks https before reaching host");

        IUnknown* cookie = reinterpret_cast<IUnknown*>(1);
        check(vt->begin_url(proxy, L"rtsp://example.invalid/v.mp4", 0, nullptr, &cookie, nullptr, nullptr) ==
                      rt::mf::kUnsupportedScheme &&
                  cookie == nullptr && mock.begin_url_calls == 0,
              "BeginCreateObjectFromURL blocks rtsp before reaching host");

        // Local console URL is rewritten before reaching mock host resolver.
        check(vt->create_url(proxy, L"GAME:\\movies\\intro.mp4", 0, nullptr, &kind, &created) == S_OK &&
                  mock.create_url_calls == 1 && read_file_text(mock.last_url) == "pkg-intro",
              "CreateObjectFromURL forwards rewritten GAME: path to host");

        check(vt->begin_url(proxy, L"file:///G:/movies/intro%20a.mp4", 0, nullptr, &cookie, nullptr, nullptr) == S_OK &&
                  mock.begin_url_calls == 1 && read_file_text(mock.last_url) == "pkg-space",
              "BeginCreateObjectFromURL forwards rewritten file:///G: path to host");

        // Remaining IMFSourceResolver methods forward directly.
        check(vt->create_stream(proxy, nullptr, L"hint.mp4", 0, nullptr, &kind, &created) == S_OK &&
                  mock.create_stream_calls == 1,
              "CreateObjectFromByteStream forwards to host");
        check(vt->end_url(proxy, nullptr, &kind, &created) == S_OK && mock.end_url_calls == 1,
              "EndCreateObjectFromURL forwards to host");
        check(vt->begin_stream(proxy, nullptr, L"hint.mp4", 0, nullptr, &cookie, nullptr, nullptr) == S_OK &&
                  mock.begin_stream_calls == 1,
              "BeginCreateObjectFromByteStream forwards to host");
        check(vt->end_stream(proxy, nullptr, &kind, &created) == S_OK && mock.end_stream_calls == 1,
              "EndCreateObjectFromByteStream forwards to host");
        check(vt->cancel(proxy, nullptr) == S_OK && mock.cancel_calls == 1,
              "CancelObjectCreation forwards to host");

        check(vt->release(proxy) == 0 && mock.refs.load() == 0,
              "Release frees proxy and releases wrapped host resolver");
    }

    std::filesystem::remove_all(base, ec);
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
