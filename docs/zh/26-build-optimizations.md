# 26 —— 构建优化

**读者:** 构建所编译的单元多于一次编辑看起来需要的人,以及想知道 mcpp 在运行编译器之外对构建做了
什么的人。

**本章回答的那一个问题:** mcpp 应用哪些优化,哪些默认开启,每一项改变什么,怎样改变它的取值。

**不在这里:** 怎样组织代码使编辑的影响保持局部,见 [13 —— 代码与工程风格](13-code-and-project-style.md);
profile 的各个键,见 [04 —— mcpp.toml](04-mcpp-toml.md);精确规则,见
[SPEC-019](../specs/build-optimizations.md) 与 [SPEC-018](../specs/module-rebuilds.md)。

不含本章所列任何一项的构建是**标准构建**:每个单元编译一次,编译器写出的每个文件原样保留,其后
的每一步都执行。mcpp 在三处对它有所增加——源文件、构建图、每一次编译——每一项增加都是一张表
中的一个条目。低风险且收益经过测量的条目默认开启,其余由参数开启。

## 条目

| 条目 | 默认 | 参数 | 作用 |
|---|---|---|---|
| `keep-bmi` | 开 | — | 模块接口编译为原有的 BMI:导入者不编译 |
| `keep-object` | 开 | — | 对象编译为原有字节:不归档,不链接 |
| `gcm-stamps` | 开 | — | GCC 在 BMI 中写入两个时钟,它们不算差别 |
| `gcm-locations` | 开 | `[profile.<name>] bmi_cutoff`,`MCPP_BMI_CUTOFF` | GCC:只有行映射移动的 BMI 被保留,导入者给出此前的行号 |
| `scan-pass` | 开 | — | 图的扫描先于编译,作为单独的一遍执行 |
| `split-schedule` | 关 | `[build] bmi_schedule`,`MCPP_BMI_SCHEDULE` | BMI 存在时导入者即开始,不等编译器退出 |
| `private-fragment` | 开 | `[build] private_fragment`,`MCPP_PRIVATE_FRAGMENT` | 私有模块片段构建为独立单元 |

`MCPP_BUILDOPT=off mcpp build` 对一次调用给出标准构建:全部条目关闭。没有参数的条目产出的字节与
标准构建相同,因此没有可选之处,只由这一条语句关闭。

## 一次编辑所编译的单元

以 clang 23.1.3 与 gcc 16.1.0 实测:模块 `c` 被 `b` 导入,`b` 被 `main` 导入;编辑的是 `c` 中一个
非 inline 的函数。

| `c` 的写法 | 编辑 | clang 编译 | gcc 编译 |
|---|---|---|---|
| 单文件,函数体在接口中 | 函数体 | `c`、`b`、`main` | `c` |
| 单文件,函数体在接口中 | 函数体之上的一行注释 | `c`、`b`、`main` | `c`、`b`、`main` |
| 单文件,函数体在 `module :private;` 之后 | 片段中的任何内容 | 片段 | 片段 |
| 接口文件与实现文件 | 实现中的任何内容 | 实现 | 实现 |

内容未变而时间被更新的文件只编译自身的单元,各编译器与宿主相同:BMI 与对象是原有的字节。

## 私有模块片段

```cpp
export module shapes;
export int area(int w, int h);
export inline int twice(int v) { return 2 * v; }

module :private;                    // 自此以下,不为导入者所达
int area(int w, int h) { return w * h; }
```

语言规定片段不可为导入者所达。mcpp 按此构建:`module :private;` 之前的文本是接口单元,片段是同一
模块的实现单元。该行以下的编辑只编译这个单元并链接;该行以上的编辑到达导入者。

- 诊断、调试信息与 `__FILE__` 给出原文件。
- GCC 未实现 `module :private;`。构建为独立单元之后,该文件在 GCC 上同样可以构建。
- 片段读取对接口的翻译单元局部的内容时——模块范围内定义的宏、未命名的命名空间、命名空间作用域
  的 `static`、命名空间作用域的 using 指令——按原样构建,`--verbose` 给出起决定作用的行。编译器
  不能按原样构建片段时,该说明是警告。
- `[build] private_fragment = "as-written"` 把文件原样交给编译器。

## 构建给出的说明

```
note  hub.cppm is the whole of module hub, which 46 units import: its function bodies are
      compiled with its interface, so an edit of one can compile all 46 again. After
      `module :private;` they are a unit of their own, and an edit of them compiles that
      unit alone
```

重编译的起点是一个只有一个文件的模块、且导入它的单元不少于 5 个时给出这条说明,每个构建目录一次。
此外,替换或保留了 BMI 的构建说明是哪些([04](04-mcpp-toml.md),`bmi_cutoff`)。

## home 的缓存放置的内容（@NEXT@+）

在各条目之外，`[build] cache` 为 home 的（缺省）时，home 的构建缓存（`build-cache/v1/`，[25](25-home-indices-and-store.md)）
为 home 中任何工程的下一次构建保存一次构建的产物：

- **单元。** 编译执行器启动编译器之前，以编译的命令、编译器与源文件的内容查找这次编译。条目写出该键此前一次编译
  读取的文件（其依赖文件，或扫描的依赖文件与它导入的 BMI）及其内容；这些文件仍是那些内容时，放置其对象与 BMI，
  不启动编译器。编译之后保存输出与读取的文件。BMI 以产生它的编译的键命名，因为 GCC 对同一源文件的每次编译写出
  不同的 BMI；包存储中的文件以路径命名，因为存储不变。mcpp 的新检出在已填充的缓存上 4.1 s 构建完成（原为 164 s），
  288 次编译全部被放置。cl.exe 的编译不进入缓存。
- **构建程序的输出。** 来源被命名的包——索引的版本、git 的提交、路径依赖的内容摘要——其构建程序在
  `build-cache/v1/out/<包>@<版本>/<键>/out`（`MCPP_OUT_DIR`）中运行，键为不含该目录的配置：第二个工程取用这些输出，
  不再运行（[SPEC-004 §9](../specs/manifest-semantics.md) 第 18 项）。

在 `[cache] unused_days` 之内没有构建使用过的条目与其他条目一样被移除。

## 构建的记录（@NEXT@+）

构建记录它运行的每条边：所属的包、用时，编译另有编译器的处理器时间与最大驻留集，以及是否由单元缓存放置；
以及缓存为每个包提供的单元数。`MCPP_BUILD_RECORD=<文件>` 把该次调用每次构建的记录追加到该文件，每条边一行
（[SPEC-017](../specs/test.md) B4，`builds.ndjson`）；`mcpp test` 在其存储中保存一次运行的构建。`--verbose` 下构建以
`Slowest` 行写出最慢的五条边及其处理器时间与内存。运行满一分钟的边以一行 `Running` 被点名一次，写出它编译的包与源文件。

## 改变一个条目的取值

```toml
[build]
private_fragment = "as-written"   # "unit"(默认)| "as-written"
bmi_schedule     = "on"           # "auto"(默认,即 off)| "on" | "off"

[profile.dev]
bmi_cutoff = "exact"              # "ignore-locations"(默认)| "exact"
```

每一项有对一次调用生效的环境变量(`MCPP_PRIVATE_FRAGMENT`、`MCPP_BMI_SCHEDULE`、
`MCPP_BMI_CUTOFF`),`MCPP_BUILDOPT=off` 覆盖全部。

## 已记录、未实现

| 优化 | 预期收益 | 需要 |
|---|---|---|
| 不写 `module :private;` 而拆分单文件模块 | 不写该行而得到片段的效果 | 对源码的语义分析,由语言服务器提供 |
| clang:可观察的接口未变时保留 BMI | 函数体与注释的编辑止于该模块 | 两版接口的语义差异 |
| 按符号计算受改动影响的测试 | `mcpp test --since` 运行更少的测试 | 跨导入的引用 |
| 一个程序中同一个库的两个版本 | 依赖链指定两个版本时可以构建 | 把模块改名分开;头文件与宏不能 |
