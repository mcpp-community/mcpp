// mcpp.platform.env on macOS: the process's own variable table, by the call
// that answers for a dynamic library as well as for the program.
module;
#include <crt_externs.h>

module mcpp.platform.env;

import std;

char** mcpp::platform::env::environment() { return *::_NSGetEnviron(); }
