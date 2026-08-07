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
| `mcpp:generated=<path>`            | 把生成的源码加入构建。**相对路径在根工程按工程根解析,在依赖的 build.mcpp 里按 `MCPP_OUT_DIR` 解析** —— 两种角色都可能出现的包应发绝对路径(见下文) |
| `mcpp:source=<path>` *(0.0.100+)*  | 把一份**既有**源文件选入构建(绝对路径,或相对包根)。下游效果与 `generated=` 相同;语义区别在于文件是程序*选中*的(tarball payload / vendored 源树)而非程序写出的——例如对大型源码包做 per-target 源选择 |
| `mcpp:include-dir=<dir>` *(0.0.100+)* | 为本包自身 TU 增加一个**私有** include 目录(`-I`;绝对路径或相对包根,自动规范化)。取代过去 `cxxflag=-I` + `cflag=-I` 的双重裸发 |
| `mcpp:include-dir-after=<dir>` *(0.0.100+)* | 同 `include-dir`,但排在系统目录**之后**搜索(`-idirafter`)——用于会遮蔽系统头的 payload 源树 |
| `mcpp:rerun-if-changed=<path>`     | 该文件变化时重跑 `build.mcpp` |
| `mcpp:rerun-if-env-changed=<VAR>`  | 该环境变量变化时重跑 `build.mcpp` |

程序**请求**构建边(开关、库、源码),它**不能**新增注册表依赖——请把依赖图保持在
`mcpp.toml` 里声明式管理(包括平台条件依赖 `[target.windows.dependencies]`)。
`build.mcpp` 用于*叶子*决策:开关、代码生成、链接需求。

`include-dir`/`include-dir-after` 刻意保持**私有**(Cargo 纪律):只染色本包自身的
TU,绝不向消费者传播。需要消费者可见的 include 目录属于公共接口,应写在声明式
manifest/描述符里(`[build] include_dirs`),而不是构建期程序里。

## 类型化 API:`import mcpp;`(推荐)

除了打印裸字符串,你还可以把 `build.mcpp` 写成**模块优先**——`import mcpp;`,不需要
`#include`。`mcpp` 模块**内置在 mcpp 二进制里**(因此永远和你这版 mcpp
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
| `mcpp::source(p)` | `mcpp:source=` |
| `mcpp::include_dir(d)` / `mcpp::include_dir_after(d)` | `mcpp:include-dir=` / `mcpp:include-dir-after=` |
| `mcpp::rerun_if_changed(p)` / `mcpp::rerun_if_env_changed(v)` | 对应的 `rerun-*` 指令 |
| `mcpp::rerun_if_changed_glob(pat)` *(2026.8.6.2+)* | `mcpp:rerun-if-changed-glob=` —— 匹配 `pat` 的文件**集合**发生变化时重跑(见下) |
| `mcpp::dep_bin(pkg, tool)` *(2026.8.5.1+)* | 读 `MCPP_DEP_<PKG>_BIN_<TOOL>` —— 依赖构建出的 **host 工具**的绝对路径(见下) |
| `mcpp::action{…}.submit()` *(2026.8.5.1+)* | `mcpp:action=` —— **声明一个构建图节点**,而不是在这里把活干了(见下) |

### 依赖产出的 host 工具(2026.8.5.1+)

在 `mcpp.toml` 里声明需求,然后调用它:

```toml
[dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

```cpp
// build.mcpp
import mcpp;
int main() {
    const char* protoc = mcpp::dep_bin("protobuf", "protoc");
    // … 调用它,然后声明它产出了什么 …
}
```

mcpp 会**为构建机器**构建那个 `kind = "bin"` target(即使在 `--target` 下),
全局缓存,并把路径交给你。这个请求写在 `mcpp.toml` 而不是这里,理由和依赖本身
一样:向依赖图索取一个额外产物是**图级别**的请求,而图必须保持可静态分析。
完整契约(含 `[tools.overrides]` 与 `reexport = true` —— 库据此把整条工具链交给
你,于是你只写**一条**依赖而不是四条)见 [05 §2.14](05-mcpp-toml.md)。

### 用通配符声明输入:`rerun_if_changed_glob`(2026.8.6.2+)

重跑键由**声明过的**输入构成。声明具体文件是可行的;而 glob 一个目录不可行 ——
新增一个 `.proto` 不改变任何已声明文件的哈希,于是程序不重跑,新文件静默地永远
不被生成。`rerun_if_changed_glob` 就是程序用来说「我的输出取决于这里有哪些文件」
的方式:

```cpp
import mcpp;
int main() {
    mcpp::rerun_if_changed_glob("proto/**/*.proto");
    // … 扫描目录,为每个文件声明一条 action …
}
```

模式相对 manifest 目录,`*` / `**` 的文法与 `sources = [...]` 完全一致。它的指纹
是**排序后的匹配路径集合**,不含其他任何东西:

- **不含内容** —— 字节内容重要的文件本来就该用 `rerun_if_changed` 声明,那条
  条目已经在哈希它;
- **不含 mtime 与 size** —— mtime 在 `git checkout`、容器构建、`rsync` 下都不
  稳定,而 size 是比上面那个哈希更弱的信号。

构建输出目录与 `.git` 永远不进入集合,因此再宽的模式也不会让程序对着自己的产物
无限重跑。

### 声明工作而不是干活:`mcpp::action`(2026.8.5.1+)

在**这里**直接把源码写出来是省事的路,超过一定规模就是错的:它每次 prepare 跑
一遍、全量、串行,失败还只报「build.mcpp exited 1」。**声明**这份工作,它就成为
构建图里的一条边 —— 增量、并行,失败能归因到具体那条边。

```cpp
import mcpp;
int main() {
    const std::string out = std::string(mcpp::out_dir()) + "/foo.pb.cc";
    mcpp::action a;
    a.id = "protoc:foo";
    a.role = "source";              // "source" | "check" | "object" | "artifact"
    a.arg(mcpp::dep_bin("protobuf", "protoc"))
     .arg("--cpp_out=...").arg("proto/foo.proto")
     .input("proto/foo.proto")
     .output(out.c_str())
     .submit();
}
```

四种 role,一个原语 —— `role` 只决定这条边的输出接到哪:

| `role` | 输出 | 顺序 | 典型 |
|---|---|---|---|
| `source` | 进编译集 | 编译边消费它们 | protoc、转译器 |
| `check` | 一个 stamp 文件 | **与编译并行**(`blocking = true` 才前置) | clang-tidy、格式/ABI 检查 |
| `object` | 进**链接**集 | 链接边消费它们 | 资源编译器、`objcopy` 嵌 blob、生成的 `.def`、预编译 `.o` |
| `artifact` | 一个新文件 | 它的**输入**是链接产物,所以在链接之后跑 | 签名、打包、size budget |

全程不涉及任何 phase 机制:顺序由 ninja 自己的文件依赖决定 —— 这也是为什么
`artifact` 不会像朴素的「post 构建钩子」那样把自己重复施加一遍。

`object`(2026.8.7.1+)可选 `.target("name")`,可重复。它之所以需要名字:与
`artifact` 不同,它跑在链接**之前**,没有 `${mcpp.target_file:…}` 可以反推。
**每一个**匹配不到链接单元的名字都是错误 —— 包括写在一个匹配得上的名字旁边的那个,
那正是拼错真实的样子。

**优先省略它。** 不写 target 时,产物接到本次构建里该包产出的每个镜像 —— 可执行、
动态库,**以及测试二进制**。测试二进制在这个集合里,是因为它链接的是同一份库代码:
把它排除掉,`mcpp build` 会通过而 `mcpp test` 在这个 action 本来要提供的那个符号上
报 `undefined symbol`。改成显式点名也不行 —— 测试链接单元是从 `tests/*.cpp`
**发现**出来的,名字不在 `mcpp.toml` 里,而写了它的 `build.mcpp` 在普通
`mcpp build` 下会直接构建失败,因为那条链接单元根本不存在。

如果本次构建里没有任何东西能接收这些产物(纯静态库包),mcpp 会报一条 degradation:
这条边只能经由链接被达成,没有链接就意味着命令一次都不会跑,而构建什么都不说。

> 把预编译对象写进 `[build].ldflags` 同样能到达链接器,但**不要**用它承载构建产物:
> ldflags 是链接命令里的一串字符、不是图里的文件,没有任何东西跟踪它,改了它得到的是
> `ninja: no work to do`。Windows 资源请用 [`[resources]`](05-mcpp-toml.md);
> `object` 是其余一切的出口。

**必须写出输出文件名。** mcpp 在 prepare 期就定死源码集、fingerprint 与模块图,
所以名字未知的产物无法构建。内容可以晚到,名字不行。畸形 action 是**硬错误**,
绝不静默跳过。

生成**模块接口**时,把它的接口也声明出来:

```cpp
a.output(gen.c_str()).provides("my.generated").imports("std").submit();
```

mcpp 会播下一个带着该声明的占位文件,使 prepare 期的扫描与你的生成器将要产出的
内容一致 —— 与 `[modules].scan_overrides` 同一条「声明 + 验证」的取舍,build 期由
编译器自己的 P1689 输出复核。

命令是 **argv 而不是 shell 字符串**(不假设存在 shell —— Windows 没有能依赖的那个),
插值只有封闭的一组:

| 变量 | 含义 |
|---|---|
| `${mcpp.out_dir}` | 构建输出目录 |
| `${mcpp.bin_dir}` | 产出的二进制所在目录 |
| `${mcpp.compile_db}` | `compile_commands.json` 的路径(clang-tidy 的 `-p` 要的就是它) |
| `${mcpp.target_file:<name>}` | target `<name>` 构建出的文件 |

上面的裸 stdout 协议仍是底层基底;`import mcpp;` 是其上的类型化层。

### `import mcpp;` 才是会演进的那一面(mcpp 2026.8.5.1+)

和 mcpp 对话有两条路,它们的**兼容性承诺不同**:

| | `import mcpp;` | 手写 `printf("mcpp:…")` |
|---|---|---|
| 兼容性 | 该模块**内置在 mcpp 二进制里**,由运行它的那个 mcpp 现场编译,程序与引擎不可能不一致 | 你的字符串是冻结的文本,没有任何东西替你校验 |
| 新指令 | 以新函数的形式到来 | **不会再新增** |
| 未知指令 | **硬错误** | 警告后忽略 |

用 `import mcpp;` 的程序会自动声明它编译时对应的协议版本(`mcpp:protocol=<N>`,
在 `main` 之前发出——你不需要自己写)。mcpp 用它做两件事:

- 程序声明的协议**高于** mcpp 所理解的 → **拒绝执行**,并给出升级提示。继续跑会
  静默丢掉构建依赖的指令,而「构建成功了但那个 flag 根本没到」是最难查的一类问题。
- 既然双方已被证明一致,**未知指令就是错误**而不是警告:在同一个协议版本内,
  它只可能是拼写错误。

`printf` 风格的程序什么都不声明,因此保留历史上的「警告并忽略」行为。这一面
**冻结在上表的 11 条指令**上——它仍然能用、也会继续能用,但新能力只在类型化 API 里
落地。**要长期维护的程序请用 `import mcpp;`。**

### `import std;`(mcpp 2026.8.2.1+)

`build.mcpp` 可以 `import std;`(以及 `import std.compat;`),单用或与
`import mcpp;` 并用皆可:

```cpp
// build.mcpp
import std;
import mcpp;

int main() {
    for (auto const& f : std::vector<std::string>{"FOO", "BAR"})
        mcpp::define(f.c_str());
}
```

mcpp 会把它自己构建时用的**同一份** std 模块暂存过来,缓存键是
(工具链 × 标准 × 方言)——所以普通构建下这是零成本,产物本来就在。只有交叉构建
(`--target …`)才会多编一份:`build.mcpp` 在**宿主**上编译并运行,而工程的目标
是别的平台。

`#include` 依然有效,对只需要 `std::fopen` 的程序也依然是更合适的选择——构建脚本
没有必须模块化的要求。

凡是 mcpp 能用来构建宿主程序的工具链,都能构建 `build.mcpp`,原生 MSVC 也不例外
——模块处理读的是主构建同一批表,所以 `cl.exe` 的 `.ifc` + `/reference` 不需要
单独支持。

## 环境契约(mcpp 0.0.95+)

运行中的程序以 `MCPP_*` 环境变量得到构建上下文(对应 Cargo 的环境变量族),
也有类型化读取端:

| 变量 | 类型化读取 | 值 |
|---|---|---|
| `MCPP_TARGET` | `mcpp::target()` | 解析后的 canonical 三元组(交叉构建下是 `--target` 三元组,原生构建是宿主) |
| `MCPP_TARGET_OS` *(0.0.100+)* | `mcpp::target_os()` | 目标的 OS 段(`linux`/`macos`/`windows`)——不必再手撕 `MCPP_TARGET` |
| `MCPP_TARGET_ARCH` *(0.0.100+)* | `mcpp::target_arch()` | 目标的 arch 段(GNU 拼写:`x86_64`、`aarch64`…) |
| `MCPP_TARGET_ENV` *(0.0.100+)* | `mcpp::target_env()` | 目标的 env 段(`gnu`/`musl`/`msvc`);三元组无 env 段(macOS)时为空串 |
| `MCPP_HOST` | `mcpp::host()` | 宿主三元组 |
| `MCPP_PROFILE` | `mcpp::profile()` | 生效 profile 名(`dev`/`release`/…) |
| `MCPP_OUT_DIR` | `mcpp::out_dir()` | mcpp 提供的可写输出/暂存目录 |
| `MCPP_MANIFEST_DIR` | `mcpp::manifest_dir()` | 包根(= CWD) |
| `MCPP_FEATURE_<NAME>` | `mcpp::has_feature("name")` | 每个活跃 feature 置 `1`(`<NAME>` 消毒规则与 `MCPP_FEATURE_` 编译宏一致) |
| `MCPP_FEATURES` | — | 活跃 feature 逗号列表 |
| `MCPP_DEP_<NAME>_DIR` | `mcpp::dep_dir("name")` | 每个已声明依赖解析后的安装目录(canonical 名与去命名空间短名两种拼写都可用;`<NAME>` 消毒规则同 `MCPP_FEATURE_`)。依赖包的 build.mcpp **和**根工程的 build.mcpp 都能拿到(根工程的 build.mcpp 在依赖解析之后运行,0.0.100+) |

这些契约值**无条件**折入重跑键——换 target、换 profile、开关 feature 都会触发重跑,
不需要任何 `rerun-if-env-changed` 声明。

## 依赖包的 build.mcpp(mcpp 0.0.95+)

带 `build.mcpp` 的依赖包也会被编译并运行(Cargo `build.rs` 模型——构建一个包
即信任其构建程序),时机在其 feature 解析之后、源扫描之前。作用域照 Cargo:
`cxxflag`/`cflag`/`cfg` 指令只染色**该包自身的 TU**;`link-lib`/`link-search`
到达终链。其产物(二进制、缓存、`MCPP_OUT_DIR`)放在**消费方工程**的
`target/.build-mcpp/deps/<pkg>@<ver>/` 下——registry 包根跨工程共享(且可能只读),
绝不写入;相对 `generated=` 路径按 `MCPP_OUT_DIR` 解析,而非包根。

### 既独立构建又被当依赖的库:发绝对路径

上面这两条规则——根工程按工程根、依赖按 `MCPP_OUT_DIR`——意味着**相对**
`generated=` 不可能两种角色都对。而一个库正好两种角色都有:自己的 CI 独立构建它,
别人从 registry 当依赖用它。

写进 `MCPP_OUT_DIR` 再发裸文件名,在依赖角色下能用,在根工程下则失败:

```
error: build.mcpp declared generated source 'foo.cppm' but it does not exist after the run
```

正确做法是写进 `MCPP_OUT_DIR`(包根可能只读),并发**绝对**路径:

```cpp
const auto out = std::filesystem::path(mcpp::out_dir()) / "foo.cppm";
// ... 写文件 ...
mcpp::generated(out.string().c_str());
```

`mcpp::out_dir()` 恒为绝对路径,因此两种角色下都正确,不需要判断自己处在哪一种。

生成**模块接口**是可以的:`.cppm` 走与其他源文件相同的扫描,所以一个生成出来的、
声明 `export module …` 的文件可以被该包自己的 TU import。

## 增量:声明输入(避免无谓重跑)

mcpp **不会**每次构建都重跑 `build.mcpp`。它会缓存程序产出的指令,只有当它依赖的东西
变化时才重跑:

- `build.mcpp` 源码本身,
- 工具链,
- 任何用 `rerun-if-changed` 声明的文件,
- 任何用 `rerun-if-env-changed` 声明的环境变量,
- (或某个 `generated` 产物 / `source=` 选中的文件丢失了),
- (或该缓存是由一个对某条指令解释不同的 mcpp 写下的——条目带一个格式 **epoch**,
  遇到不认识的 epoch 就重跑一次,而不是把值按错误的含义重放)。

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
- **运行有时间上限**(mcpp 2026.8.5.1+,**仅 POSIX**):构建程序默认有 **600 秒**,
  超时后 mcpp 杀掉它并让构建失败,错误里会点名是哪个包。用
  `MCPP_BUILD_PROGRAM_TIMEOUT=<秒>` 覆盖(`0` = 不限)。**Windows 上这个上限不生效**
  —— 进程启动器还没有 kill-by-handle 的路径(`mcpp.platform.process`),所以在那里
  卡死的构建程序仍会把构建挂住。与 `mcpp test --timeout` 是同一条限制;明说,而不是
  含糊过去。**编译**这一步刻意**不设**上限——与 `mcpp test` 同一条不对称纪律:
  编译跑得久通常是正当的(首次构建 `std` 模块就是分钟级),杀掉它只会产生莫名其妙的
  失败;而构建**程序**跑得久通常是卡住了,不设上限就会让整个构建挂死且毫无诊断。
