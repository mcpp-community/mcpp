// 演示 mcpp pack --mode static：musl 全静态二进制可分发到任意 Linux。
//
// 跑这个例子：
//   mcpp pack            ← 走 [pack] default_mode = "static"
//   tar -tzf target/dist/static-app-0.1.0-x86_64-linux-musl-static.tar.gz
//   # 解开后 ./static-app 在任意 Linux x86_64 直接跑
import std;

int main() {
    std::println("Hello from a fully-static, fully-portable mcpp binary!");
    std::println("This ELF has no PT_INTERP / RUNPATH / external lib deps.");
    std::println("Try: docker run --rm -v $PWD:/x alpine /x/static-app");
}
