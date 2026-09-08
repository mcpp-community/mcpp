# 08 —— 测试

**读者:**任何有代码需要持续可用的人。

**本章回答的那一个问题:**测试怎么写、怎么跑,mcpp 认为什么是一个测试,以及
在本机跑不了的东西怎么测。

**不在这里:**runner 怎么抵达一台设备 —— 那是
[41 —— 抵达一台设备](41-devices.md);以及机器可读流的 schema,那是
[50 —— 机器可读输出](50-machine-output.md)。本章只说明哪个旗标产生它,到此为止。

在此之前:[05 —— 依赖与解析](05-dependencies.md) 覆盖 `[dev-dependencies]`,
那是测试如何取到产物取不到的包。在此之后:
[09 —— 按场景选命令](09-commands-by-scenario.md) 是其余一切的查阅入口。

## 测试的定义

每一个 `tests/**/*.cpp` 都是一个测试:mcpp 把每个文件编译成它自己的程序并运行它。
测试通过的判据是它的程序以 0 退出。

```
myproject/
  mcpp.toml
  src/…
  tests/
    test_parse.cpp        一个程序
    unit/test_span.cpp    另一个
```

没有框架,也不需要注册。测试**可以**用一个框架 —— `[dev-dependencies]` 是它取到
框架的方式 —— 但 mcpp 持有的契约是退出码,这也是为什么为别的框架写的测试不需要
适配层。

`mcpp new` 会生成 `tests/test_smoke.cpp`,让工程一开始就有这个目录。

## 运行它们

```bash
mcpp test                 # 构建并运行每个测试
mcpp test parse           # 只运行名字匹配的那些
mcpp test --list          # 列出会跑哪些,不构建也不运行
mcpp test -- --verbose    # `--` 之后的一切传给每个测试程序
```

测试的构建轴与 `mcpp build` 相同,因此测试跑在它要检查的那个配置上,而不是默认
配置上:

| 旗标 | 选中的集合 |
|---|---|
| `--profile <name>` | `dev`(默认)、`release`、`dist`,或 manifest 声明的某个 `[profile.*]` |
| `--features <list>` | 这次测试构建的 feature 集合 |
| `--target <triple>` | 宿主以外的目标 |
| `--accel <spec>` / `--no-accel` | 本次构建面向的设备后端 |
| `--cap <list>` | 钉住某个能力的 provider |

`--timeout <secs>` 杀掉仍在运行的测试(默认 300;`0` 关闭),`--build-timeout <secs>`
限制编译。一个挂住的测试被报为**以它自己的名字失败**,而不是一个停下来的任务。

## 取到产物取不到的包的测试

```toml
[dev-dependencies]
counters = { path = "../counters" }
```

`[dev-dependencies]` 的条目只为测试构建解析,别处一概不用:它不在产物里,包的
消费者也永远看不见它。这就是「测试的依赖」与「包自己的依赖」之间的区别,也是
测试框架不会变成一个库所发布内容的一部分的原因。

[`examples/11-features`](../../examples/11-features/) 声明了一个并使用它。

## 在本机跑不了的目标上测试

面向交叉目标或裸机板子的测试,会被编译到那个目标,并经由一个 **runner** 执行 ——
runner 是板级支持包提供的一串 argv,mcpp 把测试二进制附加在其后执行它。

```bash
mcpp test --target thumbv7em-none-eabihf     # 为板子构建,经它的 runner 运行
mcpp test --no-runner                        # 忽略 runner,直接执行
```

测试本身一个字都不用改。同样的 `tests/**/*.cpp` 为设备编译,判据仍然是退出码 ——
这正是裸机 runner 被选成「能产生 semihosting 退出码或 QEMU 退出码」的原因。

`--no-runner` 是给「本机就能原生执行这些二进制、不该为模拟器付代价」的宿主准备的。

runner 本身、具名 runner,以及一块板子声明什么,见
[41 —— 抵达一台设备](41-devices.md)。

## 不能彼此并排运行的测试

`mcpp test` 在一个工作池上运行测试程序。一块板子接一个探针、一块 GPU、一个串口,
或者一份单座许可证,同一时刻只容一个使用者;两个 worker 同时去拿的结果是交错,
而不是干净地失败。

拥有该资源的那个包**自己声明**这一点,`mcpp test` 随后把这些测试串行化。工程方
永远不需要记得加 `-j1`。

## 报告给程序

```bash
mcpp test --message-format json
```

每个测试一条 NDJSON 记录,供 CI 任务或编辑器消费。schema 及其版本见
[50 —— 机器可读输出](50-machine-output.md);属于本章的只有「这个旗标存在」以及
「人类可读格式是默认」。

## 当前边界

- 一个测试是一个 `.cpp` 产出一个程序。mcpp 不发现文件内部的用例,因此框架的
  逐用例选择发生在程序内部,经由 `--` 之后的参数。
- `--build-timeout` 只在 POSIX 上有效。
- `--workspace-timeout` 限制 `--workspace` 的扇出并报告跑到了哪些;它不把超时
  归因到某个成员。
