// mcpp.build.command_line — the implementation, apart from the interface so
// that its instantiations are not carried by every module that imports it.
module mcpp.build.command_line;

import std;
import mcpplibs.os.platform;

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::build::command_line {

// THE ARGV RULES OF CommandLineToArgvW AND THE MSVC RUNTIME, NOT ONLY `\"`.
//
// Backslashes are literal unless a run of them precedes a `"`: then 2n
// backslashes stand for n and the quote ends (or begins) a quoted region,
// and 2n+1 stand for n and a literal quote. Escaping only the quote is wrong
// whenever backslashes precede it. The case that was met is an
// argument ENDING in a backslash: `"C:\Kits\10\"` closes nothing, because
// its last two characters are an escaped quote, and every following
// argument shifts. Measured 2026-10-05 in a Visual Studio developer
// environment, whose `WindowsSdkDir` ends in `\`: the LLVM row's std module
// precompile received `Files\Microsoft`, `Visual` and the rest of the
// command as separate inputs. Doubling such a run changes only arguments
// that were parsed wrongly before.
std::string quote_windows(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    std::size_t backslashes = 0;
    for (char c : s) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        out.push_back(c);
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::string paste_arg(std::string_view s) {
    constexpr bool is_windows = mcpplibs::os::is_windows;
    const bool plain = !s.empty() && std::ranges::all_of(s, [](unsigned char c) {
        return std::isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-' || c == ':'
            || c == '@' || c == '+' || c == '=' || c == ',' || c == '%' || c == '~'
            || (c == '\\' && is_windows) || c >= 0x80;
    });
    return plain ? std::string(s) : quote(s);
}

std::string quote_posix(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string quote_windows_through_cmd(std::string_view s) {
    const std::string inner = quote_windows(s);
    std::string out;
    out.reserve(inner.size() + 8);
    // cmd's quote state, which is toggled by EVERY `"` -- it does not know
    // MSVCRT's `\"`. Inside a quoted region a metacharacter is already inert
    // and `^` is a literal character, so only the characters that fall OUTSIDE
    // one are escaped. That keeps the result byte-identical to `quote_windows`
    // for every payload with nothing to escape, which is nearly all of them.
    bool inQuotes = false;
    for (char c : inner) {
        if (c == '"') { inQuotes = !inQuotes; out.push_back(c); continue; }
        if (!inQuotes) {
            switch (c) {
                case '<': case '>': case '&': case '|':
                case '^': case '(': case ')':
                    out.push_back('^');
                    break;
                default:
                    break;
            }
        }
        out.push_back(c);
    }
    return out;
}

std::string quote(std::string_view s) {
    if constexpr (mcpplibs::os::is_windows) return quote_windows(s);
    else return quote_posix(s);
}

std::string quote_through_shell(std::string_view s) {
    if constexpr (mcpplibs::os::is_windows) return quote_windows_through_cmd(s);
    else return quote_posix(s);
}


std::string display(const std::vector<std::string>& argv) {
    std::string out;
    for (auto const& a : argv) {
        if (!out.empty()) out += ' ';
        const bool plain = !a.empty() && std::ranges::all_of(a, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c))
                || std::string_view("_-./:=+@%,").contains(c)
                || (mcpplibs::os::is_windows && c == '\\');
        });
        out += plain ? a : quote(a);
    }
    return out;
}

std::vector<std::string> windows_shell_arguments(std::string_view command) {
    return { "/d", "/s", "/c", "\"" + std::string(command) + "\"" };
}

std::string windows_shell_command_line(std::string_view command) {
    std::string line = "cmd.exe";
    for (auto const& a : windows_shell_arguments(command)) line += " " + a;
    return line;
}

std::string clean_ld_library_path_prefix(const std::vector<std::filesystem::path>& dirs) {
    if constexpr (mcpplibs::os::is_linux) {
        if (dirs.empty()) return "";
        std::string joined;
        for (auto& d : dirs) {
            if (!joined.empty()) joined += ':';
            joined += d.string();
        }
        return std::format("env LD_LIBRARY_PATH={} ", quote(joined));
    } else {
        (void)dirs;
        return "";
    }
}

} // namespace mcpp::build::command_line
