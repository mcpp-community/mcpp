---
subject: design
status: landed
---

# 程序起点模型：启动者、两层入口与 `main` 的三种陈述（mcpp#845、mcpp#981）

**日期：** 2026-11-20 · **实施：** mcpp 2026.11.20.1（mcpp#980） · **前序：** 设计 2026-10-26 §24（mcpp#845）
· **规范：** SPEC-004 §14、SPEC-016 §2.1 · **使用文档：** docs/04 `kind = "entry"`、`main = false`、`start`，docs/21 Web 行

本记录完整陈述 mcpp 对"一个映像从哪里开始执行"的模型：各类目标上的启动者，入口的两层，清单中陈述程序起点的
三种形式与陈述镜像入口的键，以及哪些事实属于行的数据。2026.11.20.1 在 #845 的模型上增加 `main = false`
（mcpp#981）。§2 是模型，§3 是理由，§4 是与业界做法的比较，§5 是不采纳的方案，§7 是写作中发现、仍可讨论之处。

## 1. 问题

每个平台以不同的方式开始执行一个映像：

- **hosted 行**（Linux、macOS、Windows、iOS、BSD）：加载器把控制交给 C 运行时（`_start`、`mainCRTStartup`），
  C 运行时初始化后调用 `main`；PE 上另有入口函数的选择（`main`、`wmain`、`WinMain`、`wWinMain`）。
- **Android 应用**：Java 进程以 `System.loadLibrary` 载入 `lib<name>.so`，经 `JNI_OnLoad`、`Java_*` 函数或
  `ANativeActivity_onCreate` 进入；没有 `main`。Android 的可执行文件（命令行工具）仍由 bionic 的 C 运行时调用 `main`。
- **Web**（`wasm32-emscripten`）：程序形态下 JavaScript 胶水初始化运行时并调用 `main`；模块形态下页面调用模块导出的
  函数或工厂（`--no-entry`），没有 `main`。
- **ok 映像**：okrun 进入格式固定的 `ok_main`，映像内的 C 库调用 `main`。
- **裸机**：ROM、硬件或引导协议跳转到 `_start` 或向量表；有 crt0 时它调用 `main`，内核与引导程序可以没有 `main`。

构建工具需要知道三件事：程序代码从哪个翻译单元开始（它不进入其他映像，它的标志只作用于它，链接时它是否必需）；
没有 C 运行时的映像从哪个符号进入；映像在该行上的形态（可执行文件或共享对象）。#845 回答了前两件的一般情形。
#981 指出剩下的缺口：`app` 在每一行都必须写 `main`，而在启动者不调用程序起点的行上，清单无法陈述"没有"，
框架只能为应用提供一个什么都不定义的翻译单元。

## 2. 模型

### 2.1 启动者与两层入口

每个映像有一个**启动者**：把控制交给映像的一方。入口分两层：

- **L0 镜像入口**：启动者进入映像的位置，一个符号（ELF `e_entry`）或链接脚本放置的向量表。
- **L1 程序起点**：程序代码开始执行的翻译单元。

| 目标 | 启动者 | L0 | L0 的归属 | L1 |
|---|---|---|---|---|
| hosted | 加载器与 C 运行时 | `_start`、`mainCRTStartup` | 工具链与 C 库 | `main`；PE 上由 `windows_entry` 选择入口函数 |
| Android `app` | ART/JNI | 平台约定的导出 | 程序（入口翻译单元或库） | 无 |
| Android `bin` | 加载器与 bionic | `_start` | C 库 | `main` |
| Web 程序形态 | JavaScript 胶水 | 运行时的初始化 | 工具链 | `main` |
| Web 模块形态 | 页面 | 模块导出的函数 | 程序 | 无 |
| ok 映像 | okrun | `ok_main`（格式固定） | 映像内的 C 库 | `main` |
| 裸机，有 crt0 | ROM、硬件或模拟器 | `_start`、`Reset_Handler`、向量表 | 板级包 | `main` |
| 裸机，无 C 库 | 硬件或引导协议 | `_start` 等 | 程序或板级包 | 定义 L0 的翻译单元 |

### 2.2 清单中的陈述

所有陈述都按目标寻址（`[targets.<n>]` 与行表 `[target.<sel>.targets.<n>]`），不进入 `[build]`：入口是单个程序的
身份，库没有入口。

| 陈述 | 层 | 含义 |
|---|---|---|
| `main = "<文件>"` | L1 | 程序代码开始的翻译单元，属于本包。有 C 运行时的映像中它定义 `main`；Android 应用中它是库的一个翻译单元（通常定义平台的导出）；无 C 库的映像中它定义 `start` 所指的符号 |
| `main = { from = "<依赖>/<入口>" }` | L1 | 起点由依赖的一个 `kind = "entry"` 目标提供，在提供方的上下文中编译，链入选择它的程序 |
| `main = false`（2026.11.20.1） | L1 | 映像没有程序起点：启动者只经映像导出的名字进入它 |
| `kind = "entry"` | — | 只作为其他程序起点的翻译单元 |
| `start = "<符号>"` | L0 | 只在没有 C 运行时的行（裸机）上成立；`--require-defined` 使未定义成为链接错误 |
| `windows_entry` | L1（PE） | PE 上入口函数的选择 |
| `mcpp:link-start=<符号>` | L0 | 板级包的构建程序陈述的入口符号 |

### 2.3 行的事实

哪些行允许没有程序起点、怎样链接这样的映像，是行的事实，不是工程的：

- **形态。** `app` 在 Android 行上的形态是共享对象（`application_form`，#622），共享对象本身没有程序起点。
- **`link-without-main`**（SPEC-016 §2.1，v1.7）：可执行映像没有程序起点时加入的链接词。内置的 `wasm32-emscripten` 行为
  `["--no-entry"]`。未写的行上，可执行映像必有程序起点。平台包定义的行以同一个键陈述（`[[platform.define]]`）。
- **谁进入映像。** 拒绝消息按行说明：C 运行时、加载器、入口符号。

### 2.4 规则（SPEC-004 §14 第 6 至 8 条）

1. 程序目标在它所构建的每一行上都陈述起点：文件、`{ from }` 或 `false`。不写仍是错误。
2. `false` 的成立由行回答：程序在该行的形态是共享对象时成立；形态是可执行映像时，只在该行陈述
   `link-without-main` 时成立，链接加入那些词。其他行上拒绝，并说明该行由谁进入映像与补救的写法。
3. 行表的 `main = false` 替换目标在该行的起点。基表的文件仍是其他行的起点，不作为该行映像的源文件；目标表中
   描述起点的键（`cflags`、`cxxflags`、`defines`、`start`、`windows_entry`）在该行不适用。
4. 同一张表中 `false` 与描述起点的键并存、`main = true`、入口目标与 `[test]` 中的 `false` 都被拒绝。
5. `mcpp run` 拒绝没有程序起点的映像；结果记录的产物条目写 `entry.main = false`，没有 `launch`。

一个三行的应用：

```toml
[targets.app]
kind = "app"
main = "platform/linux/main.cpp"

[target.'cfg(env = "android")'.targets.app]
main = false

[target.'cfg(os = "emscripten")'.targets.app]
main = false
```

Linux 行从 `platform/linux/main.cpp` 开始；Android 行链接 `libapp.so`，桌面文件不进入它；Web 行以 `--no-entry` 链接
`app.js` 与 `app.wasm`，页面调用模块导出的函数。

## 3. 设计理由

1. **必填的依据只在一部分行上成立。** 要求写 `main` 是因为启动者会调用程序起点：缺了它，链接在稍后失败，提前报告更好。
   在启动者不调用程序起点的行上，这个依据不存在。因此规则改为"每一行都陈述起点，且可以陈述没有"，而不是取消必填：
   在需要起点的行上，遗漏仍然在清单处被报告。
2. **"没有"必须是一个值。** 行表只能以值覆盖键，不能删除从基表继承的键。多平台应用的基表一定写着桌面的 `main`，
   所以"这些行上可以不写"无法表达 Android 与 Web 行上的事实。
3. **一个概念一种拼写。** 仍是 `main` 这个键，值域从文件与 `{ from }` 扩展到 `false`。没有新键，没有同义的第二种写法。
4. **事实在数据中。** 哪些行允许、怎样链接由行回答：Android 沿用已有的形态函数，Web 的链接词写在行数据中，平台包
   定义的行以同一个键陈述。引擎不按系统名分支。
5. **做得少不能静默。** 不写仍是错误；需要起点的行上拒绝并说明启动者；`mcpp run` 不启动一个没有起点的模块。
6. **一个事实陈述一次。** `--no-entry` 由引擎从 `main = false` 渲染。要求作者同时在 `ldflags` 写 `--no-entry`，等于
   要求引擎解读 `ldflags` 来核对两处陈述，而 mcpp 不解释 `ldflags`。
7. **与入口目标互补。** 平台的入口是一个翻译单元中的代码时（NativeActivity 胶水、JNI 注册），它应是入口目标，由
   `main = { from }` 选择并直接链入；入口散在多个翻译单元或保留在库中时，`main = false` 如实陈述没有起点。

## 4. 与业界做法的比较

| 工具 | 做法 | 代价 |
|---|---|---|
| CMake | 没有入口的概念；工程在平台分支中写 `add_library(SHARED)` 或 `add_executable`，Web 上在链接选项中写 `--no-entry` | 形态与入口选择是每个工程的命令式分支；遗漏或错配在链接或运行时才出现 |
| Cargo | 二进制 crate 与 `cdylib` 是不同的目标种类；Android 与 Web 上使用 `cdylib`，入口由库提供（`android-activity`、`wasm-bindgen` 的 `start`）；`build = false`、`readme = false` 是"路径或 false"的取值惯用法 | 同一个应用在不同平台是不同的 crate 形态，靠条件编译与外部工具（`cargo-apk`、`wasm-pack`）组合 |
| Android NDK | `android_native_app_glue` 作为静态库提供 `ANativeActivity_onCreate`，链接需要 `-u ANativeActivity_onCreate` 才保留它 | 入口的保留依赖链接选项与归档成员抽取，遗漏时在运行时才失败 |
| Emscripten | `--no-entry` 是链接选项，含义正是"没有 `main`" | 选项与源码是否定义 `main` 分开陈述，二者不一致时链接失败 |
| WASI | command 与 reactor 两种模块（`-mexec-model=reactor`） | 同一区分，另一种拼写 |
| mcpp | `kind = "app"` 是"用户启动的那个东西"，形态由行决定；起点按行陈述为文件、依赖的入口或 `false`；链接词来自行的数据 | 每个没有起点的行写一行 `main = false` |

mcpp 的做法把形态与起点都写成按行的声明：工程不写平台分支，遗漏在规划时被报告并指出启动者，结果记录说明起点的
来源；框架不需要占位文件。`false` 的取值沿用 Cargo 的惯用法。将来出现 WASI 行时，它以 `link-without-main =
["-mexec-model=reactor"]` 陈述 reactor 形态，模型与清单不变。

## 5. 不采纳的方案

| 方案 | 不采纳的理由 |
|---|---|
| 在这些行上允许省略 `main`（隐式） | 无法覆盖基表继承的 `main`（§3 第 2 条）；Web 上遗漏 `main` 会静默地变成没有起点的模块；形成第二条规则 |
| 行表 `kind = "shared"` | 按行改变目标的身份；`app` 已经承载按行的形态；行表只在库的两种形态之间选择 |
| 新键（`entry = "none"`、`start = "platform"`） | 同一概念两种拼写；`start` 是 L0 的符号，层次不对 |
| `main = ""` 或 `main = "none"` | 空字符串像笔误；`"none"` 是合法的文件名 |
| 作者在 `ldflags` 写 `--no-entry`，引擎核对 | 一个事实两处陈述；引擎需解读 `ldflags`（§3 第 6 条） |
| 按源码推断（扫描是否定义 `main()`） | 遗漏变成静默的形态改变；推断的结论不能在清单中看到 |
| 推荐库导出入口并以链接选项保留 | 依赖链接顺序与归档成员抽取，无法给出来源诊断（设计 2026-10-26 §24.6）；仍然可行，但不是模型推荐的形式 |

## 6. 验证

- 单元：`Manifest/ParsesMainFalseForAnImageWithNoProgramStart`、`Manifest/RefusesMainFalseBesideWhatDescribesAStart`。
- e2e：`deps/entry 981`（本机行的拒绝说明 C 运行时与补救写法；不匹配本机的行上的 `false` 不改变本机程序；不写仍被拒绝）；
  `android/rows 981`（Android 行链接共享库，桌面文件不进入，导出名在库中；`bin` 被拒绝）；`web/emscripten 981b`
  （链接带 `--no-entry`，模块导出函数且没有 `main`，`mcpp run` 被拒绝，结果记录写 `entry.main = false` 且没有 `launch`）。
- 手工：同一工程在 Linux、`aarch64-linux-android` 与 `wasm32-emscripten` 上构建；Linux 程序输出 `desktop 1`，`libprobe.so`
  导出 `probe_value`，`probe.wasm` 的导出为 `probe_value` 等运行时名字，没有 `main`。

## 7. 写作中发现、仍可讨论之处

1. **宿主调用的名字没有被校验。** `start` 有 `--require-defined`，宿主调用的名字没有对应的检查：Android 的 `JNI_OnLoad`、
   `Java_*`、`ANativeActivity_onCreate` 与 Web 的导出函数由宿主侧代码决定，遗漏时在运行时才失败。可讨论的方向是由程序
   陈述宿主调用的名字（候选：沿用 `exports`，在 Web 行渲染为 `-sEXPORTED_FUNCTIONS`，在 Android 行作为导出与必须定义的
   名字），使遗漏成为链接错误。需要先确认 `exports` 在 Web 行上的含义与共享库上的含义一致。
2. **Web 的模块包装仍写在 `ldflags`。** `-sMODULARIZE`、`-sEXPORT_NAME` 决定 JavaScript 包装的形态，与起点无关，本次不纳入
   模型。若需要声明式的 Web 模块形态（工厂名），应作为 Web 行的产物键另行设计，而不并入 `main`。
3. **键名 `main` 的含义宽于字面。** 它命名"程序代码开始的翻译单元"：在 Android 上该文件定义平台的导出，在裸机上定义
   `start` 所指的符号，现在还可以是 `false`。已发布的键不改名（AGENTS.md §3），文档陈述它的含义。若要更贴切的拼写，
   只能以别名引入，与"一个概念一种拼写"相冲突；本记录建议保持。
4. **只面向 Android 的工程也写 `main = false`。** 显式规则的代价是一行；替代是在形态为共享对象的行上把不写读作"没有"。
   本记录按 §3 第 1、5 条选择显式。
