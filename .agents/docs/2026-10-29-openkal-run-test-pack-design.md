---
subject: platform
status: active
---

# 另一架构的 ok 映像的运行、测试与打包，以及 ok 映像与 Web 的关系（2026.10.29.1）

日期：2026-10-29。基线：mcpp `7b5f10aa`（2026.10.28.1 + #858）。前文：[platforms design](2026-10-28-platforms-design.md)（SPEC-013）、
[part 2](2026-10-28-platforms-part2-packaging-and-distribution-design.md)（SPEC-014）。相关规范：SPEC-002 §6（执行宿主）、SPEC-013、SPEC-014。

**修订记录**

- rev 1：七项的实测与定案；一个发布 PR（#859）。
- rev 2（本稿，评审）：§2.6 的 `emit xpkg` 规则改为按**程序**运行的系统描述。带载体的单腿 ok 映像包（M3）是载体所属系统的
  程序，rev 1 把它一并拒绝，对在 Linux 上打出、此前可用的条目是回归；包现在在 `systems` 中记录其载体的 `<os>-<arch>`，条目
  读这一事实，只拒绝不带载体的 ok 映像包。

## 0. 摘要

2026.10.28.1 在 x86_64 Linux 上对 `aarch64-openkal` 的审计发现七处缺陷，全部源于同一个假设：运行器的程序、载体与列出的格式都按
**本机**决定，而它们应当按**目标**决定。本批的规则：`same-arch` 的运行器在产物架构的机器上运行，给出执行宿主时其程序是目标架构的
（平台载体的运行器形式）；单目标的 self-contained 包的载体是映像架构的；`--list-formats` 与 `self env` 报告所选目标的配置；
`mcpp test` 的人类输出给出完整的拒绝原因一次。Web 不是 ok 映像的目标：ok 映像只有 x86_64 与 aarch64，Web 经 `wasm32-emscripten` 与
`dist-web` 到达；`emit xpkg` 与 `--pkg-format single-file` 拒绝 WebAssembly 包。

## 1. 实测（本机构建的 2026.10.28.1，x86_64 Linux，`/usr/bin/qemu-aarch64` 8.2.2）

| 编号 | 观察 | 位置 |
|---|---|---|
| F1 | `mcpp run`、`mcpp test --target x86_64-openkal`、`mcpp run` 依次运行，第三条打印 `warning: fingerprint changed (28312234d2e59c78 → 7ff323110ea05b87), full rebuild`：测试前构建包的程序时，缓存条目的 `[target=]` 为空 | `src/build/execute.cppm:4319`，`run_build_plan(*binCtx, …, false)` 缺第四个参数；`mcpp run` 在 3187–3205 修过同一缺陷 |
| F2 | `mcpp pack --target aarch64-openkal --no-universal --mode self-contained` 写出 `bin/app: ELF 64-bit LSB executable, x86-64`，携带 aarch64 映像，在任何机器上都不能运行 | `src/pack/pack.cppm:2517–2519` 传 `host_arch`；单文件（3258）取映像的架构 |
| F3 | `mcpp pack --target aarch64-openkal --list-formats` 列出 `tar, zip, dir, single-file`，没有 `okb` | `src/cli/cmd_publish.cppm:274–292` 只设 profile 与 features |
| F4 | `mcpp test --target aarch64-openkal` 的摘要只有原因的第一行，`run_under` 的写法没有出现；注释称全文已在确立时打印，而该原因（`invocationNotRunReason`）在 `test_resolve_runner` 中被静默记录 | `execute.cppm:3704–3705`、`4185–4189` |
| F5 | `MCPP_TARGET_AARCH64_OPENKAL_RUN_UNDER=qemu-aarch64 mcpp run --target aarch64-openkal`：`qemu-aarch64: …/openkal-tools@0.1.3/…/bin/okrun: Invalid ELF image for this architecture`，退出 255 | 运行器首词 `okrun` 总是解析到 `platform_tool`（`execute.cppm:1078–1084`，用于 2876、3716），即为本机构建的工具 |
| F6 | 同一环境下 `mcpp self env --target aarch64-openkal` 报告 `runner okrun {} (platform) -> …/okrun`（x86_64），不说明本机拒绝运行，也不说明给出执行宿主后用哪个程序 | `src/cli/cmd_build.cppm` 的 `cmd_env_configuration` |
| F7a | `mcpp emit xpkg --package web-0.1.0-wasm32-emscripten.tar.gz` 写出 `archs = {"wasm32"}`、只有 `linux` 一项 | `src/pack/distribution.cppm` 的 `xlings_system`：不是 Windows、macOS 的三元组一律为 `linux` |
| F7b | `--pkg-format single-file` 在 wasm 目标上没有设计也没有拒绝，代码会去构建 `wasm32-linux-musl` 的启动器 | `pack.cppm:2264–2268`、`3305–3309` |

F5 的对照：把运行器手工改为 `cmd = ["qemu-aarch64", "<aarch64 的 okrun>", "{}"], host = "any"`，其中程序取 self-contained 通用包的
`libexec/okrun/linux-aarch64/okrun`，`mcpp run` 与 `mcpp test` 通过。

## 2. 定案

### 2.1 运行器的程序按执行宿主的架构决定（F5、F6）

`host = "same-arch"` 的含义是"产物在其架构的机器上运行"。给出执行宿主（`run_under`，含 `MCPP_TARGET_<T>_RUN_UNDER`）时，运行器在
执行宿主上运行，执行宿主的架构即产物的架构，所以运行器的程序**必须**是该架构的。规则只看三件事：运行器是否 `same-arch`、目标架构
是否等于本机架构、是否给出了执行宿主；`--no-runner` 声明三元组在本机是原生的，不经运行器。三者的组合只有三种结果——本机的程序、目标
架构的程序、拒绝——由 `mcpp.platform` 的一个纯函数 `runner_reach` 回答，拒绝（`unreachable_execution`）、运行器程序的解析
（`mcpp run`、`mcpp test`）与 `self env` 都读它，单元测试按表覆盖。

目标架构的程序是平台的载体以运行器形式（不携带任何内容，SPEC-013 §11）为**本机系统**与**目标架构**构建的程序，与 self-contained
通用包放在 `libexec/<runner>/<os>-<arch>/` 中的是同一个程序，由同一个函数构建（`carrier_build`，经新导出的
`mcpp::pack::runner_program`）。系统取本机的：执行宿主由本机经 `run_under` 到达，mcpp 不知道它的系统；用户态模拟器（`qemu-aarch64`）
与同系统的远程机器两种情形都成立。只对运行器首词是平台工具的情形改用目标架构的程序：首词是别的程序时，它不是平台构建的，mcpp
没有它的其他架构的版本。平台没有 `carrier`、或本 mcpp 不为该系统与架构构建程序（`row_is_built`）时，运行被拒绝并写出原因，而不是
退回本机的程序（退回即 F5）。`host = "any"` 的运行器不受影响：它从任何机器到达产物，其程序是本机的。

`self env --target` 报告运行器程序的架构；给出执行宿主时报告目标架构的程序与它是否已构建（第一次运行时构建）以及执行宿主的程序；
不给时报告拒绝的原因。`self env` 只准备不构建，不为报告而构建载体。

平台包中不带 `--target` 的 `mcpp test` 原本在 CLI 层对其他架构的目标设 `noRun`；它改为交给 `run_tests` 按同一规则判断：拒绝时
报告为已构建未运行（原行为），给出执行宿主时运行。

### 2.2 单目标 self-contained 包的载体是映像架构的（F2）

载体程序的架构取映像的（`arch_of(plan.triple)`），系统取本机的，与单文件一致。本 mcpp 不构建该行（如 Windows 主机上的
`aarch64-windows-gnu`）时 `carrier_build` 直接拒绝并写出系统、架构与行，不再进入生成工程的打包后才失败。

### 2.3 `--list-formats` 准备所选目标（F3）

`--target`（多次给出时取最后一个，与打包相同）进入准备；平台阶段声明的格式（`okb`）因此出现在列表中。

### 2.4 `mcpp test` 的拒绝原因给出一次全文（F4）

原因在运行阶段开始、第一个测试被报告为未运行之前，以警告打印一次全文（每次构建一次，工作空间的多个成员共用同一次构建时也是一次）；
摘要保留第一行。与内核拒绝 spawn 的情形（`hostCannotRunReason`）相同，于是摘要处的注释成立。

### 2.5 F1

`run_build_plan(*binCtx, …, overrides.target_triple)`。

### 2.6 WebAssembly 包（F7a、F7b）

**`emit xpkg`：拒绝。** SPEC-014 §8 的条目有两种形状：通用包（启动器在包内，经平台运行器启动映像，三个系统同一资产）与按系统的
包（`xpm.<system>` 安装该系统的程序，`archs` 为机器架构）。`wasm32-emscripten` 的包两者都不是：它没有启动器，`bin/<name>.js` 由
JavaScript 引擎或浏览器执行，`wasm32` 不是任何系统的机器架构，xvm 注册的 `bin/<name>` 不存在。把它描述成系统无关的条目需要为 wasm
生成启动器并在条目中写运行器前缀，即一个新形状，而 Web 的分发路径是 `dist-web` 产出的页面，不是 xlings。规则按模型写成一般形式：
单腿包按其**程序**运行的系统与架构描述。腿为 Linux（非 Android）、macOS 或 Windows 的三元组时取三元组的；腿为 ok 映像而包携带
载体（`self-contained`、`static`）时，程序是载体，为本机系统、映像架构的程序，包在 `systems` 中记录它（`<os>-<arch>`，与通用包
记录 `libexec/` 中加载器的字段相同），条目读这一记录而不推测。程序不是任何系统的程序时拒绝：WebAssembly、Android、iOS、裸机，
以及不带载体的 ok 映像包（一个 `.oke` 需要运行器，出路为通用包）；此前它们都被写成 `linux` 或 `macosx`。

**`--pkg-format single-file`：拒绝。** 单文件是一个系统执行的可执行文件（原生启动器或载体）；WebAssembly 模块不是。拒绝在
`refused_combination` 中，任何构建之前，写出两个选择与出路（`tar`、`zip`、`dir` 或提供者的格式，页面为 `dist-web`）。

### 2.7 ok 映像与 Web（文档）

ok 映像是 x86_64 与 aarch64 上位置无关的 ELF，只导入 `kal_*`，需要加载器发布的可执行内存；okb 只装 ok 映像。Web 经
`wasm32-emscripten` 目标（打包为 `dist-web`）到达，使用 openkal API 的代码经 openkal-emscripten 实现到达。SPEC-013 §14、SPEC-014 §0
与 docs/24 各写一句。

## 3. 测试

| 项 | 单元 | e2e |
|---|---|---|
| F1 | — | 960 A：`run`、`test --target`、`run` 不出现 fingerprint 警告，缓存含该目标的条目 |
| F4 | — | 960 B：完整原因（含 `run_under` 的写法）恰好一次 |
| F5 | `PlatformRun.TheRunnerReachesTheArtifactsArchitecture`（`runner_reach` 的表） | 960 D：记录 argv 的执行宿主收到的运行器程序是另一架构的 ELF；本机有该架构的 qemu 时真实运行 `run` 与 `test` |
| F6 | — | 960 C：人类输出给出拒绝；JSON 的 `runner.refusal`、`runner.arch`；给出执行宿主时为另一架构 |
| F2、F3 | — | 961：载体为映像架构的 ELF（有 qemu 时运行它）；`--target <t>-openkal --list-formats` 含 `okb`，本机目标不含 |
| F7a | `XpkgDescriptorDescribesProgramsOfASystem`（含 Linux 与 macOS 上打出的带载体包、不带载体的包） | 962 A：合成的 wasm32-emscripten 包被拒绝，消息写出三元组与出路；961 C：带载体的包的 `systems` 与条目，不带载体的包被拒绝 |
| F7b | — | 962 B：有 emsdk 载荷时 `--pkg-format single-file --target wasm32-emscripten` 在编译前被拒绝 |

## 4. 文档

SPEC-013 1.2（§10 规则、§11 单目标包的架构、§14 ok 映像与 Web、§16 `self env` 与平台包的 `mcpp test`、§18 判据 7 与 12）；
SPEC-014 1.3（§0 ok 映像与 Web、§1.2、§1.5 两行、§5.2、§8 的系统规则、§10 判据 8、9）；docs/10、24、35、41、50 与 docs/zh 对应章节；
docs/90 的计数；CHANGELOG 2026.10.29.1（并入 Unreleased 的 #858）。
