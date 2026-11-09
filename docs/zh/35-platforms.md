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
映像是一种发布形态,原生行是保证相同的构建目标,二者不分高低([24](24-openkal-cross.md),"两种角色,不分等级")。

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
             the platform's (openkal-platform 0.24.0), replacing your gcc@16.1.0 — override with `[target.x86_64-openkal] toolchain`
      Target x86_64-openkal → x86_64-unknown-none-elf
             compiler-runtime  compiler-rt    (openkal-llvm-runtime@0.19.0, platform)
             c-abi             musl           (openkal-musl@0.28.0, platform)
             c++-abi           libc++         (openkal-llvm-runtime@0.19.0, platform)
    Platform openkal-platform 0.24.0 (built-in)
             tools             okld, okprof, okrun (openkal-tools@0.5.0, platform)
             runner            okrun (platform)
             link              lld, start ok_main (platform)
   Compiling app v0.1.0 (.)
     Running `okrun … target/x86_64-openkal/…/bin/app.oke`
```

每个值都带来源报告。`mcpp test`、`mcpp pack` 与 `--target aarch64-openkal` 无需更多声明；与本机架构不同的
映像经执行宿主运行（[24](24-openkal-cross.md)）。`mcpp.lock` 以
`origin = "platform"` 记录平台加入的包,并以 `targets` 写出每个条目的配置(只属于本机原生配置的条目不写),
因此为本机的构建与为 ok 映像的构建不互相改写对方的条目。

## 平台陈述的内容

| 组 | 键 | 作用 |
|---|---|---|
| 目标 | `targets`、`toolchain`、`sysroot` | 服务哪些三元组、它们使用的工具链、预构建的目标基 |
| 层 | `[platform.layers]` | `c-abi`、`c++-abi`、`compiler-runtime`、`kernel-abi` 的提供包,或 `false` |
| 工具 | `[platform.tools]` | 为本机构建的程序,或查找到的程序(`{ program = "<name>" }`) |
| 编译 | `[platform.build]`、`[platform.build.'cfg(…)']` | 目标每个翻译单元的标志，含 std 模块（2026.11.3.1） |
| 链接 | `[platform.link]` | `driver`、`flags`、`start`、`script`、`inputs`、名字 `exe` 与 `shared`、封装步骤 `seal` |
| 运行 | `runner`、`runners` | `mcpp run` 与 `mcpp test` 怎样启动产物,以及具名的设备动作 |
| 载体 | `carrier` | `mcpp pack` 为依赖加载器的产物按系统构建的程序:self-contained 树的运行器、单文件;一个表或表的列表,每项服务其 `hosts` 中的 `<os>-<arch>` |
| 运行器存在之处 | `runner.hosts` | 运行器程序存在的 `<os>-<arch>`:需要它的包在哪里运行(`runs_on.with`,[10](10-pack-and-release.md)) |
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
| 命令行 | `--platform openkal-platform@0.24.1`、系列 `--platform openkal-platform@^0.24`、`--platform path:../openkal-platform`、`--platform none` |
| 环境变量 | `MCPP_TARGET_X86_64_OPENKAL_PLATFORM=…` |
| 工程 | `[target.<selector>] platform = "openkal-platform@0.24.0"`,或 `{ path = "…" }` |
| 本机 | `~/.mcpp/config.toml` 中的同一个键 |
| 目标表 | 平台列(`mcpp toolchain list`) |

`platform` 不能写在 `cfg(platform = …)` 选择器下。`none` 只以引擎对该目标的约定构建。

## 宿主图的平台

构建程序与构建运行的工具按本机的约定为本机编译。`[host] platform` 为它们选择一个平台:

```toml
[host]
platform = "openkal-platform@0.24.0"
```

```
        Host platform openkal-platform 0.24.0 (mcpp.toml:2): build programs are x86_64-openkal images run by okrun
```

此后每个构建程序都编译为本机架构的 ok 映像,在 Linux、macOS 与 Windows 上使用同一个工具链,经平台的运行器
运行。平台自己的工具仍以原生方式构建。`--host-platform` 与 `MCPP_HOST_PLATFORM` 对一次运行选择它,
`~/.mcpp/config.toml` 中的 `[host]` 对一台机器选择它。

## 宿主上的安排

宿主三元组选用一个平台时,只适用平台的 `[platform.hosted]` 部分:它的工具链与它的层。对 openkal-platform 而言,
即家族的 C 库与 C++ 运行时,位于系统自己的 openkal 实现之上:

```toml
[target.'cfg(any(linux, macos, windows))']
platform = "openkal-platform@0.24.0"
```

Windows 上这一安排对应 GNU ABI 的行(`--target x86_64-windows-gnu`);`[platform.hosted] targets` 之外的
三元组被拒绝,消息给出它所服务的同系统同架构的行。

## 版本组

`[platform.versions]` 为一个家族列出每个包的一个版本。从 mcpp 2026.11.7.1 起由版本组选择版本:平台是依赖图解析的
第一个包,此后对版本组所列包的每个版本请求,只要其约束容许组中版本,就解析为组中版本,即使索引中有更新的补丁。
openkal 的包之间以兼容范围互相依赖(`openkal-musl = "^0.28.0"`),所以构建链接的就是版本组选定的版本。组中版本
不满足的请求在任何编译之前被拒绝:

```
error: openkal-musl is requested as '^0.27.0' by app, and the platform openkal-platform 0.24.1 is a version group
       that names openkal-musl 0.28.1.
       The group chooses one version of each package of the family for a
       build. Either select the platform release of the version you need
       ([target.<selector>] platform = "openkal-platform@<version>"), or admit 0.28.1 where
       it is requested (openkal-musl = "^0.28.1").
```

只为本机构建的包(工具的)以及从目录或仓库取得的包不受版本组约束。早于 2026.11.7.1 的 mcpp 改为在解析之后检查
版本组;一个家族以一次索引改动整组发布时,两者结果相同。

## 内置平台是一个版本

目标表为两个 ok 映像目标选择一个 openkal-platform 版本,即当前 mcpp 验证过的版本:mcpp 2026.11.8.1 为 0.24.4,2026.11.7.2 为 0.24.3
(2026.11.7.1 选择系列 `^0.24`)。不写平台的工程在任何时候、有无 `mcpp.lock` 都构建同一组版本;锁中记录了另一个版本时,
构建采用默认版本并写出锁中记录的版本。更新的平台不需要新的 mcpp:工程在自己的清单中选择,

```toml
[target.'cfg(os = "openkal")']
platform = "openkal-platform@0.24.4"     # 一个版本,或一个系列:"openkal-platform@^0.24"
```

使用默认平台的构建失败、且已同步的索引中有更新版本时,最后输出一行 `tip:`,写出两个版本与上面这一行。

系列取其中 `[package] mcpp` 下限当前 mcpp 满足的最新版本,并写出因需要更新的引擎而跳过的版本;`mcpp.lock` 记录结果,
之后的构建在系列容许时采用记录的版本。`mcpp update`(或 `mcpp update openkal-platform`)删除这条记录,下一次构建重新
取最新版本。

## 编写平台包

平台包是带 `[platform]` 段的包。openkal-platform 的节选:

```toml
[package]
namespace = "mcpplibs"
name      = "openkal-platform"
version   = "0.24.0"
mcpp      = ">=2026.11.3.1"

[platform]
targets   = ["x86_64-openkal", "aarch64-openkal"]
toolchain = "llvm@23.1.3"
runner    = { cmd = ["okrun", "{}"], host = "same-arch", hosts = ["linux-x86_64", "linux-aarch64", "macos-aarch64", "windows-x86_64"] }
carrier   = { package = "openkal-run@0.5.0", section = ".oke", main = "carrier/main.cpp" }
plugin    = "build.mcpp"

[platform.layers]
compiler-runtime = "openkal-llvm-runtime@0.19.0"
"c++-abi"        = "openkal-llvm-runtime@0.19.0"
c-abi            = { name = "openkal-musl", version = "0.28.0", default-features = false, features = ["posix", "malloc-mimalloc"] }

[platform.tools]
okld  = "openkal-tools@0.5.0"
okprof = "openkal-tools@0.5.0"
okrun = "openkal-tools@0.5.0"

[platform.build]
cxxflags = ["-fPIC", "-femulated-tls", "-fvisibility=hidden", "-fasynchronous-unwind-tables", "-Xclang", "-fstack-clash-protection", "-ffunction-sections", "-fdata-sections", "-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer", "-fstack-protector-strong", "-mstack-protector-guard=global", "-ftrivial-auto-var-init=zero", "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST"]

[platform.build.'cfg(arch = "aarch64")']
cxxflags = ["-ffixed-x18", "-mno-outline-atomics", "-mbranch-protection=pac-ret+bti"]

[platform.link]
driver = "lld"
start  = "ok_main"
flags  = ["-shared", "-z", "max-page-size=65536", "-z", "pack-relative-relocs", "-z", "now", "-z", "noexecstack", "--hash-style=both", "--gc-sections", "--icf=safe", "--eh-frame-hdr", "--build-id=sha1", "--emit-relocs", "-mllvm", "-emulated-tls"]
inputs = [{ from = ["okld", "note-source"], as = "ok-notes.S" }]
exe    = "{name}.oke"
shared = "lib{name}.okso"
seal   = [["okld", "note", "{}"], ["okld", "place", "{}"], ["okld", "check", "{}", "--with", "{needed}"]]
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
  (入口的名字在 `mcpp_carried_name`):程序为 self-contained 树的运行器时不携带任何内容,运行其第一个参数所指的映像;
  或映像本身;或映像引用其他映像时的单文件所携带的全部映像的一个 stored zip(`PK\x03\x04`,每个映像对齐到
  64 KiB)([SPEC-013](../specs/platforms.md) §11)。

开发期间,工程以 `--platform path:<dir>` 或 `platform = { path = "<dir>" }` 选择工作树。在平台包中执行
`mcpp test` 而不给 `--target` 时,以该包为平台为其每个 `targets` 构建测试,运行本机架构的那些与给出了执行宿主的
那些(2026.10.29.1+),其余报告为已构建、未运行。

## 定义目标与提供者（2026.11.9.1+）

平台可以服务内置行未命名的系统。`[[platform.define]]` 陈述行，`[[platform.provider]]` 陈述服务它的提供者：

```toml
[[platform.define]]
triple        = "x86_64-freebsd"
clang-triple  = "x86_64-unknown-freebsd14.1"
object-format = "elf"
family        = "unix"
execution     = "freebsd"          # 其程序运行的系统

[[platform.provider]]
serves   = ["x86_64-freebsd"]      # 模式：`*` 代表任一段
priority = 100
hosted   = true                    # C 环境有 `main`
```

选中该平台（`[target.x86_64-freebsd] platform = {...}` 或 `--platform`）后，`--target x86_64-freebsd` 构建到
`target/x86_64-freebsd/`，`cfg(os = "freebsd")` 成立；程序的系统不是本机时，在运行器陈述运行方式之前为「已构建、
未运行」（docs/41）。行的机器描述属于平台：机器与内置行不同的目标是另一行。`[platform]` 与 `[platform.hosted]` 仍是
提供者的两种简写。

`mcpp new <名> --template platform` 生成一个定义 `<arch>-<名>` 的平台包。读取平台清单时按行检查其语句：定义内置行、
同一行定义两次是错误，无论平台在何处被读取；不服务任何行的提供者、以同一优先级服务同一身份的两个提供者是平台自身构建的
警告。`mcpp why platform` 列出平台定义的行与所服务的身份，各附提供者与优先级：在平台目录中是其自身，在项目中是其目标所选
的平台（`--target`；`--format json`）。

## 查看一个配置

```sh
mcpp self env --target x86_64-openkal                 # toolchain, platform, tools, runner, and why runs are refused here
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
