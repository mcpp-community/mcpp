# 35 —— 平台

**读者:** 为一个由平台包配置的目标构建的人(ok 映像,`<arch>-openkal`)、要改动该配置一部分的人,
以及为一类目标编写平台包的人。

**本章回答的那一个问题:** 一类目标怎样作为一个包来配置,构建怎样选择它,工程怎样覆盖或替换它。

**不在这里:** 平台所命名的目标侧各层,见 [22 —— 目标侧](22-target-side.md);openkal 的模型与宿主上的
交叉构建,见 [24 —— 基于 openkal 的交叉构建](24-openkal-cross.md);打包一个程序,见
[10 —— 发布打包](10-pack-and-release.md);精确规则,见 [SPEC-013](../specs/platforms.md)。

在此之前:[34 —— 编写板级支持包](34-authoring-a-bsp.md)。
在此之后:[40 —— 裸机与 freestanding 目标](40-baremetal.md)。

**平台**是一个带版本的包,陈述一类目标所需的全部内容:工具链、目标侧各层的提供包、构建在本机运行的
工具、编译与链接约定、产物的命名与封装,以及运行器。工程一项都不必写。目标表为 `x86_64-openkal` 与
`aarch64-openkal` 选择 `mcpplibs.openkal-platform`(2026.10.27.1+),这两个目标的内置支持即是如此。

## 最短可跑

一个只写出源码的工程:

```toml
[package]
name    = "app"
version = "0.1.0"
```

```sh
mcpp run --target x86_64-openkal
```

```
    Resolved llvm@23.1.3 → x86_64-openkal → …/xim-x-llvm/23.1.3/bin/clang++
             the platform's (openkal-platform 0.17.3), replacing your gcc@16.1.0 — override with `[target.x86_64-openkal] toolchain`
      Target x86_64-openkal → x86_64-unknown-none-elf
             compiler-runtime  compiler-rt    (openkal-llvm-runtime@0.16.6, platform)
             c-abi             musl           (openkal-musl@0.22.6, platform)
             c++-abi           libc++         (openkal-llvm-runtime@0.16.6, platform)
    Platform openkal-platform 0.17.3 (built-in)
             tools             okld, okrun (openkal-tools@0.1.3, platform)
             runner            okrun (platform)
             link              lld, start ok_main (platform)
   Compiling app v0.1.0 (.)
     Running `okrun … target/x86_64-openkal/…/bin/app.oke`
```

每个值都带来源报告。`mcpp test`、`mcpp pack` 与 `--target aarch64-openkal` 无需更多声明。`mcpp.lock` 以
`origin = "platform"` 记录平台加入的包,并以 `targets` 写出每个条目的配置(只属于本机原生配置的条目不写),
因此为本机的构建与为 ok 映像的构建不互相改写对方的条目。

## 平台陈述的内容

| 组 | 键 | 作用 |
|---|---|---|
| 目标 | `targets`、`toolchain`、`sysroot` | 服务哪些三元组、它们使用的工具链、预构建的目标基 |
| 层 | `[platform.layers]` | `c-abi`、`c++-abi`、`compiler-runtime`、`kernel-abi` 的提供包,或 `false` |
| 工具 | `[platform.tools]` | 为本机构建的程序,或查找到的程序(`{ program = "<name>" }`) |
| 编译 | `[platform.build]`、`[platform.build.'cfg(…)']` | 目标每个翻译单元的标志 |
| 链接 | `[platform.link]` | `driver`、`flags`、`start`、`script`、`inputs`、名字 `exe` 与 `shared`、封装步骤 `seal` |
| 运行 | `runner`、`runners` | `mcpp run` 与 `mcpp test` 怎样启动产物,以及具名的设备动作 |
| 载体 | `carrier` | `mcpp pack` 为依赖加载器的产物按系统构建的程序:self-contained 的程序、通用包的运行器、单文件 |
| 包 | `universal` | 任一目标的打包覆盖全部目标,成为一个包([10](10-pack-and-release.md)) |
| 代码 | `plugin` | 在平台阶段运行的构建程序,陈述随依赖图而变的部分 |
| 版本组 | `[platform.versions]` | 一个家族中每个包的一个版本 |
| 宿主部分 | `[platform.hosted]` | 宿主三元组选用该平台时适用的部分 |

## 覆盖平台的一部分

工程在 `[target.<selector>]` 下以平台自己的键名覆盖一个字段。`cfg(platform = "openkal")` 不论架构都匹配该平台:

```toml
[target.'cfg(platform = "openkal")'.layers]
c-abi = { path = "../openkal-musl" }         # a working tree in place of the release

[target.'cfg(platform = "openkal")'.tools]
okld = { path = "../openkal-image/tools" }

[target.'cfg(platform = "openkal")']
runner = ["my-okrun", "{}"]
```

| 字段 | 规则 |
|---|---|
| 编译标志、`link.flags` | 追加在平台的之后;约定不能删除 |
| `link.inputs`、`link.seal` | 整体替换 |
| `layers`、`tools`、`runners` | 按键替换;`<layer> = false` 不提供该层 |
| `runner` | argv 替换;未写出时保留 `host` 与 `exclusive` |
| 其余字段 | 替换 |

同样的表也可以写在 `~/.mcpp/config.toml` 中,对一台机器生效。图中已有包提供的层交给该包,报告为 `graph`。
C++ 运行时的包同时提供编译器运行时,因此 `"c++-abi" = false` 只有与 `compiler-runtime = false` 同时写出才生效。

## 选择平台,或不用平台

第一个给出答案的来源决定目标的平台:

| 来源 | 写法 |
|---|---|
| 命令行 | `--platform openkal-platform@0.17.3`、`--platform path:../openkal-platform`、`--platform none` |
| 环境变量 | `MCPP_TARGET_X86_64_OPENKAL_PLATFORM=…` |
| 工程 | `[target.<selector>] platform = "openkal-platform@0.17.3"`,或 `{ path = "…" }` |
| 本机 | `~/.mcpp/config.toml` 中的同一个键 |
| 目标表 | 平台列(`mcpp toolchain list`) |

`platform` 不能写在 `cfg(platform = …)` 选择器下。`none` 只以引擎对该目标的约定构建。

## 宿主图的平台

构建程序与构建运行的工具按本机的约定为本机编译。`[host] platform` 为它们选择一个平台:

```toml
[host]
platform = "openkal-platform@0.17.3"
```

```
        Host platform openkal-platform 0.17.3 (mcpp.toml:2): build programs are x86_64-openkal images run by okrun
```

此后每个构建程序都编译为本机架构的 ok 映像,在 Linux、macOS 与 Windows 上使用同一个工具链,经平台的运行器
运行。平台自己的工具仍以原生方式构建。`--host-platform` 与 `MCPP_HOST_PLATFORM` 对一次运行选择它,
`~/.mcpp/config.toml` 中的 `[host]` 对一台机器选择它。

## 宿主上的安排

宿主三元组选用一个平台时,只适用平台的 `[platform.hosted]` 部分:它的工具链与它的层。对 openkal-platform 而言,
即家族的 C 库与 C++ 运行时,位于系统自己的 openkal 实现之上:

```toml
[target.'cfg(any(linux, macos, windows))']
platform = "openkal-platform@0.17.3"
```

Windows 上这一安排对应 GNU ABI 的行(`--target x86_64-windows-gnu`);`[platform.hosted] targets` 之外的
三元组被拒绝,消息给出它所服务的同系统同架构的行。

## 版本组

`[platform.versions]` 为一个包与包之间精确钉住的家族列出每个包的一个版本。依赖图中出现其中某个包的
另一个版本时,在任何编译之前拒绝:

```
error: openkal-musl 0.22.2 is in this build, and the platform openkal-platform 0.17.3 is a version group that names openkal-musl 0.22.6.
       requested by: own
       The family's packages pin one another exactly, so two versions of one of
       them cannot be linked into one program. Either select the platform release
       of that version ([target.<selector>] platform = "openkal-platform@<version>"), or write
       openkal-musl = "0.22.6" where it is requested.
```

从目录或仓库取得的包不受版本组约束。

## 编写平台包

平台包是带 `[platform]` 段的包。openkal-platform 的节选:

```toml
[package]
namespace = "mcpplibs"
name      = "openkal-platform"
version   = "0.17.3"
mcpp      = ">=2026.10.27.1"

[platform]
targets   = ["x86_64-openkal", "aarch64-openkal"]
toolchain = "llvm@23.1.3"
runner    = { cmd = ["okrun", "{}"], host = "same-arch" }
carrier   = { package = "openkal-run@0.1.3", section = ".oke", main = "carrier/main.cpp" }
universal = true
plugin    = "build.mcpp"

[platform.layers]
compiler-runtime = "openkal-llvm-runtime@0.16.6"
"c++-abi"        = "openkal-llvm-runtime@0.16.6"
c-abi            = { name = "openkal-musl", version = "0.22.6", default-features = false, features = ["posix"] }

[platform.tools]
okld  = "openkal-tools@0.1.3"
okrun = "openkal-tools@0.1.3"

[platform.build]
cxxflags = ["-fPIC", "-femulated-tls", "-fvisibility=hidden", "-fasynchronous-unwind-tables"]

[platform.build.'cfg(arch = "aarch64")']
cxxflags = ["-ffixed-x18", "-mno-outline-atomics"]

[platform.link]
driver = "lld"
start  = "ok_main"
flags  = ["-shared", "-z", "max-page-size=65536", "-z", "now", "--gc-sections", "--icf=safe", "--eh-frame-hdr"]
inputs = [{ from = ["okld", "note-source"], as = "ok-notes.S" }]
exe    = "{name}.oke"
shared = "lib{name}.okso"
seal   = [["okld", "note", "{}"], ["okld", "check", "{}", "--with", "{needed}"]]
```

- **程序写名字,不写位置。** `inputs` 与 `seal` 中的 `okld` 依次在图为本机构建的程序、声明的载荷、`PATH` 中查找。
- **`{}` 是产物。** 封装步骤中的 `{needed}` 展开为产物所需的共享映像;它前面的 `--with` 在每个映像前重复。
- **封装步骤失败时不发布。** 上一次成功构建发布的产物保留原位。
- **随依赖图而变的部分写成代码。** `plugin = "build.mcpp"` 在平台阶段运行(`mcpp::phase()` 为 `"platform"`),
  可以陈述标志、链接字段与运行器(`mcpp::link_driver`、`mcpp::link_input_from`、`mcpp::artifact_name`、
  `mcpp::seal`、`mcpp::run_host`);其他指令被拒绝,平台提供的包格式除外(`pack-format` 与 `artifact`
  action,例如由暂存树生成 `okb`)。板级支持包的构建程序在构建阶段陈述同样的字段
  ([34](34-authoring-a-bsp.md))。
- **载体的入口读取它携带的内容。** mcpp 把携带的字节放在 `mcpp_carried_begin` 与 `mcpp_carried_end` 之间
  (入口的名字在 `mcpp_carried_name`):程序为通用包的运行器时不携带任何内容,运行其第一个参数所指的映像;
  或映像本身;或映像引用其他映像时的单文件所携带的全部映像的一个 stored zip(`PK\x03\x04`,每个映像对齐到
  64 KiB)([SPEC-013](../specs/platforms.md) §11)。

开发期间,工程以 `--platform path:<dir>` 或 `platform = { path = "<dir>" }` 选择工作树。在平台包中执行
`mcpp test` 而不给 `--target` 时,以该包为平台为其每个 `targets` 构建测试,运行本机架构的那些,其余报告为
已构建、未运行。

## 查看一个配置

```sh
mcpp self env --target x86_64-openkal                 # toolchain, platform, tools, runner
mcpp self env --target x86_64-openkal --format json
mcpp self env --host                                  # the host graph, its runner and tools once its build programs were built
mcpp toolchain list                                   # the PLATFORM column
mcpp toolchain install --target x86_64-openkal        # fetch and build what the target needs
```

`--message-format json` 在 `plan` 事件与 `result.json` 中给出 `platform` 与 `hostPlatform`
([50](50-machine-output.md))。

## 拒绝

| 原因 | 条件 | 出路 |
|---|---|---|
| `platform-not-a-platform` | 所选的包没有 `[platform]` | 选择一个平台包,或 `none` |
| `platform-target-unserved` | 平台不服务该三元组 | 为它的某个 `targets` 构建,或为消息给出的宿主行构建 |
| `platform-tool-absent` | 链接或封装要运行的程序既未构建、也未找到 | 在 `[target.'cfg(platform = "…")'.tools]` 下写出构建它的包 |
| `platform-version-group` | 家族中某个包的另一个版本在图中 | 选择携带该版本的平台版本,或钉住版本组中的版本 |
| `platform-phase-directive` | 插件陈述了平台阶段不接受的指令 | 改由包自己的构建程序陈述 |
| `lld-required-absent` | 平台以 `ld.lld` 链接,而工具链没有它 | 使用平台的工具链 |

## 当前边界

- 一个平台的每个字段只有一个值;随架构不同的字段只有标志可以写在 `[platform.build.'cfg(arch = "…")']` 下。
- 宿主图的平台按一次运行或一个工程选择;缺省为宿主约定。
- `[host]` 只接受 `platform`。
