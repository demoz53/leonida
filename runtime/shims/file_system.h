// Console (GDK) file paths for a mapped title.
//
// A title's file API sees the console's drives: "GAME:\\data\\shaders\\x.sps" is a file inside the installed game
// folder. ivall/xcl (GPL-2.0-only; facts only here) documents for Xbox Series GDK titles that GAME: names the game
// folder and that writes go to a writable overlay rather than to the package, which is read-only. gta6rt does the
// same:
//   GAME:<rest>    a read opens the overlay copy when one exists, else <game dir>\<rest>;
//                  an open that can write opens <overlay dir>\<rest>, copying the package file there first when the
//                  disposition keeps its contents, so the package is never modified;
//                  a delete leaves a marker in the overlay instead of removing a package file.
//   anything else  is a host path and is passed through unchanged.
// A GAME: path that climbs above the root with ".." or names another drive is refused with ERROR_INVALID_NAME.
//
// The shims that use this live in file_system.cpp: CreateFile*, FindFirstFile*/FindNextFile*/FindClose,
// GetFileAttributes*/GetFileAttributesEx*, DeleteFile*, CreateDirectory*/RemoveDirectory*, MoveFile*/MoveFileEx*,
// CopyFile*/CopyFileEx*/CopyFile2 and SetFileAttributes* (the rules for each are in docs/runtime_imports.md).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rt::fs {

inline constexpr wchar_t kPersistentStorageDir[] = L".gta6rt-persistent";

// Sets the roots. main.cpp calls this once, before anything is mapped; the test calls it per case. The overlay is
// created on the first write, not here.
void configure(const std::filesystem::path& game_dir, const std::filesystem::path& overlay_dir);

std::filesystem::path game_dir();
std::filesystem::path overlay_dir();

// Resolves a console path (GAME:, G:, T:) for read access using the same overlay, package, and deletion-marker rules
// as CreateFileW, or returns a host path unchanged. Returns false and writes the Win32 error code when the path is
// refused (e.g. traversal outside the console root or a deleted package file).
bool resolve_read_path(const std::wstring& path, std::wstring& out, std::uint32_t* error = nullptr);

// For tests: true makes the CopyFile2 shim use its CopyFileExW fallback even where the host has CopyFile2, so the
// path Wine 9 takes can be checked on Windows. Off by default; main.cpp never sets it.
void force_copy_file2_fallback(bool force);

}  // namespace rt::fs
