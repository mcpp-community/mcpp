# `build.mcpp` —— 原生构建程序

[English](../07-build-mcpp.md) | **简体中文**

绝大多数工程只需要 `mcpp.toml`。当你需要构建期逻辑——探测主机、生成源码、依据环境
决定某个编译开关——就在工程根目录放一个 `build.mcpp`。它是 mcpp 版的 Zig `build.zig`
/ Cargo `build.rs`,但用 **C++** 编写:不引入第二种语言,而且 mcpp 自己吃自己的狗粮。

mcpp 用你的工具链编译 `build.mcpp`,并在主构建**之前**运行它。程序通过向 stdout 打印
`mcpp:` 指令与 mcpp 通信,这些指令会增补本次构建。

## 快速示例

```cpp
// build.mcpp
#include <cstdio>
#include <fstream>

int main() {
    // 生成一份源码,主构建会编译 + 链接它。
    std::ofstream("src/generated.cpp") << "const char* banner() { return \"hi\"; }\n";

    std::puts("mcpp:generated=src/generated.cpp");   // 加入构建
    std::puts("mcpp:cxxflag=-DHAVE_BANNER=1");        // 为所有 C++ TU 定义宏

    if (std::getenv("USE_FAST")) std::puts("mcpp:cxxflag=-DFAST_PATH=1");
    std::puts("mcpp:rerun-if-env-changed=USE_FAST");  // USE_FAST 变化时重跑我
    return 0;
}
```

```bash
mcpp build      # 编译 + 运行 build.mcpp,然后构建工程
```

## 指令

把这些打印到 stdout(每行一条)。任何不以 `mcpp:` 开头的行都会被忽略,因此你可以
自由打印诊断日志。

| 指令 | 作用 |
|---|---|
| `mcpp:cxxflag=<flag>`              | 给 C++ 编译追加 `<flag>` |
| `mcpp:cflag=<flag>`                | 给 C 编译追加 `<flag>` |
| `mcpp:link-lib=<name>`             | 链接 `-l<name>` |
| `mcpp:link-search=<dir>`           | 增加库搜索目录(`-L`;相对路径按工程根目录解析) |
| `mcpp:cfg=<name>`                  | 为 C 与 C++ 同时定义 `-D<name>` |
| `mcpp:generated=<path>`            | 把生成的源码(相对工程根目录)加入构建 |
| `mcpp:rerun-if-changed=<path>`     | 该文件变化时重跑 `build.mcpp` |
| `mcpp:rerun-if-env-changed=<VAR>`  | 该环境变量变化时重跑 `build.mcpp` |

程序**请求**构建边(开关、库、源码),它**不能**新增注册表依赖——请把依赖图保持在
`mcpp.toml` 里声明式管理(包括平台条件依赖 `[target.windows.dependencies]`)。
`build.mcpp` 用于*叶子*决策:开关、代码生成、链接需求。

## 类型化 API:`import mcpp;`(推荐)

除了打印裸字符串,你还可以把 `build.mcpp` 写成**模块优先**——`import mcpp;`,无
`#include`、无 `import std;`。`mcpp` 模块**内置在 mcpp 二进制里**(因此永远和你这版 mcpp
的协议匹配),按需编译;它的函数只是 emit 上面那些指令:

```cpp
// build.mcpp
import mcpp;

int main() {
    mcpp::cxxflag("-DHAVE_BANNER=1");
    mcpp::link_lib("m");                 // -lm
    mcpp::link_search("vendor/lib");     // -L…
    mcpp::define("HAVE_FEATURE");         // == mcpp:cfg= → -DHAVE_FEATURE
    mcpp::generated("src/gen.cpp");
    mcpp::rerun_if_changed("config.h");
    mcpp::rerun_if_env_changed("USE_FAST");
}
```

| 函数 | emit |
|---|---|
| `mcpp::cxxflag(s)` / `mcpp::cflag(s)` | `mcpp:cxxflag=` / `mcpp:cflag=` |
| `mcpp::link_lib(s)` / `mcpp::link_search(s)` | `mcpp:link-lib=` / `mcpp:link-search=` |
| `mcpp::define(s)` | `mcpp:cfg=`(即 `-D<s>`) |
| `mcpp::generated(p)` | `mcpp:generated=` |
| `mcpp::rerun_if_changed(p)` / `mcpp::rerun_if_env_changed(v)` | 对应的 `rerun-*` 指令 |

如果 `build.mcpp` 还需要*写*生成文件,混入一个文本 `#include <fstream>` 即可——这没问题,
只有 `import std;` 是不必要的。上面的裸 stdout 协议仍是底层基底;`import mcpp;` 是其上的
类型化层。

## 环境契约(mcpp 0.0.95+)

运行中的程序以 `MCPP_*` 环境变量得到构建上下文(对应 Cargo 的环境变量族),
也有类型化读取端:

| 变量 | 类型化读取 | 值 |
|---|---|---|
| `MCPP_TARGET` | `mcpp::target()` | 解析后的 canonical 三元组(交叉构建下是 `--target` 三元组,原生构建是宿主) |
| `MCPP_HOST` | `mcpp::host()` | 宿主三元组 |
| `MCPP_PROFILE` | `mcpp::profile()` | 生效 profile 名(`dev`/`release`/…) |
| `MCPP_OUT_DIR` | `mcpp::out_dir()` | mcpp 提供的可写输出/暂存目录 |
| `MCPP_MANIFEST_DIR` | `mcpp::manifest_dir()` | 包根(= CWD) |
| `MCPP_FEATURE_<NAME>` | `mcpp::has_feature("name")` | 每个活跃 feature 置 `1`(`<NAME>` 消毒规则与 `MCPP_FEATURE_` 编译宏一致) |
| `MCPP_FEATURES` | — | 活跃 feature 逗号列表 |

这些契约值**无条件**折入重跑键——换 target、换 profile、开关 feature 都会触发重跑,
不需要任何 `rerun-if-env-changed` 声明。

## 依赖包的 build.mcpp(mcpp 0.0.95+)

带 `build.mcpp` 的依赖包也会被编译并运行(Cargo `build.rs` 模型——构建一个包
即信任其构建程序),时机在其 feature 解析之后、源扫描之前。作用域照 Cargo:
`cxxflag`/`cflag`/`cfg` 指令只染色**该包自身的 TU**;`link-lib`/`link-search`
到达终链。其产物(二进制、缓存、`MCPP_OUT_DIR`)放在**消费方工程**的
`target/.build-mcpp/deps/<pkg>@<ver>/` 下——registry 包根跨工程共享(且可能只读),
绝不写入;相对 `generated=` 路径按 `MCPP_OUT_DIR` 解析,而非包根。

## 增量:声明输入(避免无谓重跑)

mcpp **不会**每次构建都重跑 `build.mcpp`。它会缓存程序产出的指令,只有当它依赖的东西
变化时才重跑:

- `build.mcpp` 源码本身,
- 工具链,
- 任何用 `rerun-if-changed` 声明的文件,
- 任何用 `rerun-if-env-changed` 声明的环境变量,
- (或某个 `generated` 产物丢失了)。

所以请**声明你的输入**:如果程序读了 `config.h` 或 `USE_FAST` 变量,就分别 emit
`mcpp:rerun-if-changed=config.h` / `mcpp:rerun-if-env-changed=USE_FAST`。这用一份明确的
输入/输出契约取代了过去「进程退出码为 0 就当成功」的猜测——让增量构建保持正确。

无变化时你会看到 `build.mcpp up to date (cached)`;否则是 `build.mcpp compiling` /
`running`。

## 说明与限制

- **在主机上运行——交叉构建下也是**(mcpp 0.0.95+)。`mcpp build --target <triple>`
  下,程序用宿主解析的工具链编译、在宿主运行,并看到 `MCPP_TARGET` = 交叉三元组。
  纯声明式的目标门控仍首选 `[target.'cfg(...)']` 表——参见
  [05 - mcpp.toml 工程文件指南](05-mcpp-toml.md)。
- **当前工作目录是工程根目录**,因此相对路径(`src/generated.cpp`)会落在你预期的位置。
- `build.mcpp` 非零退出会中止构建并打印其输出。
