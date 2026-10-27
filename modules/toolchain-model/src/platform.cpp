// mcpp.toolchain.platform — the facts of the host that are read at run time.
module mcpp.toolchain.platform;

import std;
import mcpp.invocation;
import mcpplibs.os.platform;

namespace mcpp::toolchain::platform {

bool is_android_host() {
    if constexpr (mcpplibs::os::is_linux) {
        static const bool android = [] {
            if (mcpp::invocation::get("ANDROID_ROOT") && mcpp::invocation::get("ANDROID_DATA")) return true;
            std::error_code ec;
            return std::filesystem::exists("/system/bin/linker64", ec)
                || std::filesystem::exists("/system/bin/linker", ec);
        }();
        return android;
    } else {
        return false;
    }
}

}  // namespace mcpp::toolchain::platform
