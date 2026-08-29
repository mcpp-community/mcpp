# `build.mcpp` —— 原生构建程序

[English](../07-build-mcpp.md) | **简体中文**

绝大多数工程只需要 `mcpp.toml`。需要构建期逻辑时 —— 探测主机、生成源码、依据环境
决定某个编译开关——就在工程根目录放一个 `build.mcpp`。它是 mcpp 版的 Zig `build.zig`
/ Cargo `build.rs`,但用 **C++** 编写:不引入第二种语言,而且 mcpp 自己吃自己的狗粮。

mcpp 用当前工具链编译 `build.mcpp`,并在主构建**之前**运行它。程序通过向 stdout 打印
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

把这些打印到 stdout(每行一条)。任何不以 `mcpp:` 开头的行都会被忽略,因此可以
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
| `mcpp:runner=<token>` *(2026.8.19.2+)* | 执行本次构建产物的命令的**一个 argv token**(宿主跑不了它时)。一个 token 一次调用、按顺序;产物路径会被追加(或替换 `{}`)。**到达消费者**。⚠️ 可执行文件要发**绝对路径**,且**只能有一个**依赖提供它 |
| `mcpp:link-script=<path>` *(2026.8.19+)* | 用这个**链接脚本**链接(`-T`;相对路径按包根解析,发出的是绝对路径,因为链接是在构建目录里跑的)。与 `include-dir` 不同,它**到达消费者** —— 板子的内存布局恰恰是消费者写不出来的那一项 |
| `mcpp:warning=<text>` *(2026.8.21.2+)* | 对用户说一句话并**继续**。唯一一条不改变编译行、链接行与源码集的指令。它**穿过构建缓存** —— 见下 |
| `mcpp:rerun-if-changed=<path>`     | 该文件变化时重跑 `build.mcpp` |
| `mcpp:rerun-if-env-changed=<VAR>`  | 该环境变量变化时重跑 `build.mcpp` |

程序**请求**构建边(开关、库、源码),它**不能**新增注册表依赖——请把依赖图保持在
`mcpp.toml` 里声明式管理(包括平台条件依赖 `[target.windows.dependencies]`)。
`build.mcpp` 用于*叶子*决策:开关、代码生成、链接需求。

`include-dir`/`include-dir-after` 刻意保持**私有**(Cargo 纪律):只染色本包自身的
TU,绝不向消费者传播。需要消费者可见的 include 目录属于公共接口,应写在声明式
manifest/描述符里(`[build] include_dirs`),而不是构建期程序里。

## 类型化 API:`import mcpp;`(推荐)

除打印裸字符串外,`build.mcpp` 也可以写成**模块优先**形式——`import mcpp;`,不需要
`#include`。`mcpp` 模块**内置在 mcpp 二进制里**(因此始终与当前这版 mcpp
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
| `mcpp::link_script(p)` *(2026.8.19+)* | `mcpp:link-script=` |
| `mcpp::runner(tok)` *(2026.8.19.2+)* | `mcpp:runner=` —— 见下 |
| `mcpp::xpkg_dir(ns, name)` / `mcpp::xpkg_dir(name)` *(2026.8.19+)* | 本 manifest 在 `[xlings] deps` 里声明的包的载荷目录;没声明或没安装时返回 `""`(见下) |
| `mcpp::warning(text)` *(2026.8.21.2+)* | `mcpp:warning=` —— 见下 |
| `mcpp::action{…}.submit()` *(2026.8.5.1+)* | `mcpp:action=` —— **声明一个构建图节点**,而不是在这里把活干了(见下) |

### `warning` —— 成功了,而且仍然被听见(2026.8.21.2+)

构建程序的输出**只在程序非零退出时**到达用户:mcpp 抓取它,并在失败时打印抓到的东西。
于是 `std::printf` 或 `std::fprintf(stderr, ...)` 写的提示,在**恰恰需要它的那些成功
构建**上一个字都不显示。

```cpp
if (const char* dir = mcpp::xpkg_dir("xim", "qemu-riscv"); dir && *dir) {
    mcpp::runner(std::format("{}/bin/qemu-system-riscv64", dir).c_str());
    // …… 其余 argv ……
} else {
    mcpp::warning("qemu-riscv 未安装,于是 `mcpp run` 没有 runner。"
                  "装一次即可:  xlings install qemu-riscv -y");
}
```

⚠️ **它之所以存在,是因为两个替代方案都更差,而且都试过。** 写到 stderr 的提示在成功
构建上什么都不打印。非零退出也不对:`mcpp build` 并不需要模拟器,让一个正确的构建失败,
是用一句缺失的话换一条坏掉的命令。

用它来说一个程序**已经正确处理**、但用户会想知道的情况 —— 最常见的是「我没找到 X,所以
我没有配置任何依赖它的东西」。要报错就非零退出,那条路径的输出已经会被打印。

**它不会让构建失败。** `mcpp build` 仍然退出 0。

**它带归属。** 该行显示为 `<包名>: <文字>`,因为一个 workspace 里可能有好几个程序在说话,
而读者需要知道该打开哪一份清单。

⭐ **它穿过构建缓存。** 构建程序的结果是被缓存的,命中时不再运行 —— 所以一条只活在运行
路径上的提示,会在工程的第一次构建出现、之后再也不出现,而那读起来像「问题已解决」。
mcpp 在每次命中时重放它。

⚠️ **全工程 no-op 构建什么都不打印,包括这一条。** 无事可做时,构建根本到不了
`build.mcpp` 阶段 —— 它同样不会报告构建了哪个目标、推断了哪些源码。touch 一下源码,提示
就回来了。

### `runner` —— 产物的执行方式(2026.8.19.2+)

板级支持包知道模拟器、机器型号和固件模式,也知道模拟器**在哪** —— 而静态 manifest
写不出来:载荷路径里带着 home 和版本号。

```cpp
const char* qemu = mcpp::xpkg_dir("xim", "qemu-riscv");
mcpp::runner(std::format("{}/bin/qemu-system-riscv64", qemu).c_str());
for (auto a : {"-machine","virt","-nographic","-no-reboot","-kernel"})
    mcpp::runner(a);
```

这样消费者**完全不需要 `[target.<triple>]` 段**。它若还是写了,**以它为准** ——
调试时把 `-bios default` 换成 `-bios none -semihosting` 是正当需求 —— 且 mcpp 会说明
它覆盖了哪个依赖。

⚠️ **可执行文件要发绝对路径。** 裸名会经 `PATH` 解析到一个 shim,而 shim 按**拥有它
的 home** 派发,那未必是本次构建用的 home。

⚠️ **只能有一个依赖提供 runner。** 两个板级支持包都声称知道怎么跑这个产物是配置
错误;mcpp 会**同时点名两个**并报错,而不是把它们并成一个谁也不是的 argv。

### 问,而不是声明:`toolchain_dir` / `sysroot_dir`(2026.8.19.4+)

```cpp
const char* tc = mcpp::toolchain_dir();   // 已解析工具链的载荷根目录
const char* sr = mcpp::sysroot_dir();     // 目标的 C 库根目录,没有则为 ""
```

一个包需要工具链自带的头(比如 freestanding 标准库子集要的 libc++ 头),或者需要
目标 C 库里的某个**文件**(比如板级支持包要的链接脚本)时,应当**问这个目录在哪**,
而不是去声明一个依赖来把它拽进来。

⚠️ 这不是写法差异。声明 `xim:llvm` 会把包**钉死在一个标准库实现**上;声明
`xim:picolibc-riscv@1.8.12` 会把它钉死在**一个 C 库、一种架构、一个版本**上。而这些
都不是一个内容全是标准规定的名字的包的属性。**问**则会跟随 `[toolchain]` 与
`--target` 真正解析到的结果。

宿主目标上 `sysroot_dir()` 为空:那里 C 库随编译器载荷或运行时绑定而来,没人需要找它。

### 找到 `[xlings] deps` 的载荷:`xpkg_dir`(2026.8.19+)

`dep_dir` 回答的是 **mcpp** 依赖。xlings 包是另一个命名空间、另一套 store 布局,
`xpkg_dir` 是它的接口:

```cpp
// mcpp.toml
//   [xlings]
//   deps = ["xim:picolibc-riscv@1.8.12"]

const char* sysroot = mcpp::xpkg_dir("xim", "picolibc-riscv");   // 精确
const char* same    = mcpp::xpkg_dir("picolibc-riscv");          // 裸名
```

带命名空间的形式只对该命名空间下声明的包作答,应当优先使用;裸名形式是常见的单条
声明的便利写法,两个命名空间都声明同一个名字时,它回答**先声明**的那个。两者在包
未声明或未安装时都返回 `""` —— 缺失是否致命只有调用方知道,所以由它自己说。

做成接口而不是给一条路径约定,是因为另一种做法是让构建程序把
`<home>/data/xpkgs/<ns>-x-<name>/<version>` 写进代码,而那是 mcpp 可以随时改的
store 内部结构 —— 与 `dep_dir` 存在的理由相同。

⚠️ **带版本固定**的引用只解析到那个版本,否则什么都不返回。请求 `1.8.12` 却静默拿
到 `1.9.0`,是那种要到产物里才被发现的答案。

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
全局缓存,并把路径交回。这个请求写在 `mcpp.toml` 而不是这里,理由和依赖本身
一样:向依赖图索取一个额外产物是**图级别**的请求,而图必须保持可静态分析。
完整契约(含 `[tools.overrides]` 与 `reexport = true` —— 库据此把整条工具链交给
调用方,因此只需写**一条**依赖而不是四条)见 [05 §2.14](05-mcpp-toml.md)。

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

### 声明产出而非执行动作:`mcpp::action`(2026.8.5.1+)

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

mcpp 会播下一个带着该声明的占位文件,使 prepare 期的扫描与生成器将要产出的
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
| 兼容性 | 该模块**内置在 mcpp 二进制里**,由运行它的那个 mcpp 现场编译,程序与引擎不可能不一致 | 字符串是冻结的文本,没有任何机制校验它 |
| 新指令 | 以新函数的形式到来 | **不会再新增** |
| 未知指令 | **硬错误** | 警告后忽略 |

用 `import mcpp;` 的程序会自动声明它编译时对应的协议版本(`mcpp:protocol=<N>`,
在 `main` 之前发出,无需自行编写)。mcpp 用它做两件事:

- 程序声明的协议**高于** mcpp 所理解的 → **拒绝执行**,并给出升级提示。继续跑会
  静默丢掉构建依赖的指令,而「构建成功了但那个 flag 根本没到」是最难查的一类问题。
- **未知指令是错误**而不是警告,而且这条错误会把**两种可能的原因都说出来**。
  它没法只说一种:协议号是由**编译**该程序的那个 mcpp 现场打上的,并不由包本身携带
  —— 于是一个写给新 mcpp 的包到了老 mcpp 手里,身上戴的是老引擎的号。
  **两个号一致因此完全不能说明这个键是不是来自未来。**

`printf` 风格的程序什么都不声明,因此保留历史上的「警告并忽略」行为。这一面
**冻结在上表的 11 条指令**上——它仍然能用、也会继续能用,但新能力只在类型化 API 里
落地。**要长期维护的程序请用 `import mcpp;`。**

#### 一个需要更新 mcpp 的包

当已发布的包调用了当前 mcpp 没有的类型化函数时,点名的编译错误之后会跟着:

```
       The `mcpp` build module this engine bundles does not have that name.
       Either the package was written for a newer mcpp (try `mcpp self update`;
       this is mcpp 2026.8.19.2), or the name is misspelled …
```

**包自己处理不了这件事**,而原因值得知道 —— 最直觉的那道防护编译不过:

```cpp
if constexpr (requires { mcpp::runner("qemu"); })   // ✗ 名字不存在时是硬错误
    mcpp::runner("qemu");
```

`requires` 表达式作用在一个**不存在的限定名**上时是 ill-formed,而**不是求值为
`false`**。所以语言内没有特性探测这条路:采用了新指令的包只能在自己的 README 里
用文字写明版本下限,并依赖上面那条诊断。**这类包应当写清楚它需要哪个版本的 mcpp。**

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

### `PATH` —— 项目声明的那个环境(mcpp 2026.8.25.1+)

声明了 `[xlings].subos` 的项目,其构建程序运行时,该环境的 `bin` 在 `PATH` 的
最前面:

```
PATH=<被声明环境的 bin>:<mcpp 自己启动时的 PATH>
```

于是构建程序里的裸名命令,在每一台构建它的机器上都解析到项目点名的那个环境
里面。

⚠️ **只对声明了的项目生效。** 没有 `[xlings].subos` 的项目拿到的是 mcpp 启动时
的 `PATH`,逐字节不变。把一个共享目录放到每个项目前面,会让「构建看见什么」取
决于这台机器上还装过什么——同一台机器上的两个项目彼此一致,而同一个项目在两台
机器上不一致。

前置而非替换的理由:构建程序理应会调 `git`、`python3` 或 shell,这些都不在
SubOS 里。前置让被声明的环境成为默认答案;宿主仍在其后可达。

⚠️ **`command -v` 回答的是这台机器,不是这次构建。** 在此之前,构建程序拿
`PATH` 去问一个被声明过的工具,可能问到无关的那个——实测于
`qemu-system-riscv64`:答案是一个执行时报「is not installed in this subos」的
shim,而可用的那份就在项目自己的环境里,根本不在 `PATH` 上。

这个选择就是[第 8 章](08-toolchain-internals.md)已经描述的那一个——决定项目链接
哪个 C 库的同一条声明,多交付给了一个消费者。被声明的环境是什么、什么时候需要
它,见[第 17 章](17-the-project-environment.md);`examples/07-project-subos/` 是
一个可运行的工程。

## 写一个规则包

一条规则 ——「对这些 `.proto` 跑 protoc」「对这些源码跑 clang-tidy」—— 属于一个包,
而不该被复制到每个消费者的 `build.mcpp` 里。机制是
[`host-module = true`](../05-mcpp-toml.md);本节讲的是它里面应该长什么样。

下面这些从第一个规则包 `mcpplibs.grpcgen` 归纳而来,每一条特征都单独判过是必然还是偶然。
它们是指引而非规则,因为其中没有一条能给出引擎可以检查的判据。

**分层不得有断崖,且上层必须是下层的组合。** `generate_all(opt)` **就是**
`submit(plan_all(opt))`,`.grpc = true` **就是** `.plugins = {cpp()}`。超过两个旋钮之后,
无法继续下降的消费者会手写六十行绕开规则,而那六十行随后就与规则悄悄漂移。

**提供一对 plan/submit。** 最底层必须把计划好的边交回去,让消费者改完再提交。这是让上一段
成立的机制,不是命名偏好。

**不要复制引擎已经拥有的真相。** mcpp 把每条 action 的完整 argv 写进 `build.ninja`,
用 `ninja -t commands` 就能取回。第二个真相源只会漂移。规则拥有的是另一半 ——
哪些旋钮产生了这条命令 —— 它属于每条边的 `description`。

**失败与提示走不同的通道。** mcpp 只在构建程序非零退出时打印抓到的输出,所以失败写 stderr
并返回非零。而必须在**成功**构建上被看见的消息要走
[`mcpp::warning`](#warning--成功了而且仍然被听见2026821-2);成功时的 stderr 被丢弃,
也就是说选错通道恰好在需要它的那些构建上一言不发。

**一个 `(名字, 版本)` 只对应一份载荷。** mcpp 用这个二元组标识已安装的包,所以一个重新
打包却保持版本字符串不变的规则不会触发重装,消费者会继续跑旧规则且没有任何诊断。用哪套
编号由作者决定 —— 与被包装的工具同版本发布是正当的(两者从同一个 tag 出去时,这个号
告诉消费者的是真话),但载荷变了版本号就必须变。

**通过消费者来测它。** 规则只被 `build.mcpp` 消费,所以编译它什么都证明不了。它的测试是一个
依赖它的示例工程:构建,并对产物做断言。

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

因此必须**声明输入**:如果程序读了 `config.h` 或 `USE_FAST` 变量,就分别 emit
`mcpp:rerun-if-changed=config.h` / `mcpp:rerun-if-env-changed=USE_FAST`。这用一份明确的
输入/输出契约取代了过去「进程退出码为 0 就当成功」的猜测——让增量构建保持正确。

无变化时输出 `build.mcpp up to date (cached)`;否则是 `build.mcpp compiling` /
`running`。

## 说明与限制

- **在主机上运行——交叉构建下也是**(mcpp 0.0.95+)。`mcpp build --target <triple>`
  下,程序用宿主解析的工具链编译、在宿主运行,并看到 `MCPP_TARGET` = 交叉三元组。
  纯声明式的目标门控仍首选 `[target.'cfg(...)']` 表——参见
  [05 - mcpp.toml 工程文件指南](05-mcpp-toml.md)。
- **当前工作目录是工程根目录**,因此相对路径(`src/generated.cpp`)会落在预期位置。
- `build.mcpp` 非零退出会中止构建并打印其输出。
- **运行有时间上限**(mcpp 2026.8.5.1+):构建程序默认有 **600 秒**,超时后 mcpp
  杀掉它并让构建失败,错误里会点名是哪个包。可按包配置:

  ```toml
  [build]
  build_program_timeout = 1800   # 秒;0 = 不限
  ```

  优先级(与 `macos_deployment_target` 同构):

  ```
  MCPP_BUILD_PROGRAM_TIMEOUT=<秒>   本次调用
    > [build] build_program_timeout  拥有该 build.mcpp 的那个包的 manifest
    > 600                            内置默认
  ```

  值取自**拥有该构建程序的包**的 manifest,因为只有它的作者知道生成器要跑多久。
  当一个**依赖**的构建程序超时时,错误会点名要改的那份 `mcpp.toml` —— 改自己的
  那份不会有任何效果。

  不写这个键与写 `0` 不是一回事:不写=用默认上限,`0`=完全不设上限。

  **这个上限从 mcpp 2026.8.11.1 起在所有平台生效**。此前它只在 POSIX 上生效:
  Windows 的启动器会退回到无界路径,所以这个键——以及 `mcpp test --timeout`、
  `--build-timeout`——在那里都是静默的空操作。现在 Windows 把子进程放进 Job 对象,
  到期时关闭它,于是被杀掉的是**整棵进程树**而不只是直接子进程(否则一个还攥着
  捕获管道的孙进程会让杀掉之后的读取一直挂住)。

  **编译**这一步刻意**不设**上限——与 `mcpp test` 同一条不对称纪律:
  编译跑得久通常是正当的(首次构建 `std` 模块就是分钟级),杀掉它只会产生莫名其妙的
  失败;而构建**程序**跑得久通常是卡住了,不设上限就会让整个构建挂死且毫无诊断。

  > **为什么不做「超时时询问用户」**([#410](https://github.com/mcpp-community/mcpp/issues/410)):
  > 构建程序的 stdout 已经被 dup2 进一根承载 `mcpp:` 指令协议的管道,没有交互通道;
  > 多数构建发生在没有人看的地方(CI、流水线、ninja 的子进程),而一个卡在提示上的
  > 构建比一个失败的构建更难诊断;并且构建结果不应该取决于一次击键。
  > 可配置的上限 + 一条点名要改哪个文件的报错,回答的是同一个需求。
