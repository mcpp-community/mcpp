// mcpp.platform.env on Linux: the process's own variable table.
module mcpp.platform.env;

import std;

// POSIX names it and no header must declare it; C linkage attaches it to the
// global module.
extern "C" char** environ;

char** mcpp::platform::env::environment() { return environ; }
