# `bench/` — 构建引擎基准套件

[English](README.md) · **简体中文**

一个跨平台测量工具：在**同一份 C++ 源码**上比较不同的**构建引擎**，并测量
C++20 具名模块相对于头文件到底付出/节省了什么。

用 C++23 写、由 mcpp 构建，所以 Linux / macOS / Windows 上跑法完全一致 —— 它
替换掉的那套 shell 脚本只能在 Linux 上跑。

```bash
# 生成的 fixture，跨引擎、跨源码形态
bench --engines mcpp,cmake,xmake,bazel \
      --variants headers,modules,modules-impl \
      --scenarios cold,noop,touch-hub,edit-body \
      --compiler payload:gcc --jobs 32 --out report.json

# 真实工程，原地测量 —— 比如两个 mcpp 二进制对比
bench --project bench/projects/xlings/xlings-2026.8.13.1 \
      --buildfiles bench/projects/xlings \
      --engines mcpp=./target/x86_64-linux-gnu/*/bin/mcpp,mcpp \
      --scenarios noop,touch-hub --hub src/platform.cppm --body src/platform.cpp
```

每个 `mcpp=<path>` 引擎都用**那个二进制自己报的版本**作标签
(`mcpp@2026.8.13.1`)，所以两个版本永远不会并成一行。「这个版本变快了吗」就是
这样回答的 —— 真的把两个都跑一遍，而不是在测量工具里模拟其中一个。

> 本文是英文版 [`README.md`](README.md) 的对照翻译。两份内容一致；如有出入，
> 以英文版为准（CI 与守卫测试读的是英文版里引用的文件名）。

### 三份文档，分别回答什么

| 文件 | 回答 |
|---|---|
| **本文 / README.md** | 一次计时**怎么取**，以及什么是刻意不控制的 |
| [`SPEC.md`](SPEC.md) | **测什么**：六个轴、为什么 cmake 是基线、一个格子「无定义」意味着什么 |
| [`matrix.json`](matrix.json) | CI **跑哪些格子** —— 唯一真源，由 `.github/workflows/bench.yml` 读取 |

格子清单只出现在其中**一处**。写两遍的矩阵就是会自相矛盾的矩阵，而且矛盾是无声
的：两份副本看起来都对。`tests/e2e/233_bench_matrix.sh` 就是用来保证这一点的。

---

## 0. 钉住了什么，以及为什么每一条都必须钉住

一个基准数字的价值，等于取它时被摁住不动的那张清单。下面每一行都曾经是松的，
而每一行松着的时候，产出的表格测的都不是它自己声称的东西。

| 项目 | 钉到 | 声明位置 |
|---|---|---|
| cmake | **4.0.2** | `matrix.json` → `tools` |
| xmake | **3.1.0** | `matrix.json` → `tools` |
| bazel | **9.2.0** | `matrix.json` → `tools` |
| gcc | **16.1.0** | `bench/src/toolchain.cppm` |
| clang / libc++ | **22.1.8**（Windows：20.1.7） | `bench/src/toolchain.cppm` |
| 参照 mcpp | **2026.8.11.3** | `matrix.json` → `reference_mcpp` |
| mcpp（被测工作负载） | **2026.8.11.3** — `a749e9f` | 子模块 `projects/mcpp/mcpp-2026.8.11.3` |
| xlings（合并风格） | **2026.8.11.2** — `b1563fe` | 子模块 `projects/xlings/xlings-2026.8.11.2` |
| xlings（分离风格） | **2026.8.13.1** — `f072075` | 子模块 `projects/xlings/xlings-2026.8.13.1` |
| 被测 mcpp | 当前 checkout | CI 现场构建，由 `newest_artifact.sh` 定位 |

**全部由 xlings 安装**，版本精确，每个 runner 一致。CI 里跑的字面就是
`xlings install cmake@4.0.2 xmake@3.1.0 bazel@9.2.0 mcpp@2026.8.11.3`，而且
job 会打印每个工具实际解析到的版本，与钉的版本不符就大声告警。

这解决了四件已经真实发生过的事：

* **cmake 3.31.6** 是 GitHub runner 镜像自带的版本。它没有 CMake 4.0 的
  `import std` 实验开关键，所以*每一个 module 格子都 configure 失败*。换成
  4.0.2 之后全过。
* **`command -v g++`** 在那些镜像上是 gcc 13.3.0。cmake 用它配不出 C++23
  modules，xmake 直接把它编崩（internal compiler error）—— 而 mcpp 一直悄悄用
  自己 registry 里的 gcc 16.1。表格是 `48 failed / 6 ok`，却仍然被当作「构建引擎
  对比」。现在**每个**引擎都拿到 mcpp 自己载荷里的那个驱动
  （`--compiler payload:gcc`）：套件的公平性规则终于被执行，而不只是写在注释里。
* **被测工作负载会漂移。** xlings 是运行时 `git clone --depth 1` 默认分支的，
  所以目标随上游每次 push 而变 —— `--hub src/xlings.cppm` 指的文件已经消失
  好几个月，每个 xlings 格子都报 `skipped`，每个 job 都报成功。mcpp 自己的
  源码是同一个缺陷、但更难看见的形式：`--project $GITHUB_WORKSPACE` 让
  checkout 成了工作负载，于是分支上每一次提交都在悄悄改变被测对象。
  **被测的引擎是那个二进制、它本来就该变；工作负载不该变。** 现在三个都是
  git 子模块，守卫会检查每个 `hub`/`body` 在钉住的树里确实存在。
* **只测了一个 mcpp。** 一份只说「这个分支有多快」、却不说「有没有变快」的报告，
  不是 pull request 上的基准该给的东西。

> **刻意不摁住的**：runner 硬件。见英文版 §4a。

> **这些数字没有覆盖的**：mcpp 的分离式调度（`[build] bmi_schedule = "on"`）在所有
> 平台验证通过前是 opt-in 的，所以两个 mcpp 二进制都是关着它跑的。它的效果单独
> 测量，见 `.agents/docs/2026-08-13-build-optimization-status.md`。

---

## 1. 公平性不变量

一次对比只有在这五条同时成立时才有意义，任何一条破了，测的就是别的东西：

1. **同一个编译器二进制** —— 不是「同一个 family」，是同一个文件。
2. **同样的语言开关** —— `-std=c++23`、release 下同样的优化级别。
3. **同一份源码集合** —— 用 glob 而不是手写清单，手写清单会悄悄漂移。
4. **同样的产物形态** —— 一个可执行文件，同样的静态/动态标准库选择。
5. **同一个标准库形态** —— `import std;`，不是头文件垫片。

`bench/projects/common/` 里放的就是「让这个引擎驱动出 mcpp 同样的进程树」这件事
的共享实现（cmake 与 xmake 各一份），**按编译器 family 分支**，因为编译器是一个
真实的轴而不是一个标签。

---

## 2. 两种模式

* **fixture 模式** —— 生成一棵合成树，参数化（`--preset` / `--units` /
  `--fanin` / `--weight`）。唯一能同时给出 `headers` / `modules` /
  `modules-impl` 三种形态的地方，所以也是 *variant* 轴真正成为受控变量的地方。
* **`--project` 模式** —— 原地测量一棵真实的树。**永远不写入被测仓库**：
  编辑类场景会先存下文件字节、无论函数怎么退出都还原（`SourceGuard`），子进程
  日志一律落在 `--work` 目录里。

真实工程测的是「钉住的快照」，不是你的工作区 —— 见 §「Measure a PINNED
SNAPSHOT」。

---

## 3. 场景

| 场景 | 扰动 | 问的问题 |
|---|---|---|
| `cold` | 没有构建目录 | 完整建图 + 全量编译 |
| `noop` | 什么都不动 | 「已经是最新」有多便宜 |
| `touch-hub` | 给被大量 import 的单元改 mtime，**内容不变** | 引擎能不能证明接口没变？ |
| `edit-comment` | 往同一个单元里插一条注释 | 字节**确实**变了但接口没变 —— 只有比较产出 BMI 的引擎能止住级联 |
| `edit-body` | 函数体内部一处真实语义修改 | 日常循环。接口单元里的内联函数体，BMI 合理地变了，级联是**对的** |
| `touch-leaf` | 给没人 import 的单元改 mtime | 重编 1 个 + 链接 |

`edit-comment` 与 `edit-body` 是**刻意分开**的：不分开的话，一个能跳过纯注释重建
的引擎就可以宣传成「改代码快 12 倍」，而那实际上是一句关于注释的话。
`edit-body` 是反方向的对照 —— 那里没有引擎应该快，快了就是漏了该做的活。

---

## 4. 一个格子可以是「无定义」的，并且必须说明原因

| 状态 | 含义 |
|---|---|
| `ok` | 测到了，有 `samples` |
| `failed` | 引擎跑了但没产出产物 —— 这是**发现**，不是缺口 |
| `unavailable` | 这台机器上没装 |
| `skipped` | 这个引擎表达不了这个格子 —— `note` 说明缺什么 |

**退出码**：只有「测到了东西」且「没有 `failed`」才返回 0。这不是时间阈值 ——
共享 runner 上设时间阈值只会把正常波动变成没人看的红叉。缺了这条的代价很具体：
一个「通过」的矩阵 job 实际是 6 ok / 48 failed / 18 unavailable，而每个 xlings
job 一个测量都没有，这个状态持续了好几周。

`failed` 确实属于已知缺口时，写进那个格子的 `allow_failed`，并且**必须**在
`note` 里带 `KNOWN GAP` —— 守卫会检查。一个不说明原因的豁免就是一个被藏起来的
失败。

---

## 4b. 三个引擎在 combined 树上的实测

**第一次三条臂都能产出可运行二进制的测量** —— 在此之前 cmake 与 xmake 两列一直是
`failed`,所以这里原本只有 mcpp 对 mcpp。

| 场景 | **mcpp** `bmi_schedule=on` | mcpp 默认 | cmake | xmake |
|---|---|---|---|---|
| `cold` | **37.56s** · 3.18x | 92.49s · 1.29x | 119.46s · 1.00x | 105.02s · 1.14x |
| `noop` | **0.72s** · 0.50x | 0.74s · 0.49x | 0.36s · 1.00x | 0.40s · 0.90x |
| `touch-hub` | **1.04s** · 93.93x | 1.79s · 54.96x | 98.16s · 1.00x | 98.16s · 1.00x |
| `edit-body` | **29.65s** · 3.32x | 88.38s · 1.11x | 98.43s · 1.00x | 98.00s · 1.00x |
| `edit-comment` | **30.44s** · 3.22x | 93.81s · 1.04x | 97.98s · 1.00x | 97.97s · 1.00x |

**两列 mcpp 都在这里,是因为只放一列会误导。** 默认列是用户今天拿到的东西;
`bmi_schedule=on` 是可选的分离调度,不放它会严重低估 mcpp —— `edit-body` 在默认
列是 1.11x,开了调度是 3.32x。

* **`edit-body` 与 `edit-comment` 不是「没有优势」。** 两者的级联都是**欠的**
  (被扰动的函数体在接口单元里,BMI 真的变了)。默认列显示 mcpp 以 cmake 的速度
  做完这份欠下的工作;调度列显示它把**同一份工作**做快了 3.3 倍 —— 靠的是 BMI
  一产出就发布,而不是等代码生成结束。
* **`noop` 是 mcpp 真正输的一项**,两列都输:0.72–0.74s 对 cmake 的 0.36s
  (0.49x)。这是每次调用的固定开销,也是用户在每一次「改一行、构建一次」里都
  感受得到的数字 —— 什么都不做时,mcpp 是三者中最慢的。
* **默认列的 `edit-comment` 是 1.04x,不是 mcpp 自身工作负载上的 200x。** 注释插
  进了 xlings 保留在接口单元里的内联函数体,BMI 真的变了,级联是**欠的**。格子的
  note 会记录当次是哪种形态(见 §3),不要把它读成优化失效。
* **`touch-hub` 才是级联抑制的真结果:54.96x。** 内容没变,mcpp 拿编译器刚产出的
  BMI 和上一份比,跳过 45 个导入者;cmake 与 xmake 按时间戳判断,把它们全部重建
  —— 两者相差 0.00s,这正是两个时间戳驱动的引擎该有的样子。
* **§8b 那个 `bmi_schedule` 正确性缺陷在这里没有复现**,十个格子全 `ok`。它只在
  生成的 fixture 上复现 —— 那条紧密的 `unit_0→unit_1` 链会撞进窗口;三棵真实的树
  (mcpp 自己和 xlings 两种风格)都不会。这也正是该键仍然默认关闭的原因:**只有
  一个工作负载能暴露的缺陷,仍然是缺陷。**

<sub>xlings `2026.8.11.2`,gcc 16.1.0 载荷,Linux x86_64 · i9-13900K · n=1 ·
`--baseline cmake`。原始报告:`bench/results/xlings-3way-20260814/`。
**这张表拼接了两轮测量,与其等人自己发现,不如写在这里。** `mcpp 默认`、`cmake`、
`xmake` 三列来自 `xlings-combined-3way.json`;`bmi_schedule=on` 一列来自
`xlings-schedule.json` —— 调度臂从未和另外两个引擎在同一轮里测过。后一个文件**自己
也有一条默认臂**,且与这里并不完全相同:`cold` 在那边是 91.61s,这里是 92.49s,
差约 1%,属于未调优桌面机上正常的轮间波动。调度带来的加速请按**同轮那一对**读
(91.61 → 37.56,**2.44x**),而不是跨列读(92.49 → 37.56,2.46x);表里的跨引擎
比值取自 `xlings-combined-3way.json` 内部。</sub>


## 4c. 同样三个引擎在 split 树上

同一个工程,实现从接口单元里搬了出去。这正是两个 pin 存在的意义 —— 而它对结果的
影响**比换构建工具更大**。

| 场景 | **mcpp** | cmake | xmake |
|---|---|---|---|
| `cold` | **27.59s** · 1.82x | 50.13s · 1.00x | 41.90s · 1.20x |
| `noop` | **0.79s** · 0.44x | 0.34s · 1.00x | 0.50s · 0.68x |
| `touch-hub` | **1.32s** · 20.08x | 26.60s · 1.00x | 31.68s · 0.84x |
| `edit-body` | **1.79s** · 0.75x | 1.35s · 1.00x | 1.49s · 0.91x |
| `edit-comment` | **24.17s** · 1.09x | 26.35s · 1.00x | 31.28s · 0.84x |

<sub>xlings `2026.8.13.1`,`modules-impl`,主机与载荷同上。原始报告:
`bench/results/xlings-3way-20260814/xlings-split-3way.json`。</sub>

* **在这个工作负载上,重构的收益盖过了任何引擎选择。** mcpp 的 `cold` 从 92.49s
  降到 27.59s(3.35x),而 cmake 自己的 cold 也从 119.46s 降到 50.13s(2.38x)。
  把实现搬出接口单元,比换构建工具更值。
* **`edit-body` 是 0.75x —— 这里 mcpp 比 cmake 慢**(1.79s 对 1.35s)。函数体在
  `.cpp` 里,只重编一个 object、没有级联,于是这个场景量的是**每次调用的固定
  开销**而不是图推理 —— 和 `noop` 反映的是同一笔成本。在这条轴上 mcpp 没有优势
  可言,数字也就这么写。
* **`touch-hub` 仍然值:20.08x。** 比 combined 树的 54.96x 小,因为下游本来就
  没剩多少可跳的了。

## 5. 可观测性：跑的时候看得见

harness 会把进度实时打到 **stderr**（逐行 flush），stdout 留给报告：

```
[   12.3s] cmake/gcc/release/cold/xlings-2026.8.13.1/modules-impl  configure
[   15.1s] cmake/gcc/release/cold/xlings-2026.8.13.1/modules-impl  seed build
[  107.0s] cmake/gcc/release/cold/xlings-2026.8.13.1/modules-impl  seed build exited 1
             | ld: undefined reference to `mbedtls_ssl_free'
             | collect2: error: ld returned 1 exit status
```

两个刻意的设计：

* **失败时直接打出子进程日志的尾部。** 只写 `see .../logs/cmake-cold.log` 在 CI
  上等于什么都没说 —— 那个文件跟着 runner 一起销毁了。
* **每条 configure/build 都有超时**（`--timeout`，默认 1800 秒），超了就 kill 并
  报 `TIMED OUT after Ns and was killed`。曾经有两个 job 各自卡在一个子进程里
  25 分钟，日志一个字都没有。

---

## 5b. 已声明的不对称

这些去不掉,所以写出来而不是藏起来。完整清单见英文版 §5;下面是**读数字之前
必须知道**的几条。

* **三条臂拿依赖的方式不同,所以它们的 `cold` 不是同一个量。** xlings 链接
  ftxui / libarchive / lua / mbedtls,而 mcpp 的 registry 是以**源码**分发它们的。
  cmake 与 bazel 两条臂自己编这些源码 —— 而且是按各包自己的 `.xpkg.lua` 编,
  文件清单与 mcpp 完全一致 —— 于是它们的 `cold` 里含着约 470 个依赖翻译单元。
  xmake 臂改为经 xrepo 声明(和 xlings 自己的 `xmake.lua` 一样),链接的是
  xrepo 早先构建好的库,`cold` 里**不含**它们。`xmake clean` 不会清 xrepo 的包
  缓存,所以这不是首次运行的假象,而是稳定存在的**工作量差异**,不是引擎快慢。
  跨引擎比 `cold`,请用**生成的 fixture**(它完全没有第三方依赖);在 xlings 上
  请比增量场景,那里没有任何一条臂会重建依赖。
* **`+schedule=on` 那条臂是同一个二进制,不是另一个引擎。** 它是被测工程 manifest
  里的一个键,由 harness 经 `MCPP_BMI_SCHEDULE` 打开,标签写作
  `mcpp@<ver>+schedule=on`,和默认配置放在同一组行里一起读。
* **没有任何 fixture 写 `import std;`。** 各引擎在「能否、以及如何」构建 std 模块
  上差异极大(CMake 需要一把随版本变化的实验性 UUID,meson 根本没有说法),这个
  差异会淹没一切测量。fixture 一律经全局模块片段取标准库。**本套件测的是模块
  机制,不是 std 模块支持。**
* **bazel 的 cold 不是一台冷机器。** 它在工作区之外保有常驻 server 和 action
  缓存;这里的 `clean` **刻意不是** `--expunge`,否则连工具链一起丢掉,测的就
  变成了 provisioning。

## 6. 运行

```bash
# 构建 harness
cd bench && mcpp build --release

# 拉取被测的钉住工程（子模块）
git submodule update --init

# 一次 smoke
./target/*/*/bin/bench --engines mcpp,cmake --variants modules \
    --scenarios cold,noop --preset smoke --compiler payload:gcc
```

CI 跑的格子清单见 [`matrix.json`](matrix.json)；每次 run 的报告作为 artifact
上传，命名 `bench-<os>-<toolchain>-<project>`。

引用任何数字之前，请先读英文版的 §4a（什么时候一个格子**不能**被拿来比较）与
§5（已声明的不对称）。
