// mcpp.platform.windows — Windows-specific platform capabilities.
//
// Provides:
//   prepend_path()      — add a directory to the front of %PATH%
//   active_code_page()  — the process ANSI code page (GetACP)
//
// Note: Visual Studio / MSVC discovery is in mcpp.toolchain.msvc, which is
// the authoritative module for MSVC toolchain detection.  This module
// provides general-purpose Windows platform utilities only.

export module mcpp.platform.windows;

import std;

export namespace mcpp::platform::windows {

// Prepend a directory to the %PATH% environment variable.
void prepend_path(const std::filesystem::path& dir);

// The ANSI code page of this process: 65001 when the UTF-8 code page that
// mcpp.exe declares is in effect (Windows 10 version 1903 and later), the
// system's legacy code page otherwise. 0 on every other platform, which has
// no process code page. Every narrow string mcpp exchanges with Win32 is in
// this code page (#693).
unsigned active_code_page();

// A string value under HKEY_LOCAL_MACHINE, read from both registry views (the
// 64-bit one first, then WOW6432Node's), as a path. An installer records its
// installation directory there wherever it put it -- the Windows SDK's
// `Installed Roots\KitsRoot10` -- so a search that knows only `C:\Program
// Files` misses an SDK on another drive (D15). nullopt when absent and on
// every other platform.
std::optional<std::filesystem::path> machine_registry_path(std::wstring_view subkey,
                                                           std::wstring_view value);

// A directory a known-folder environment variable names (`ProgramFiles`,
// `ProgramFiles(x86)`), or nullopt. The system drive is not always `C:`.
std::optional<std::filesystem::path> env_directory(const char* name);

// Whether the system lets a program that declares itself `longPathAware` use
// paths past MAX_PATH (260): HKLM\SYSTEM\CurrentControlSet\Control\FileSystem
// `LongPathsEnabled` = 1. false when it is 0 or absent, and on every other
// platform, which has no such limit.
bool long_paths_enabled();
// Used only by the first-run Windows home suggestion; never creates the
// actual home. An existing child must be a plain directory: inspecting its
// reparse-point attributes avoids following a potentially remote target.
// Other platforms answer false without touching a filesystem.
bool writable_fixed_drive(char drive, std::wstring_view child);

} // namespace mcpp::platform::windows
