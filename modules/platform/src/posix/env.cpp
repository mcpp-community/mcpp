// mcpp.platform.env on Linux and macOS: writing the process's own variables.
module;
#include <stdlib.h>

module mcpp.platform.env;

import std;

void mcpp::platform::env::detail::put(const std::string& key, const char* value) {
    if (value) ::setenv(key.c_str(), value, 1);
    else ::unsetenv(key.c_str());
}
