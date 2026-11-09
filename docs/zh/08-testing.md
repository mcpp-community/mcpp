# 08 —— 测试

**读者：** 任何拥有「必须持续可用」的代码的人。

**本章回答的那一个问题：** 测试怎么写、怎么跑，mcpp 把什么算作一个测试，以及
在本机跑不了的东西如何测试。

**不在这里：** runner 如何抵达一台设备 —— 那是
[41 —— 抵达一台设备](41-devices.md)；机器可读流的 schema 是
[50 —— 机器可读输出](50-machine-output.md)。本章只说明哪个旗标产生它，到此为止。

在此之前：[05 —— 依赖与解析](05-dependencies.md) 覆盖 `[dev-dependencies]`，
那是测试用来取到产物取不到的包的手段。在此之后：
[09 —— 按场景选命令](09-commands-by-scenario.md) 是其余一切的查阅入口。

## 测试的定义

每一个 `tests/**/*.cpp` 都是一个测试：mcpp 把每个文件编译成它自己的程序并运行。
测试通过的判据是该程序以 0 退出。

```
myproject/
  mcpp.toml
  src/…
  tests/
    test_parse.cpp        one program
    unit/test_span.cpp    another
```

没有框架，也不需要注册。测试可以使用一个框架 —— `[dev-dependencies]` 是取到框架的
途径 —— 但 mcpp 持有的契约只是退出码，这也是为什么为其他框架写的测试不需要适配层。

`mcpp new` 生成 `tests/test_smoke.cpp`（一个不定义 `main` 的 [mtest](https://github.com/mcpp-community/mtest)
用例文件）与 dev-dependency `mcpplibs.mtest`（2026.11.8.1+）；`mcpp new <name> --template lib`
以同样方式生成一个库及其测试（[01](01-getting-started.md)）。

### 测试的位置

`tests/**/*.cpp` 是一个键的默认值：

```toml
[test]
discover = ["checks/**/*.cpp", "!checks/fixtures/**"]
```

`discover` 接受与 `[build] sources` 同一套词汇的 glob：某个 glob 匹配到的每个文件
都是一个测试程序，以 `!` 开头的 glob 把它匹配到的文件从集合中剔除，无论这些文件是
被哪个 glob 找到的。一个测试的名字，是它相对于第一个匹配它的 glob 所在固定目录的
路径，去掉扩展名 —— 因此默认配置下 `tests/unit/test_span.cpp` 的名字是
`unit/test_span`。`discover = []` 不发现任何测试。两个名字相同的文件会被拒绝，并
点出这两者。

`[test] programs = "directory"`（2026.11.8.1+）使 glob 固定目录之下同一目录的文件成为
一个程序，以目录命名（`tests/unit/*.cpp` 为程序 `unit`）：每个文件是它的一个翻译单元，
它的用例是这些文件的全部用例；有文件定义 `main` 时以该文件为起点，否则按不定义 `main`
的文件的规则开始。同一目录中两个定义 `main` 的文件会被拒绝并点出两者；直接位于固定
目录中的文件仍各自是一个程序。多文件的程序只链接一次，而每文件一个程序时每个文件链接
一次，后者正是由许多小文件组成的套件在构建上花费的时间。

使用测试协议的程序**按其用例所在文件各起一个进程**运行（2026.11.9.1+）：mcpp 先列出用例，
每个文件的那次运行只选取自己的用例（P4），像一个程序一样在各工作线程间调度，名为
`<程序>/<文件>`——兼得每文件一个程序的隔离与并行，而只链接一次。`[test] split = "case"`
使每个用例一个进程，`"none"` 只运行一次。

测试程序只保留它用到的部分（2026.11.9.1+）：各单元以每函数、每数据对象一个节编译，
测试以 `--gc-sections` 链接（Mach-O 为 `-dead_strip`），测某一模块的测试不再带上整个包
的全部函数（mcpp 自身的单元测试：每个由 32 MB 降至 14 MB）。静态构造是回收的根。
`[test] link = "objects"` 保留每个目标文件的全部内容。

由自身多个源文件编译而成的测试套件也可以是一个包：一个工作区成员，它的
`[build] sources` 承载整套套件，其唯一的测试程序驱动套件，用
`mcpp test -p <member>` 选中。

### 测试程序的起点

提供 `main` 的测试框架把它声明为入口目标（`kind = "entry"`，docs/04 §2.2），包为
全部测试指名一次：

```toml
[dev-dependencies]
fw = "0.1"

[test]
main = { from = "fw/main" }
```

每个测试文件随之成为其程序的普通翻译单元，不定义 `main`。本包自身的名字选择本包的入口，
框架借此以测试文件定义的回调测试它自己的入口。在独立目标上，`[test] start` 陈述每个测试
镜像的入口符号。

入口可以以 `test-main = true` 提供自己（2026.11.8.1+）：未陈述 `[test] main` 的包中，不定义 `main`
的测试文件以它为起点，定义了 `main` 的文件保留自己的。mtest 的入口与其 GoogleTest 适配的入口都提供
自己，测试只需写 dev-dependency。选择以文件为单位：包提供模块的入口，在文件导入其中某个模块时属于该测试，
一个也未导入时被搁置；因此同一个包中，mtest 用例的文件以 mtest 的入口为起点，GoogleTest 用例的文件以
GoogleTest 的为起点。依赖中定义 `main` 的源文件（`gtest_main.cc`）照旧作为测试的起点。同一测试仍有两个
起点时，构建被拒绝并指出它们，由 `[test] main` 选择。

## 运行测试

```bash
mcpp test                 # build and run every test
mcpp test parse           # only those whose name matches
mcpp test --list          # what would run, without building or running it
mcpp test -- --verbose    # everything after `--` goes to each test binary
```

测试的构建轴与 `mcpp build` 相同，因此测试跑在它要检查的那个配置上，而不是默认
配置上：

| 旗标 | 选中的集合 |
|---|---|
| `--profile <name>` | `dev`（默认）、`release`、`dist`，或 manifest 声明的某个 `[profile.*]` |
| `--features <list>` | 本次测试构建的 feature 集合 |
| `--target <triple>` | 宿主以外的目标 |
| `--accel <spec>` / `--no-accel` | 本次构建面向的设备后端 |
| `--cap <list>` | 钉住某个能力的 provider |
| `--toolchain <spec>` | 本次调用使用的工具链，例如 `llvm@23.1.3` |

`--timeout <secs>` 杀掉仍在运行的测试（默认 300；`0` 关闭），`--build-timeout <secs>`
限制编译耗时。挂起的测试以它自己的名字被报为失败，而不是报成一个停止的任务。

报告是命令的结果，写到标准输出：每个测试的结论、失败测试的输出、`test result` 行，
以及 `--workspace` 时的 `workspace result` 行。构建的各步骤（`Compiling`、`Running`）
属于叙述，写到标准错误，因此 `mcpp test > report.txt` 收集报告，
`mcpp test 2>&1 | tee test.log` 两者都收集
（见 [09 —— 按场景选命令](09-commands-by-scenario.md#输出流)）。

### 变更影响的测试

`mcpp test --since <commit>` 与 `--changed <path>`（2026.11.8.1+）只构建并运行一次变更
影响的测试，在构建之前由计划读出：

- 测试受其构建所用文件的变更影响：它自己的文件、它导入的模块单元及这些单元再导入的
  单元，以及链接中不属于任何模块的单元；
- 清单、锁文件、构建程序、头文件或计划中没有的源文件的变更影响所有测试；
- 其他文件只影响自己的文件中写出该文件的测试（测试数据），不影响其他测试。

`--since` 从 git 读取变更：自该提交以来提交的、工作区中修改的、新增且未被忽略的路径。
`Affected N of M tests` 给出选择结果及每个变更路径的判定；`--message-format json` 下
`affected` 记录列出每个测试及其原因。这一选择只缩小模式或 `-p` 指定的范围，不增加测试。

**测试按名字找到包的程序**（2026.10.27.1+）。`mcpp test` 先按 `mcpp build` 的方式构建包的程序，并给每个测试
`MCPP_BIN_<NAME>`：程序 `<name>` 的路径，名字转为大写，字母与数字之外的字符写作 `_`（`my-tool` 为
`MCPP_BIN_MY_TOOL`）。运行被测程序的测试从这里读取它。

**平台包在它的目标上测试**（2026.10.27.1+）。在带 `[platform]` 段的包中执行 `mcpp test` 而不给 `--target` 时，
以该包本身为平台，为平台的每个 `targets` 构建测试。其 runner 能在本机运行的目标被运行；其余已构建、报告为未运行
（[35](35-platforms.md)）。

### 在工作空间中

```bash
mcpp test -p core -p http              # the tests of two members
mcpp test --workspace --exclude legacy # every member's but legacy's
```

`-p` 可以重复，`--exclude` 把成员从 `--workspace` 或虚拟根的全体选择中去掉；两者都按
[07 —— 工作空间](07-workspace.md#53--p---package-选项)所述解析。对多个成员的测试把它们放在
一起规划、只构建一次：同一配置的成员是一张构建图，因此它们共用的包只编译一次，它的构建
程序只运行一次。然后每个成员的测试按 `[workspace] members` 的顺序运行，各自使用该成员
自己的运行时目录。命令在某个成员失败后继续，无论失败的是测试、它的包的构建还是它的规划，
逐成员汇报，只要有一个失败就以非零退出。发现按成员隔离，因此两个成员可以各有一个
`tests/main.cpp`。

## 取到产物取不到的包的测试

```toml
[dev-dependencies]
counters = { path = "../counters" }
```

`[dev-dependencies]` 的条目只为测试构建解析，别处一律不用：它不在产物里，包的消费者
也永远看不到它。这就是「测试的依赖」与「包自己的依赖」之间的区别，也是测试框架不会
成为一个库所发布内容之一部分的原因。

dev 依赖只到达测试单元，不到达包的其他部分（2026.11.8.1+）。它的包含目录与接口定义只给
`tests/` 下的文件；包自己的源文件在 `mcpp test` 下以 `mcpp build` 给出的命令编译，因此被
测试的代码就是发布的代码，`mcpp build` 与 `mcpp test` 共用一个构建目录和一份编译数据库。
只有 dev 依赖到达的内核接口（`mcpplibs.mtest` 经 `mcpplibs.os` 到达 openkal 的接口）只给
dev 依赖与测试单元：它们看到 `__OPENKAL__` 与 openkal 的头文件，包自己的源文件看不到。
dev 依赖提供的 C 库或 C++ 运行时是测试所链接的每个目标文件的属性，仍属于整个配置。

[`examples/11-features`](../../examples/11-features/) 声明了一个这样的依赖并使用它。

## 在本机跑不了的目标上测试

面向交叉目标或裸机板子的测试被编译到那个目标，并经由一个 **runner** 执行 ——
runner 是板级支持包提供的一串 argv，mcpp 把测试二进制附加在其后交给它执行。

```bash
mcpp test --target thumbv7em-none-eabihf     # built for the board, run through its runner
mcpp test --no-runner                        # ignore the runner and execute directly
mcpp test --target aarch64-macos --no-run    # build the tests for the target and stop
```

测试本身没有任何改变。同样的 `tests/**/*.cpp` 为设备编译，判据仍然是退出码 ——
这正是裸机 runner 被选来产生 semihosting 退出码或 QEMU 退出码的原因。

`--no-runner` 是为「本机能原生执行这些二进制、不该为模拟器付代价」的宿主准备的。

`--runner <name>`（2026.11.7.1+）选择具名运行器代替默认运行器，声明与参数替换
规则与 `mcpp run --runner` 相同。该选项同样适用于 `--print-launch` 与从归档读取的
测试。不存在的运行器名称会被拒绝；`--runner` 与 `--no-runner` 不能同时使用。

对于声明了 `debuggee` 的平台，以下命令构建一个测试，打印调试器要启动的命令，
但不启动测试：

```bash
mcpp test unit/math --runner debuggee --print-launch
```

`--no-run` 做出的是更窄的断言，且必须显式要求。不给它时，一个本机既无法执行、也没有
runner 可达的目标，会让每个测试都停在「未运行」，命令退出 2：mcpp 没有查明这些测试
是否通过，把这种情况报成成功，是本仓记录得最多的一种假读数。但 2 同样是 runner
坏掉时的退出码，于是一个只想要「构建」结果的调用方无法区分这两种情况。在 `--no-run`
下，每个被选中的测试都为目标编译并链接，没有一个被执行，结果也如实写出：

```
test result ok. 0 passed; 0 failed; 2 built, not run
```

编译不过的测试仍然是失败。`--no-run` 与 `--no-runner` 同时给出会被拒绝，而不是在
两者间择一执行：一个说的是「不经声明的 runner 直接执行二进制」，另一个说的是
「不要执行」。

测试程序把它读取的文件带在身边：runner 收到 `MCPP_RUNTIME_FILES`，即它已部署的文件
与它加载的共享库的清单，把程序移动到设备上的 runner 会把这些文件一并复制过去。测试
按相对于自身所在目录的路径定位这类文件。在 Android 的各行上，除非 `cxx_runtime`
另有声明，测试程序静态链接 C++ 运行时，因此设备上不需要 `libc++_shared.so`。

runner 本身、具名 runner，以及一块板子声明了什么，见
[41 —— 抵达一台设备](41-devices.md)。

### 构建一次，各处运行（mcpp 2026.10.30.1+）

ok 映像在每个系统上是同样的字节；在 Linux 上为 `x86_64-windows-gnu` 构建的测试
在 Windows 上运行。`--archive` 把两半分开：

```bash
mcpp test --target openkal --no-run --archive tests.tar.gz   # 构建各腿的测试并写出
mcpp test --archive tests.tar.gz                             # 在检出中运行本机能到达的部分
```

归档含各腿的测试程序与 `mcpp-tests.json`（每个测试的腿、参数与工作目录）。读取
时不构建；测试在检出中经本机的运行器或系统运行，报告与 `mcpp test` 相同。有测试
失败时退出码为 1，一个测试都没运行的归档也是 1。本机到达不了的腿报告为未运行
（[SPEC-015 §4](../specs/target-sets.md)）。归档是 tar，压缩方式由文件名决定：`.tar.gz`（或
`.tgz`）、`.tar.zst`、`.tar.xz` 或 `.tar`；其他名称在构建前即被拒绝。

## 不能彼此并排运行的测试

`mcpp test` 在一个工作池上运行测试程序。一块接了一个探针的板子、一块 GPU、一个串口，
或者一份单座许可证，同一时刻只容一个使用者；两个 worker 同时去争抢它的结果是交错，
而不是干净地失败。

拥有该资源的包对此做出声明，`mcpp test` 随后把这些测试串行化。工程方永远不需要
记得加上 `-j1`。

## 用例：测试协议

一个测试程序可以由若干用例组成。mcpp 不为用例提供框架，而是实现一份由框架实现的协议的运行器一侧：
[mtest 测试协议](https://github.com/mcpp-community/mtest/blob/main/spec/protocol.md) 第 1 版
（mcpp 2026.11.8.1+）。程序的入口陈述它时，程序实现该协议：

```toml
[targets.main]
kind          = "entry"
main          = "src/main.cpp"
test-protocol = 1
```

程序文件中带有标记 `mtest-protocol=1` 时同样如此，以 mtest 的库构建的程序即带有该标记。
带有标记却没有写出 `program` 事件的程序没有使用协议：与框架的库链接在一起的普通 `main`
文件是一个用例，按退出状态判定，运行与列出时都是如此。
mcpp 随之把协议的变量（`MTEST_PROTOCOL`、`MTEST_MODE`、`MTEST_REPORT`、`MTEST_TMPDIR`，
以及适用时的 `MTEST_SELECT`、`MTEST_SEED`）交给程序，读取程序写出的报告，并给出每个用例的结果：

```bash
mcpp test                       # 报告与记录中给出每个用例的结果
mcpp test --case 'fs/*'         # 每个这类程序中与模式匹配的用例
mcpp test --cases               # 列出每个程序的用例，不运行
mcpp test --seed 42             # 用例中随机性的种子
```

- 程序的退出码仍决定它的结论。报告中有失败用例时，程序即使以零退出也判为失败；开始而未结束的用例判为失败，
  原因是程序的退出码。
- 失败程序的报告列出每个失败的用例及其第一处失败的位置：`case fs/read failed at tests/fs.cpp:12: ...`。
- `--case` 的模式以 `*` 与 `?` 匹配；不含二者的名字匹配它自身及其下的用例（`fs` 匹配 `fs/read`）。
  已列举的用例所声明的用例在运行时报告，可以不被列举（协议 P3.4；mtest 的 `UTest fs{"fs", [] { "read"_test = ...; }}`
  只列举 `fs`）：已列举用例之下的名字选中该用例，之下的模式则完整运行该已列举用例。
  使用 `--case` 时，没有匹配用例的程序与所有未实现协议的程序不运行。
- 未实现协议的程序是一个用例，以退出码判定，并且收不到任何协议变量，即使 mcpp 自身的环境中有它们。

`--message-format json` 的记录带有协议、用例及其失败（[50](50-machine-output.md)）；`--print-launch`
为启动程序的调试器给出协议。

## 编写测试

```bash
mcpp test --guide            # 框架、它们的指南，以及用例相关的命令
```

框架在其入口上陈述编写测试的指南，`test-guide = "<文件>"`（2026.11.8.1+；协议的 P2.5）：包中的一个
文件，从已安装的包读取，与其版本一致。`mcpp test --guide` 列出本包的每个测试框架及其指南与仓库，以及列出、
选择、解释用例的命令；没有框架时，给出上文的契约与接入 mtest 的写法。失败的运行以
`how these tests are written: mcpp test --guide (...)` 结尾，`--message-format json` 的记录带有 `guide`。
将要编写测试、或要读懂测试为何失败的人或 agent，从这里开始。

## 报告给程序

```bash
mcpp test --message-format json
```

每个测试一条 NDJSON 记录，供 CI 任务或编辑器使用。schema 及其版本见
[50 —— 机器可读输出](50-machine-output.md)；本章只交代这个旗标存在，以及人类可读
格式是默认值。

```bash
mcpp test unit/math --print-launch -- --gtest_filter=Math.Adds
```

构建该模式指定的一个测试，并打印 `mcpp test` 启动它的方式（`mcpp.launch` 信封，mcpp 2026.10.31.2+），
供调试器启动：其中包括运行器、运行所加的环境与 `--` 之后的参数。

## 当前边界

- 一个测试是一个 `.cpp` 产出一个程序。其中的用例属于程序：mcpp 只对实现测试协议的程序列出并选择用例；
  对其他程序，框架的选择发生在程序内部，经由 `--` 之后的参数传递。
- manifest 加载不了时，`mcpp test --list` 列出的是 `tests/**/*.cpp`，而不是它读不到的
  `[test] discover` 集合。
- `--build-timeout` 只在 POSIX 上有效。
- `--workspace-timeout` 限制 `--workspace` 扇出的运行，并报告哪些成员跑完了；它不把
  超时归因到某一个成员。它在每个成员的测试开始前检查，因此不能打断成员共用的那次构建，
  那次构建由 `--build-timeout` 限制。
</content>
