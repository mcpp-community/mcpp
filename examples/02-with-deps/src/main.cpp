// 演示从 mcpp-index 拉一个外部依赖（mcpplibs.cmdline）并使用它。
//
// 跑这个例子：
//   mcpp build         ← 第一次会自动下载 mcpplibs.cmdline 并编译
//   mcpp run -- --name "C++23 modules"
import std;
import mcpplibs.cmdline;

namespace cl = mcpplibs::cmdline;

int main(int argc, char** argv) {
    auto app = cl::App("greet")
        .description("Say hello to someone with mcpp + mcpplibs.cmdline")
        .option(cl::Option("name").short_name('n').takes_value()
            .help("Name to greet (default: World)"));

    auto parsed = app.parse(argc, argv);
    if (!parsed) {
        std::println("error: {}", parsed.error().message);
        return 2;
    }

    std::string name = parsed->value("name").value_or("World");
    std::println("Hello, {}!", name);
}
