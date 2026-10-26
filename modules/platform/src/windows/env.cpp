// mcpp.platform.env on Windows: the C runtime's variable table, written with
// `_putenv_s', for which an empty value is removal.
module;
#include <stdlib.h>

module mcpp.platform.env;

import std;

char** mcpp::platform::env::environment() { return _environ; }

void mcpp::platform::env::detail::put(const std::string& key, const char* value) {
    ::_putenv_s(key.c_str(), value ? value : "");
}
