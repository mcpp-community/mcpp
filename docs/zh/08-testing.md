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

`mcpp new` 生成 `tests/test_smoke.cpp`，使工程从一开始就带有这个目录。

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

由多个源文件编译而成的测试套件本身是一个独立的包：一个工作区成员，它的
`[build] sources` 承载整套套件，其唯一的测试程序驱动套件，用
`mcpp test -p <member>` 选中。

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
| `--toolchain <spec>` | 本次调用使用的工具链，例如 `llvm@22.1.8` |

`--timeout <secs>` 杀掉仍在运行的测试（默认 300；`0` 关闭），`--build-timeout <secs>`
限制编译耗时。挂起的测试以它自己的名字被报为失败，而不是报成一个停止的任务。

## 取到产物取不到的包的测试

```toml
[dev-dependencies]
counters = { path = "../counters" }
```

`[dev-dependencies]` 的条目只为测试构建解析，别处一律不用：它不在产物里，包的消费者
也永远看不到它。这就是「测试的依赖」与「包自己的依赖」之间的区别，也是测试框架不会
成为一个库所发布内容之一部分的原因。

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

## 不能彼此并排运行的测试

`mcpp test` 在一个工作池上运行测试程序。一块接了一个探针的板子、一块 GPU、一个串口，
或者一份单座许可证，同一时刻只容一个使用者；两个 worker 同时去争抢它的结果是交错，
而不是干净地失败。

拥有该资源的包对此做出声明，`mcpp test` 随后把这些测试串行化。工程方永远不需要
记得加上 `-j1`。

## 报告给程序

```bash
mcpp test --message-format json
```

每个测试一条 NDJSON 记录，供 CI 任务或编辑器使用。schema 及其版本见
[50 —— 机器可读输出](50-machine-output.md)；本章只交代这个旗标存在，以及人类可读
格式是默认值。

## 当前边界

- 一个测试是一个 `.cpp` 产出一个程序。mcpp 不发现文件内部的用例，因此框架的逐用例
  选择发生在程序内部，经由 `--` 之后的参数传递。
- manifest 加载不了时，`mcpp test --list` 列出的是 `tests/**/*.cpp`，而不是它读不到的
  `[test] discover` 集合。
- `--build-timeout` 只在 POSIX 上有效。
- `--workspace-timeout` 限制 `--workspace` 扇出的耗时，并报告哪些成员跑完了；它不把
  超时归因到某一个成员。
</content>
