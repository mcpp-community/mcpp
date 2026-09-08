// toyc -- the driver.
//
//   toyc <input.toy> -o <output.cpp>
//
// Exit 0 and write the output, or exit 1 and print one diagnostic in the
// `file:line:col: error: message` form every editor already parses.
import std;
import example.toyc.compile;

int main(int argc, char** argv) {
    std::string in, out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc)      out = argv[++i];
        else if (a.starts_with("-o"))       out = a.substr(2);
        else if (a.starts_with("-"))      { std::println(std::cerr, "toyc: unknown option `{}`", a); return 2; }
        else if (in.empty())                in = a;
        else                              { std::println(std::cerr, "toyc: more than one input file"); return 2; }
    }
    if (in.empty() || out.empty()) {
        std::println(std::cerr, "usage: toyc <input.toy> -o <output.cpp>");
        return 2;
    }

    std::ifstream file(in, std::ios::binary);
    if (!file) {
        std::println(std::cerr, "toyc: cannot read {}", in);
        return 1;
    }
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

    const auto cpp = toyc::compile(source, std::filesystem::path(in).filename().string());
    if (!cpp) {
        std::println(std::cerr, "{}:{}:{}: error: {}",
                     in, cpp.error().line, cpp.error().col, cpp.error().message);
        return 1;
    }

    // The output directory is created by whoever declared the action; a
    // compiler that creates it silently hides a rule that got out_dir wrong.
    std::ofstream sink(out, std::ios::binary);
    if (!sink) {
        std::println(std::cerr, "toyc: cannot write {}", out);
        return 1;
    }
    sink << *cpp;
    return sink ? 0 : 1;
}
