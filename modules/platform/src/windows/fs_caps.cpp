// mcpp.platform.fs_caps on Windows: a directory's owner from its security
// descriptor, and the volume's filesystem name.
module;
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32")

module mcpp.platform.fs_caps;

import std;

namespace mcpp::platform::native {

// Windows has no atomic exchange of two names.
int exchange(const std::filesystem::path&, const std::filesystem::path&) { return ERROR_NOT_SUPPORTED; }
bool unsupported(int error) { return error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION; }
std::error_category const& category() { return std::system_category(); }
std::string owner_problem(const std::filesystem::path& dir) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(dir.wstring().c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS)
        return "its owner cannot be read";
    bool trusted = ::IsWellKnownSid(owner, WinLocalSystemSid) || ::IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
    HANDLE token = nullptr;
    if (!trusted && ::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> buffer(size);
        if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
            trusted = ::EqualSid(owner, reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
        ::CloseHandle(token);
    }
    ::LocalFree(sd);
    return trusted ? std::string{} : "it belongs to another user";
}
std::string filesystem_type(const std::filesystem::path& dir) {
    wchar_t root[MAX_PATH] {}, name[MAX_PATH] {};
    if (!::GetVolumePathNameW(dir.wstring().c_str(), root, MAX_PATH)) return {};
    if (!::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, name, MAX_PATH)) return {};
    return std::filesystem::path(name).string();
}

} // namespace mcpp::platform::native
