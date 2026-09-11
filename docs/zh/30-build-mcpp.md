# 30 —— 构建程序:`build.mcpp`

[English](../30-build-mcpp.md) | **简体中文**

**读者:**构建里需要一步 mcpp 没有规则的工作的作者 —— 代码生成、嵌入资源、
一项检查,或者第二个编译器。

**本章回答的那一个问题:**怎样把这份工作加进构建图,使它像其余部分一样被定序、
进指纹、可增量。

**不在这里:**把这一步打包给别的工程用,那是
[31 —— 编写规则包](31-authoring-a-rule-package.md);以及这一步要跑的工具,那是
[23 —— 项目环境](23-the-project-environment.md)。

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
| `mcpp:runner=<token>` *(2026.8.19.2+)* | 执行本次构建产物的命令的**一个 argv token**(宿主跑不了它时)。一个 token 一次调用、按顺序;产物路径会被追加(或替换 `{}`)。**到达消费者**。可执行文件要发**绝对路径**,且**只能有一个**依赖提供它 |
| `mcpp:link-flag=<flag>` *(2026.9.6.5+)* | 加一条本程序**算出来的**链接标志,原样传递。这是 `link-lib` / `link-search` / `link-script` 各自命名一类东西之后留下的出口:生成的版本脚本(`-Wl,--version-script=`)、运行时接管 C 库符号用的 `-Wl,--wrap=malloc`、以及 `-Wl,--exclude-libs,ALL`(静态吞入的第三方不得成为本包 ABI 的一部分)。按发出顺序追加在 `[build] ldflags` 之后。**到达消费者**,与 `[build] ldflags` 一致 —— 理由见下 |
| `mcpp:windows-subsystem=<target>:<value>` *(2026.9.12.2+)* | 设置**本包**可执行目标 `<target>` 的 PE 子系统(`console` 或 `windows`),与 `[targets.<target>] windows_subsystem`(docs/04)是同一字段。只到达该目标的链接,不到达其他目标或消费者,在非 PE 目标上不产生任何标志。本包未以 `kind = "bin"` 声明该目标、取值不在集合内、取值与 mcpp.toml 的声明矛盾,这三种情形都在应用任何指令之前被拒绝 |
| `mcpp:windows-entry=<target>:<value>` *(2026.9.12.2+)* | 设置可执行目标 `<target>` 的入口函数(`main`、`wmain`、`WinMain` 或 `wWinMain`),与 `windows_entry` 是同一字段;作用域与拒绝条件同 `windows-subsystem` |
| `mcpp:link-script=<path>` *(2026.8.19+)* | 用这个**链接脚本**链接(`-T`;相对路径按包根解析,发出的是绝对路径,因为链接是在构建目录里跑的)。与 `include-dir` 不同,它**到达消费者** —— 板子的内存布局恰恰是消费者写不出来的那一项 |
| `mcpp:warning=<text>` *(2026.8.21.2+)* | 对用户说一句话并**继续**。唯一一条不改变编译行、链接行与源码集的指令。它**穿过构建缓存** —— 见下 |
| `mcpp:fact=<name>=<version>` *(2026.9.5.2+)* | 陈述程序**测得的机器事实**(`cuda.driver=12.4`)。在编译任何东西之前与 floor 比较;见下 |
| `mcpp:floor=<name> >= <version>` *(2026.9.5.2+)* | 陈述本包对该量的**下界**。不满足 ⇒ 构建被拒并给出两侧取值(`version-floor-unmet`);没有人陈述事实的下界保持沉默 |
| `mcpp:rerun-if-changed=<path>`     | 该文件变化时重跑 `build.mcpp` |
| `mcpp:rerun-if-env-changed=<VAR>`  | 该环境变量变化时重跑 `build.mcpp` |

程序**请求**构建边(开关、库、源码),它**不能**新增注册表依赖——请把依赖图保持在
`mcpp.toml` 里声明式管理(包括平台条件依赖 `[target.windows.dependencies]`)。
`build.mcpp` 用于*叶子*决策:开关、代码生成、链接需求。

`link-flag` 刻意**不**私有,而这一点值得说明,因为相反的选择看上去更安全。编译接口有
一个声明式的公开对应物(`[build] include_dirs`),所以构建期程序若能加宽它就是绕过了
manifest —— 这正是 `include-dir` 私有的理由。链接标志没有这个分裂:`[build] ldflags`
本来就传播给消费者,因此一个私有的"算出来"形态会与它自己的声明式孪生行为不一致。

后果写明而不藏起来:一个依赖发出 `-Wl,--version-script=`,该标志也会落到消费者的链接
行上,而那通常不是它的本意。这个隐患不是新的 —— 依赖在 `[build] ldflags` 里写同一条
标志一直如此 —— 所以这条指令加宽的是**谁能算出这个值**,不是**这个值能到达哪里**。

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
| `mcpp::link_flag(s)` *(2026.9.6.5+)* | `mcpp:link-flag=` |
| `mcpp::windows_subsystem(target, value)` / `mcpp::windows_entry(target, value)` *(2026.9.12.2+)* | `mcpp:windows-subsystem=` / `mcpp:windows-entry=` |
| `mcpp::link_script(p)` *(2026.8.19+)* | `mcpp:link-script=` |
| `mcpp::runner(tok)` *(2026.8.19.2+)* | `mcpp:runner=` —— 见下 |
| `mcpp::xpkg_dir(ns, name)` / `mcpp::xpkg_dir(name)` *(2026.8.19+)* | `[xlings.workspace]` 里声明的包的载荷目录 —— 本 manifest 声明的,或编进本构建程序的某个依赖声明的(2026.9.6.6+);没声明或没安装时返回 `""`(见下) |
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

**它之所以存在,是因为两个替代方案都更差,而且都试过。** 写到 stderr 的提示在成功
构建上什么都不打印。非零退出也不对:`mcpp build` 并不需要模拟器,让一个正确的构建失败,
是用一句缺失的话换一条坏掉的命令。

用它来说一个程序**已经正确处理**、但用户会想知道的情况 —— 最常见的是「我没找到 X,所以
我没有配置任何依赖它的东西」。要报错就非零退出,那条路径的输出已经会被打印。

**它不会让构建失败。** `mcpp build` 仍然退出 0。

**它带归属。** 该行显示为 `<包名>: <文字>`,因为一个 workspace 里可能有好几个程序在说话,
而读者需要知道该打开哪一份清单。

**它穿过构建缓存。** 构建程序的结果是被缓存的,命中时不再运行 —— 所以一条只活在运行
路径上的提示,会在工程的第一次构建出现、之后再也不出现,而那读起来像「问题已解决」。
mcpp 在每次命中时重放它。

**全工程 no-op 构建什么都不打印,包括这一条。** 无事可做时,构建根本到不了
`build.mcpp` 阶段 —— 它同样不会报告构建了哪个目标、推断了哪些源码。touch 一下源码,提示
就回来了。

### 探针通道:`fact` / `floor`(2026.9.5.2+)

规则包是知道「怎么问机器它有什么」的那一方 —— 打开哪个库、调用哪个函数;
引擎则是不该知道的那一方。于是由包来**测量**,由引擎来**比较**:

```cpp
mcpp::fact("cuda.driver", "12.4");      // 这台机器有什么
mcpp::floor("cuda.driver >= 12.0");     // 本包需要它至少多新
```

在编译任何东西之前,未满足的下界会拒绝构建,并点名该量、两侧版本与陈述事实的
包;`mcpp why toolchain --format json` 把它归类为 `reason: version-floor-unmet`。没有人陈述事实的下界
**保持沉默**:不知道不等于不满足,由无知制造的拒绝是更坏的错误。

它防住的失败在构建期本身看不见:针对比驱动更新的设备运行时构建的程序,编译
干净、链接干净,到第一次使用才失败,而消息两边都不点名。解析了该运行时的规则包
在第一次编译之前就知道两个数字。

**两个字符串对引擎都没有意义。** `cuda.driver` 是流过引擎的数据;引擎读到的
是一个名字、一个关系、一个版本,第二个后端不需要改引擎。事实的拼法与包在
`[runtime] provides` 里静态声明的一致,下界与 `[[runtime.requirements]]` 的
`kind = "version-floor"` 一致:两条通道落进同一张表。

**事实随程序的其它输出一起进缓存**,命中时被回放。要声明什么会改变它 ——
对读出版本的那个库 `rerun_if_changed` —— 否则事实会比它描述的机器活得更久。

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

**可执行文件要发绝对路径。** 裸名会经 `PATH` 解析到一个 shim,而 shim 按**拥有它
的 home** 派发,那未必是本次构建用的 home。

**只能有一个依赖提供 runner。** 两个板级支持包都声称知道怎么跑这个产物是配置
错误;mcpp 会**同时点名两个**并报错,而不是把它们并成一个谁也不是的 argv。

### 问,而不是声明:`toolchain_dir` / `sysroot_dir`(2026.8.19.4+)

```cpp
const char* tc = mcpp::toolchain_dir();   // 已解析工具链的载荷根目录
const char* sr = mcpp::sysroot_dir();     // 目标的 C 库根目录,没有则为 ""
```

一个包需要工具链自带的头(比如 freestanding 标准库子集要的 libc++ 头),或者需要
目标 C 库里的某个**文件**(比如板级支持包要的链接脚本)时,应当**问这个目录在哪**,
而不是去声明一个依赖来把它拽进来。

这不是写法差异。声明 `xim:llvm` 会把包**钉死在一个标准库实现**上;声明
`xim:picolibc-riscv@1.8.12` 会把它钉死在**一个 C 库、一种架构、一个版本**上。而这些
都不是一个内容全是标准规定的名字的包的属性。**问**则会跟随 `[toolchain]` 与
`--target` 真正解析到的结果。

宿主目标上 `sysroot_dir()` 为空:那里 C 库随编译器载荷或运行时绑定而来,没人需要找它。

### 驱动第二个编译器:`toolchain_sysroot` / `toolchain_binutils_dir`(2026.9.5.2+)

```cpp
const char* sr = mcpp::toolchain_sysroot();        // mcpp 传的 `--sysroot`,没有则为 ""
const char* bu = mcpp::toolchain_binutils_dir();   // mcpp 用 `-B` 指的目录,没有则为 ""
```

规则包有时必须运行一个 **mcpp 并未解析**的编译器。`nvcc` 拒绝 libc++ 宿主编译器,
在 GCC 16 的 `<type_traits>` 上失败,因此 CUDA 规则包要从声明的载荷里另选一个宿主
编译器;`hipcc` 与 `-fsycl-host-compiler` 面对同一个问题。

那个编译器对自己被放进的环境一无所知。在 sub-OS 里 C 库不在 `/usr/include`,汇编器
不在 `/usr/bin`,于是它遇到的第一个 `#include` 就失败:

```
crt/host_config.h:218: fatal error: features.h: No such file or directory
```

这两个答案就是 mcpp 为同一目标传给自己那个编译器的开关。把它们转发过去
——`--sysroot=<值>` 与 `-B<值>`,经外层工具的宿主选项拼法——第二个编译器就看到
第一个看到的东西。

**不是 `sysroot_dir()`。** 那个回答的是目标**档位**的问题,在宿主目标上为空,
而宿主目标恰恰是这一对存在的场合。mcpp 不传某个开关时,对应的那个为空串。

### 解析出的 C++ 标准库:`cxx_stdlib`(2026.9.6.3+)

```cpp
const char* impl = mcpp::cxx_stdlib();   // "libstdc++" | "libc++" | "msvc-stl" | ""
```

`compiler()` 回答不了这个问题。clang 在一台机器上链 libc++、在另一台上链
libstdc++,两种情况都答 `clang`,而两个实现接受的东西不同——在头文件里析构一个
指向不完整类型的 `unique_ptr`,libstdc++ 通过而 libc++ 不通过。需要按名字拒绝
这种配置的构建程序不能去问 `compiler()`,因为那个答案会连同能工作的配置一起拒掉:

```cpp
if (std::string_view(mcpp::cxx_stdlib()) == "libc++") {
    std::fprintf(stderr,
        "this feature does not compile under libc++; select a libstdc++ "
        "toolchain, or turn the feature off\n");
    return 1;
}
```

名字里带 `cxx` 是有意的:`MCPP_TARGET_LIBC` 是 **C** 库。两者是不同的问题,而在
一个不断提到 glibc 与 musl 的生态里,它们不能共用一个词。

### 找到 `[xlings.workspace]` 的载荷:`xpkg_dir`(2026.8.19+)

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

**带版本固定**的引用只解析到那个版本,否则什么都不返回。请求 `1.8.12` 却静默拿
到 `1.9.0`,是那种要到产物里才被发现的答案。

**带约束**的引用(`>=8.5.0`、`^1.2`)解析到满足它的**最高已安装版本**
(2026.9.6.6+)。在那之前整个版本位是拿去与目录名比对的,于是一条范围装上了载荷,然后
回答「没装」—— 这正是规则包无法声明下界、而每个工程都要把规则的包列表重写一遍的原因。

**依赖声明的包同样被作答**(2026.9.6.6+),而且答的是这次构建**真正装上**的版本,不是
本地 manifest 写下的那个。一个包只有一个版本:工程与规则都命名它时,离产物更近的声明赢,
而两侧被告知同一个答案。见 [23 — The Project Environment](23-the-project-environment.md) 的「一个包一个版本」。

**`[feature-xlings.<f>]` 在 `<f>` 生效时同样被作答**(2026.9.6.2+)。这张表从诞生
起就参与供给 —— 在那里写下一个包,它就会被下载并安装 —— 但构建程序的环境只由
`[xlings.workspace]` 填充,于是载荷明明在盘上,`xpkg_dir` 却返回 `""`。这时构建程序
唯一说得出口的话是「请声明这个包」,而它指的那条声明作者早已写下。

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
调用方,因此只需写**一条**依赖而不是四条)见本章*依赖产出的 host 工具*。

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

**声明过的输入在快路径上同样被比较**(2026.9.5.4+)。当所有源文件都不比
`build.ninja` 新时,工程走快路径,跳过读取构建程序缓存的那个阶段;在 2026.9.5.4
之前,快路径只问 glob 的路径集合。数据文件既不在 `src/` 下也没有 C++ 扩展名,mtime
扫描同样看不见它:改了它,上一次的产出原样留着,构建打印 `Finished dev in 0.00s`。
现在快路径按缓存记录的方式比较三类输入 —— glob 的路径集合、声明文件的内容哈希、
声明环境变量的值 —— 因此 `rerun_if_changed` 在两条路径上含义相同。

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
| `source` | 可编译的进编译集,其余只产出、不编译 | **声明它的那个包的每条编译边都等它** | protoc、转译器、协议/IDL 生成器 |
| `check` | 一个 stamp 文件,由 mcpp 写入 | 与编译并行;`blocking = true` 让该包的编译边等它 | clang-tidy、格式/ABI 检查 |
| `object` | 进**链接**集 | 链接边消费它们 | 资源编译器、`objcopy` 嵌 blob、生成的 `.def`、预编译 `.o` |
| `artifact` | 一个新文件 | 它的**输入**是链接产物,所以在链接之后跑 | 签名、打包、size budget |

`artifact` 也是唯一允许写 `${mcpp.stage_dir}` 的 role —— 见下文
[产出可分发物](#产出可分发物pack_format-与-stage_dir20269111)。另外三个跑在链接之前
或与链接并行,没有任何已暂存的东西可读,所以 mcpp 会拒绝这个占位符,而不是把它展开成
一个恰好存在的路径。

全程不涉及任何 phase 机制。`object` 与 `artifact` 由 ninja 自己的文件依赖定序 ——
这也是为什么 `artifact` 不会像朴素的「post 构建钩子」那样把自己重复施加一遍。
`source` 与 blocking 的 `check` 则由一条 order-only 边定序:从声明它的那个包的
编译边,指向该包的 action 产物。

> **`source` 为什么需要这条边(mcpp 2026.8.30.2+)。** 生成的 `.cpp` 会成为编译它
> 那条边的输入,所以顺序是白得的。生成的**头文件**永远不会:它是通过 `-I` 找到的,
> 而能记录它的 depfile 要等到某次编译成功之后才存在。在此之前,一个产物全是头文件的
> action 在 `build.ninja` 里有节点却无人可达 —— 不在 `default`、不在 goal 集、没有
> 任何边消费它 —— 于是它从不执行,而编译器读到的是 mcpp 为已声明产物写下的那个空占位
> 文件。这条边**按包**划分,因为 `include_dir` 只染色声明它的那个包自己的 TU。

**命令自己发现依赖的 action 要声明 depfile**(mcpp 2026.9.7.1+)。`input()` 在
`build.mcpp` 运行时就把边的输入定死了,而那时命令还没执行,所以一个靠解析源码才知道自己
`#include` 图的编译器没有任何通道把结果报回来 —— 改动一个命令只是**读**过的文件不会触发
任何重建,`mcpp build` 会在一个陈旧产物上保持绿色。

```cpp
a.depfile = dep.c_str();        // 命令会写出的路径
a.arg("--depfile").arg(dep.c_str());
```

mcpp 为那条边写出 `depfile =` 与 `deps = gcc`,ninja 读取该文件并把它列出的文件并入这条边
的依赖。与此相关的每个设备编译器都已经能输出它:`glslangValidator --depfile`、
`glslc -MD -MF`、`slangc -depfile`、`nvcc`/`clang` 的 `-MD -MF`。

> **不要同时把 depfile 声明为 `output()`。** `deps = gcc` 会让 ninja 读完即删,所以一条
> 承诺了该输出的边会永远是脏的。

**check 的命令不必自己写 stamp**(mcpp 2026.8.29.1+)。判定是退出码,stamp 是**构建图**
需要的记账;命令成功时由 mcpp 创建它。在此之前每个 check 都需要一个包装脚本去 touch
那个文件 —— 而 command 是 argv、不假设有 shell,所以那个包装器**根本没法可移植地写出来**。
已经自己写 stamp 的命令不受影响:已存在的文件不会被动。

> stamp 缺失**不会**让构建失败。ninja 只是留着那个声明的输出不存在,并在之后**每次构建
> 都重跑**那条边 —— 看起来像一个通过了的检查,实际上它从未被满足。

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
> `ninja: no work to do`。Windows 资源请用 [`[resources]`](04-mcpp-toml.md);
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

### 产出可分发物:`pack_format` 与 `stage_dir`(2026.9.11.1+)

一个 `.msi`、一个 `.deb`、一个 AppImage、一个签过名的 `.app`,都不是那四个 role 的
惯常活计,而它们全都是 `artifact`:每一个都消费**链接产物**,产出用户去安装的东西。
引擎为它们加的是一套机制,而不是任何一种格式。

`mcpp pack --format <name>` 把 `<name>` 交给解析后的图去解决,方式与 `--target`
够到一个引擎不必逐个认识的三元组相同。`tar` 与 `dir` 仍然是 `mcpp pack` 自己拥有的
归档形状;此外的一切都来自某个包。

一个提供方有两半,而这两半不许被并成一半:

```cpp
import mcpp;
#include <string>
#include <string_view>

int main() {
    // 第一半,无条件。
    mcpp::provides_pack_format("appimage");

    // 第二半,有条件。
    if (std::string_view(mcpp::pack_format()) != "appimage") return 0;

    const std::string out = std::string(mcpp::out_dir()) + "/app.AppImage";
    mcpp::action a;
    a.id   = "appimage";
    a.role = "artifact";
    a.arg(tool).arg("${mcpp.stage_dir}").arg(out.c_str())
     .input("${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    return 0;
}
```

**无条件声明,有条件提交。** 声明是让引擎能回答一个发起请求的那次构建自己回答不了的
问题:`--format bogus` 要点名**当下确实可用**的那些格式,`--help` 要说「解析后的图
提供的任何格式」。两者读的都是一次「什么格式都没要」的 pass 收集到的集合。一个只在被
问到时才声明的成员,对它的作者仍然照常工作 —— 作者永远传的是自己那个格式 —— 而对其他
所有人,这个集合变成不可知的。对一个谁都没为之提交的格式,mcpp 会拒绝,而不是报告一次
「什么包都没产出」的成功打包。

**`mcpp pack --format <name>` 会 prepare 两次。** 一条 `artifact` action 是一条
ninja 边,而那棵暂存树是 mcpp 在链接**之后**产出的,所以这棵树不可能成为构建出它自己
那一次 pass 的输入。第一次 pass 收集声明,并在任何东西被编译之前拒绝未知的格式;随后
才是构建与暂存;第二次 pass 设上 `pack_format` 与 `pack_stage_dir`,并构建提供方提交
的那条边。第二次 pass 里没有任何值是重新推导出来的 —— 三元组与暂存路径都是第一次
pass 和那次暂存已经回答过的。

**暂存树是一个 bundle,不是一个根文件系统。** 它就是 `--mode vendored` 的含义:
`bin/`、`lib/`,可重定位、根在哪儿都行,并且已经过了 strip 策略、调试信息拆分与
`include`/`exclude`。一个 AppImage、一个 `.app`、一个 `.msi` 要的就是它现在这个样子。
而要一棵 FHS 树的格式(`.deb`、`.rpm`)自己负责重排布局,因为一个文件该落在哪个目录是
那个格式的知识,不是引擎的。

`mcpp pack --format dir` 把同一棵树写到一个路径上就停下,这是人去查看一个成员将会拿到
什么的方式。

**`mcpp pack` 报告的是这次请求**引入**的那些 artifact action。** 一条无论有没有人
要格式都在场的 action —— 一个签名 stamp、一次 size budget —— 不是可分发物,点名它会
是一个看起来像对的错答案。这个判据里没有任何一项是成员的性质:一个只打包一个具名程序、
从不读暂存树的格式,与一个消费整个闭包的格式被同等识别。对一个谁都没为之提交的格式,
会被点名拒绝。

**写了 `${mcpp.stage_dir}` 的 action 会自动获得一条对这棵树的 manifest 的依赖。**
mcpp 会写出 `<暂存树>.stage-manifest` —— 一个兄弟文件,永不是成员,所以它不会跑进任何
人的安装包里 —— 逐条列出每个已暂存文件的大小与相对路径。这条依赖由引擎添加,因为「用
了」本身就意味着「依赖」:没有它,这条边只在链接产物变化时才变脏,而一个闭包多出了某个
依赖的共享库、同时程序自己的字节并没有变的情况,会把上一次的可分发物原地留下,并报告为
已是最新。

命令是 **argv 而不是 shell 字符串**(不假设存在 shell —— Windows 没有能依赖的那个),
插值只有封闭的一组:

| 变量 | 含义 |
|---|---|
| `${mcpp.out_dir}` | 构建输出目录 |
| `${mcpp.bin_dir}` | 产出的二进制所在目录 |
| `${mcpp.compile_db}` | `compile_commands.json` 的路径(clang-tidy 的 `-p` 要的就是它) |
| `${mcpp.target_file:<name>}` | target `<name>` 构建出的文件 |
| `${mcpp.stage_dir}` *(2026.9.11.1+)* | `mcpp pack` 暂存出的那棵树,绝对路径。仅 `artifact` role 可用,且仅在 `mcpp pack --format <name>` 下可用 |

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
if constexpr (requires { mcpp::runner("qemu"); })   // 名字不存在时是硬错误
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
| `MCPP_TOOLCHAIN_SYSROOT` *(2026.9.5.2+)* | `mcpp::toolchain_sysroot()` | mcpp 传给自己那个编译器的 `--sysroot`;不传时为空串。供运行**第二个**编译器的规则包使用 —— 见上文「驱动第二个编译器」 |
| `MCPP_TOOLCHAIN_BINUTILS_DIR` *(2026.9.5.2+)* | `mcpp::toolchain_binutils_dir()` | mcpp 用 `-B` 指的目录;不指时为空串(musl 与 MinGW 载荷自带汇编器与链接器) |
| `MCPP_CXX_STDLIB` *(2026.9.6.3+)* | `mcpp::cxx_stdlib()` | 解析出的工具链使用的 C++ 标准库 —— `libstdc++`、`libc++`、`msvc-stl`;没有工具链解析时为空串。与 `MCPP_TARGET_LIBC` 不是同一个问题,后者是 C 库 |
| `MCPP_ACCEL` *(2026.9.5.2+)* | `mcpp::accel()` | 本次构建的设备轴,已解析 —— `--accel` / `--no-accel` 优先于 `[build] accel` —— 线上形态 `cuda12.9+{sm_89} ptx>=89`;不要加速器时为空串。规则包从它推导自己的开关(`-gencode`、`--offload-arch`),架构集合因此只在 manifest 写一次。同一个值也喂给 `cfg(accelerator = "…")` 这个 layer 键 |
| `MCPP_LANGUAGE_MODULES` *(2026.9.7.1+)* | -- | 声明它的那个包设了 `[language] modules` 时为 `1`,否则 `0`。**生成**面向消费者声明的规则读它来在模块接口与头文件之间选择,项目因此只需说一次。旧引擎不设这个变量,规则把缺席读作 `0` —— 也就是这个变量存在之前每个消费者的行为 |
| `MCPP_PKG_NAME` *(2026.9.7.1+)* | -- | 这个程序所构建的包的 `[package] name`。规则生成的每个名字都由它推导:消费者导入的模块、访问器所在的命名空间、生成头里的符号。在它存在之前,可用的最接近的答案是 `MCPP_MANIFEST_DIR` 的末段,那是目录名 —— 于是一个叫 `vulkan-saxpy` 的包放在名为 `app` 的目录下会生成 `app.shaders`,而工作区里每一个 `<something>/app/` 都声称拥有同一个模块。旧引擎下缺席,规则把缺席读作「沿用先前的推导」 |
| `MCPP_PKG_NAMESPACE` *(2026.9.7.1+)* | -- | `[package] namespace`。包未声明命名空间时为空。需要产出在索引范围内唯一的名字的规则用这一对而不是单用名字,因为包身份是 `(namespace, name)` |
| `MCPP_PKG_VERSION` *(2026.9.11.1+)* | `mcpp::package_version()` | `[package] version`。每一种安装包格式都要写版本号;在这个变量之前,项目只能把版本号在成员自己的 options 里再写一遍,而那份副本会与 `[package]` 漂移,且没有任何东西能发现 |
| `MCPP_PKG_DESCRIPTION` *(2026.9.11.1+)* | `mcpp::package_description()` | `[package] description`。包未声明时为空 |
| `MCPP_PKG_LICENSE` *(2026.9.11.1+)* | `mcpp::package_license()` | `[package] license` |
| `MCPP_PKG_AUTHORS` *(2026.9.11.1+)* | `mcpp::package_authors()` | `[package] authors`,以 `;` 连接。不用 `,`:一条 author 的惯例写法是 `Name <mail@host>`,名字里可能带逗号,以逗号连接的列表无法再切回原来的条目 |
| `MCPP_PKG_REPO` *(2026.9.11.1+)* | `mcpp::package_repo()` | `[package] repo` |
| `MCPP_PACK_FORMAT` *(2026.9.11.1+)* | `mcpp::pack_format()` | 本程序所处的这次 `mcpp pack` 的 `--format` 取值;任何普通构建下都为空。承载含义的正是这个空值 —— 成员据此为自己的提交加闸,于是 `mcpp build` 拿到的还是它一直以来的那张图 |
| `MCPP_PACK_STAGE_DIR` *(2026.9.11.1+)* | `mcpp::pack_stage_dir()` | `mcpp pack` 已经把闭包暂存到的位置,绝对路径;本次构建不在打包时为空。读它来判断这次要干的活是什么形状,而把 `${mcpp.stage_dir}` 写进 action —— 这样图里的路径与程序读到的路径不可能不一致 |
| `MCPP_DEVICE_SOURCES` *(2026.9.5.2+)* | `mcpp::device_sources()` | 本包有效 `sources` 匹配到的设备类源文件(`.cu`、`.hip`…),相对包根,一行一个;没有时为空串。引擎一个都不编译 —— 由本程序引入的规则包把每一个变成一条 `mcpp::action`。已经过收窄:构建未覆盖的 `{ glob, accel }` 条目贡献为空,因此 `--no-accel` 得到空列表 |
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

**只对声明了的项目生效。** 没有 `[xlings].subos` 的项目拿到的是 mcpp 启动时
的 `PATH`,逐字节不变。把一个共享目录放到每个项目前面,会让「构建看见什么」取
决于这台机器上还装过什么——同一台机器上的两个项目彼此一致,而同一个项目在两台
机器上不一致。

前置而非替换的理由:构建程序理应会调 `git`、`python3` 或 shell,这些都不在
SubOS 里。前置让被声明的环境成为默认答案;宿主仍在其后可达。

**`command -v` 回答的是这台机器,不是这次构建。** 在此之前,构建程序拿
`PATH` 去问一个被声明过的工具,可能问到无关的那个——实测于
`qemu-system-riscv64`:答案是一个执行时报「is not installed in this subos」的
shim,而可用的那份就在项目自己的环境里,根本不在 `PATH` 上。

这个选择就是[第 8 章](91-toolchain-internals.md)已经描述的那一个——决定项目链接
哪个 C 库的同一条声明,多交付给了一个消费者。被声明的环境是什么、什么时候需要
它,见[第 17 章](23-the-project-environment.md);`examples/07-project-subos/` 是
一个可运行的工程。

## 写一个规则包

一条规则 ——「对这些 `.proto` 跑 protoc」「对这些源码跑 clang-tidy」—— 属于一个包,
而不该被复制到每个消费者的 `build.mcpp` 里。机制是
[`host-module = true`](../04-mcpp-toml.md);本节讲的是它里面应该长什么样。

下面这些从第一个规则包 `mcpplibs.grpcgen` 归纳而来,每一条特征都单独判过是必然还是偶然。
它们是指引而非规则,因为其中没有一条能给出引擎可以检查的判据。

**每一个设备源都必须到达某个 action**(2026.9.5.2+ 的契约,自 2026.9.6.5 起强制)。
设备类源是引擎唯一没有编译规则的源:它经 `MCPP_DEVICE_SOURCES` 交给本包的构建程序,
要么作为 action 回来,要么根本不会被编译。若有源没有回来,mcpp 拒绝这次构建并点名文件:

    error: `opkit`: device sources that no action compiles:
             src/backends/cuda/saxpy.cu
             src/backends/vulkan/saxpy.comp

判据是 action 的**输入**,而不是「构建程序跑过了」:跑了却什么都没认领恰恰是常见情形,
因为一条规则只取它认识的扩展名、把其余留给别人。这同时也是 action 本就需要满足的条件
—— 编译某个文件却不把它声明为输入的 action,在那个文件变化时不会重跑 —— 所以满足这条
判据的规则也就是能正确增量的规则。它取代的读数是链接期的 undefined reference:那条消息
点的是符号而从不是那个文件;而 `kind = "lib"` 的目标连这条都没有,因为静态库不做解析。

**一条规则只取它认领的扩展名。** `mcpp::device_sources()` 是本包设备源的**全集**,
同一个构建程序里的每条规则读到的是同一个值。带两个后端的工程会把一个 `.cu` 和一个
`.comp` 放进这一份清单,于是把全集拿走的规则会把编译器不接受的文件递给它。规则按扩展名
挑选,并在本次构建没有命名它所服务的后端时安静返回 —— 带多条规则的构建程序会把它们
全部调用一遍。

**没有任何东西提供的 import 会被点名拒绝。** 一个构建程序可以 import 的是:`std`、
`std.compat`、内置的 `mcpp`,以及依赖边要来的 host 模块。此外的名字在编译器被调用之前
就被拒绝,并给出那个本该让它可导入的键:

    error: build.mcpp imports 'mcpp.rules.spirv', and no dependency provides it
    as a host module.
           ...
             [build-dependencies.<namespace>]
             <name> = { version = "...", host-module = true }
           declared without `host-module = true`: mcpp.plugins (in [build-dependencies])

**模块名由规则的源码声明,`mcpp.*` 是保留前缀。** host 模块以其接口单元声明的名字注册,
而不是以包名注册,所以 `export module mcpp.rules.spirv;` 就是消费者 import 的那个名字。
官方插件集中在一个包里,`mcpp:plugins`(仓库 `mcpp-community/mcpp-plugins`):规则包命名为
`mcpp.rules.<x>`,构建期工具命名为 `mcpp.tools.<x>`,每个成员由该包的一个 feature 选择
(见 [`host-module = true`](../04-mcpp-toml.md))。`mcpp.build.*` 是引擎自己的模块族,
不用于插件。引擎判定不了谁是官方,所以检查以包的**命名空间**为键,两者不一致时告警 ——

    warning: build rule 'mcpplibs.plugins' declares the module
    'mcpp.rules.spirv'; the 'mcpp.' prefix is reserved for rules maintained by
    the mcpp project.

什么都不会坏;只是这个名字声称了一个该包并不具有的来源。项目之外的规则自选前缀。

**工具不是规则,而可分发物两者都不是。** 三类活计,成员的前缀说明它回答三个问题中的
哪一个:

| 前缀 | 回答的问题 | 编译编译单元 | 在构建程序里执行 |
|---|---|---|---|
| `rules-*` | 这个编译单元如何被编译 | 是 | 否 |
| `tools-*` | 构建程序自己需要做什么 | 否 | 是 |
| `dist-*` *(2026.9.11.1+)* | 链接之后出来的是什么,以及用户以什么形态安装它 | 否 | 否 |

规则说明一个编译单元如何被 mcpp 并不驱动的编译器编译:它提交一条
action,由引擎调度。工具说明的是构建程序需要、而没有任何编译器执行的事,并在构建程序
运行时当场做掉。`mcpp.tools.embed`(feature `tools-embed`,mcpp 2026.9.5.4+)是第一个:
它把数据文件写成程序编译进去的头文件(字节数组或 32 位字数组),内容未变时不重写文件,
因此无条件调用它不会带来任何重编。

`examples/09-heterogeneous/cuda` 与 `examples/09-heterogeneous/vulkan` 像任何工程一样从 `mcpp:plugins`
消费 `mcpp.rules.cuda` 与 `mcpp.rules.spirv`。

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
[`mcpp::warning`](#warning--成功了而且仍然被听见20268212);成功时的 stderr 被丢弃,
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

## 依赖产出的 host 工具(mcpp 2026.8.5.1+)

一个包能构建出消费者在**构建期**需要的二进制 —— `protoc`、`grpc_cpp_plugin`、
`flatc`、`moc`、转译器。在依赖上声明:

```toml
[dependencies]
protobuf = { version = "35.1",   tools = ["protoc"] }
grpc     = { version = "1.83.0", tools = ["grpc_cpp_plugin"] }
```

每个名字必须是该包的一个 `kind = "bin"` target。mcpp 会**为构建机器**构建它,
并把绝对路径以 `MCPP_DEP_<PKG>_BIN_<TOOL>` 交给 `build.mcpp` —— 用
`mcpp::dep_bin("protobuf", "protoc")` 读取(见 [30 — build.mcpp](30-build-mcpp.md))。

四条值得知道的性质:

- **永远是 host 二进制。** 即使 `mcpp build --target <triple>`,工具依然为**本机**
  构建 —— 代码生成器必须在这里跑。它是一次独立的、面向 host 的子构建:工具包
  自己的 `[toolchain]`、自己的依赖解析生效,不需要与当前构建一致。安全的原因是
  可执行文件与工程代码**零 ABI 接触**。
- **单一版本轴。** 工具的版本**就是**依赖的版本,所以「protoc 与其运行时不匹配」
  这种情况**不可表达**。(把工具单独打包正是会出这个问题,而且它在**运行期**才咬人,
  不是编译期。)
- **默认关闭。** 没人要就什么都不构建,成本由消费者付。包用 `[features]` +
  `required_features` 给昂贵的部分加门(protobuf 的 `protoc` 需要 libprotoc 的
  ~157 个额外 TU,只用运行时的人绝不该编译它)。
- **全局缓存**,按 包版本 × host 工具链 × feature × 自身依赖闭包 键控 —— 每台机器
  构建一次,而不是每个工程一次。

**这个键里没有源码内容,而对 `path` 依赖这一点是看得见的。** 已发布的版本不可变,
所以对来自索引的工具,这个键是精确的。而正在旁边被编辑的工具,两次构建之间版本相同,
缓存里的二进制就留在原地:在
[`examples/12-a-new-device-language`](../../examples/12-a-new-device-language/)
上实测,改动工具的 emitter 之后 `mcpp run` 打印的是上一次的答案,而抬高工具包的版本
之后它被重建、产物随之改变。抬版本,或用 `mcpp cache clean` 清空构建缓存 ——
tool store 就住在里面,路径是 `<mcpp cache dir>/tool/<index>/<name>@<version>/`。

**缺口在重建,不在跟踪。** 把工具列进 action 输入的规则,确实会在那个文件的字节变化
时重跑 —— 实测直接覆盖 store 里的二进制,产物随之改变。不发生的是「让这些字节变化」
的那次重建。

### `[tools.overrides]` —— 使用已有的二进制

```toml
[tools.overrides]
"compat.protobuf:protoc" = "/usr/bin/protoc"
```

或者不改 manifest(CI、发行版打包):

```bash
MCPP_TOOL_PROTOBUF_PROTOC=/usr/bin/protoc mcpp build
```

命中 override 会**完全跳过构建**。每个同类系统都提供这条逃生舱(vcpkg 的
`VCPKG_HOST_TRIPLET`、CMake 的 `LLVM_NATIVE_TOOL_DIR`、Qt 的 `QT_HOST_PATH`),
理由一样:一个在本机构建不出来的工具**不能是死路**。它**刻意不进** cache key ——
逃生舱不是可复现输入。

### `host-module = true` —— 可复用的构建规则以包分发

一条规则(比如「对这些 `.proto` 跑 protoc」)应该**写一次**,而不是复制进每个
消费者的 `build.mcpp`。把它做成普通的 mcpp 库包再 import:

```toml
[dependencies]
protobufgen = { version = "0.1.0", host-module = true }
```

```cpp
// build.mcpp
import mcpp;
import protobufgen;
int main() { return protobufgen::generate({"schema"}) ? 0 : 1; }
```

mcpp 会把该包的 lib 根模块**为 host 编译,且与 `build.mcpp` 在同一条命令里** ——
这正是 BMI 能用的前提:一个模块接口只对「在 standard / dialect / 编译器身份上与
它一致」的编译可导入。

于是规则**有版本、能测试、能通过既有的包管理器分发**,而且是用 **C++** 写的
—— 不引入第二门语言,这正是 `build.mcpp` 存在的理由。

**模块名是规则源码自己声明的那个**(mcpp 2026.8.29.1+)。`export module
acme.rules.protobuf;` 就以 `acme.rules.protobuf` 被 import,与包叫什么无关。
模块名是作者定义的 API,不镜像包身份 —— 普通库包一直遵循的就是这条规则。

2026.8.29.1 之前 host 模块这条路径注册的是裸 `package.name`,于是声明名与包名
分叉的规则包在 GCC 上能构建、在 Clang 与 MSVC 上失败:GCC 的 BMI 隐式落在
`gcm.cache` 且按**声明名**索引,而另外两者拿到的是显式的 `<name>=<bmi>` 映射。
因此包名不再承担任何 C++ 命名约束,`grpc-rules` 重新是合法包名。

**两个规则不得声明同一个模块名。** `import` 寻址的是模块,所以两个这样的包对编译器
不可区分,而它们的 BMI 与对象文件同名 —— 后者覆盖前者,存活的那个对象被送进链接两次。
mcpp 拒绝这种情形,并点名两个包与各自的 interface 路径。检查的范围是一次 `build.mcpp`
能看见的那些规则,不是索引级的全局唯一性 —— `path` 依赖与私有 registry 本来就绕得开。

**`mcpp.` 前缀保留给由 mcpp 项目维护的规则。** 不在 `mcpp` 命名空间下的包声明该前缀
的模块名时给出一条同时点名两者的警告,构建继续。之所以是警告:引擎判定不了谁是官方,
`path` 依赖、私有镜像与内部 fork 都合法,而且从这里看都一样。

lib 根必须在 `src/<name>.cppm`(或 `[lib] path` 指向的位置);缺失时报
*"host module 'x': no interface unit at …"*。

**一个包可以提供多条规则,由 feature 选择**(mcpp 2026.9.5.3+)。包解析后的
`[build] sources` 里 —— 含 feature 加入的源文件 —— 每一个模块接口单元都以它自己声明的
名字编成一个 host 模块,lib 根排在最前。feature 单元可以 import lib 根;除此之外每个
单元单独编译,因此只 import `std` 与 `mcpp`。只有写在清单里的源文件参与:未声明
`sources` 的包所推断出的 `src/**` 不被读取,所以此前发布的规则包暴露的仍是它当时暴露
的那一个模块。

```toml
# 集合包的 manifest
[build]
sources = ["src/plugins.cppm"]                   # export module mcpp.plugins;

[features]
rules-cuda  = { sources = ["rules/cuda.cppm"] }  # export module mcpp.rules.cuda;
rules-spirv = { sources = ["rules/spirv.cppm"] } # export module mcpp.rules.spirv;
```

```toml
# 消费者
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-spirv"], host-module = true }
```

**用 `[build-dependencies]` 而不是 `[dependencies]`** —— 规则包正是 [04 §2.6.1](04-mcpp-toml.md) 描述的那种
情形:它的库绝不该到达目标,而它的规则仍然被需要。两条轴是分开的:
`host-module = true` 说的是**要哪一种构建期产物**,而 section 说的是**这个包是否到达
目标**;规则包在第二条轴上的答案是"否",而 section 就是说这件事的地方。写在
`[dependencies]` 里同样能工作 —— 这恰恰是为什么这条区分必须被**陈述**,而不能指望由
一次失败来教会。

模块集合就是 feature 集合:feature 未激活的单元不编译,import 它会以未知模块失败。
`mcpp:plugins` 是 mcpp 项目维护的集合(仓库 `mcpp-community/mcpp-plugins`);其成员
命名为 `mcpp.rules.<x>`(规则包)与 `mcpp.tools.<x>`(构建期工具)。

*仅构建期:* `host-module = true` 的依赖**不会**被编进、也不会被链进本工程的 target,
它所依赖的东西也不会。它只在 `build.mcpp` 期间运行,别处都不出现。(2026.8.5.2 之前
它还会被当作普通库再编一遍,这正是规则里 `import mcpp;` 失败的原因:在那第二次编译里
内置模块并不存在。2026.8.29.1 之前被排除的只有规则本身,它自己的 `[dependencies]`
仍会被编译并链进消费者的二进制,而规则却 import 不到它们。)

### 依赖另一个规则的规则(mcpp 2026.8.29.1+)

规则在自己的 `[build-dependencies]` 里声明所需之物,并可以 import 其中标了
`host-module = true` 的条目:

```toml
# 写在规则包自己的清单里
[build-dependencies]
globbing = { path = "../globbing", host-module = true }
```

```cpp
// 规则自己的接口
export module tidyrule;
import std;
import mcpp;
import globbing;
```

mcpp 先编译内层规则,同一条命令、同一套 flag,因此 BMI 的一致性仍是结构性事实而不是
需要事后校验的性质。

消费者**不可以** import `globbing`:构建期的 provision 只在 `reexport = true` 的边上
再跨一跳,而 mcpp 自己执行这条规则,不交给编译器 —— 在 GCC 上那个 import 会成功,
然后在别人的机器上失败。

*限制:* 每个 host 模块只有一个接口单元。带实现单元或多个模块的库还不能作为规则的
构建期依赖。

### `reexport = true` —— 由库替用户拉起整条工具链(2026.8.6.2+)

上面这些都由**使用工具的人**声明。当知识本来属于库时,这个位置就错了:gRPC
的代码生成需要 protobuf 的 `protoc`,而 gRPC 包的任何使用者都不应该知道这件事。

`reexport = true` 把一条边上的构建期提供物 —— 它的 `tools`、它的
`host-module`、以及该依赖的目录 —— 交给**本包自己的消费者**:

```toml
# 写在 grpc 包自己的 manifest 里
[feature-deps.codegen]
"compat.protobuf" = { version = "35.1",   tools = ["protoc"],          reexport = true }
grpc-plugin       = { version = "1.83.0", tools = ["grpc_cpp_plugin"], reexport = true }
grpcgen           = { version = "1.83.0", host-module = true,          reexport = true }
```

于是使用者只写一行,再 import 那个规则:

```toml
[dependencies]
grpc = { version = "1.83.0", features = ["codegen"] }
```

```cpp
// build.mcpp
import mcpp;
import grpcgen;
int main() { return grpcgen::generate_all() ? 0 : 1; }
```

- **默认关闭,并且刻意不复用边上的 `visibility`。** `visibility` 本身默认就是
  `"public"`,复用该可见性意味着任意深度的依赖都能静默地向构建程序的工具
  命名空间里塞东西。「把某样东西交给消费者」是一条供应链主张,必须写下来。
- **一次声明只走一跳。** 被再导出的提供物到达声明它的那个包的消费者;要继续
  往上走,下一个包必须自己也写 `reexport`。每个包只决定**它**交出什么。
- **feature 可以往一条已经声明过的依赖上追加请求。** gRPC 无条件依赖 protobuf,
  而它的 `codegen` feature 往同一条边加 `tools = ["protoc"], reexport = true`。
  `tools` 与 `features` 取并集,`host-module` 与 `reexport` 取或;`version` /
  `path` / `git` 不合并 —— feature 仍然无法静默覆盖无条件条目的身份。
- **传播的是可见性,不是执行。** `dep_bin()` 只返回路径,跑不跑仍由消费者的
  `build.mcpp` 决定;谁构建了这个工具、tool store 怎么做键,都不改变。
- **裸名由阶梯决定,而不是靠运气。** 一旦两个库都能再导出,它们可能同时提供
  尾名 `protobuf`。全限定的 `MCPP_DEP_<NS>_<NAME>_BIN_<TOOL>` 总是发布;裸名
  依次绑定到 `mcpplibs.<x>`、`compat.<x>`、无命名空间的 `<x>`,最后才是「剩下
  的唯一候选」——存在争用时 mcpp 会说出来,而不是默默选一个。

#### 旧版 mcpp 读到用了这些键的 manifest

不认识的依赖键会被**记为降级**并忽略(mcpp 2026.8.6.2+),因此一份为更新的
mcpp 写的包仍然能加载,这个读取器认识的部分照常生效。在那之前它是**整份加载
失败**且报错误导,这正是「已发布的包永远无法采用新键」的原因——与索引下限确立
的是同一条性质:**数据不得决定程序是否可用**。

因此,一个**依赖** `reexport` 才有那套人机工程的包,仍然需要足够新的客户端;
变化在于该包的其余部分在旧客户端上不再一起失效。

#### 按平台裁剪提供物

一个包可能只在部分平台声明 `bin` 目标。既然现在是**库**决定请求什么,无条件的
请求就会把「不支持的平台」变成用户改不掉的硬错。用条件段裁剪:

```toml
[target.'cfg(not(windows))'.feature-deps.codegen]
"compat.protobuf" = { version = "35.1", tools = ["protoc"], reexport = true }
```

`[target.<sel>.feature-deps.<feature>]`(2026.8.6.2+)与 `[target.<sel>]` 下的
其余依赖表(`dependencies` / `dev-dependencies` / `build-dependencies`)遵循同
一套谓词规则,针对**解析后的 target** 求值。**feature 本身在所有平台都注册**
—— 只有它拉进来的东西是条件性的 —— 因此在没有任何谓词匹配的平台上请求它,不是
「未知 feature」错误。



## 当前边界

- **在主机上运行——交叉构建下也是**(mcpp 0.0.95+)。`mcpp build --target <triple>`
  下,程序用宿主解析的工具链编译、在宿主运行,并看到 `MCPP_TARGET` = 交叉三元组。
  纯声明式的目标门控仍首选 `[target.'cfg(...)']` 表——参见
  [04 - mcpp.toml 工程文件指南](04-mcpp-toml.md)。
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
