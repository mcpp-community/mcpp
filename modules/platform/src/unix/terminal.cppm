// POSIX terminal bindings. The portable terminal module selects the platform.
module;
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

export module mcpp.platform.unix.terminal;

export namespace mcpp::platform::unix_terminal {
// No std types cross this widely imported backend boundary (GCC 16).
bool input_is_terminal();
}

namespace mcpp::platform::unix_terminal {
bool input_is_terminal() {
#if defined(__unix__) || defined(__APPLE__)
    return ::isatty(STDIN_FILENO) != 0;
#else
    return false;
#endif
}
}
