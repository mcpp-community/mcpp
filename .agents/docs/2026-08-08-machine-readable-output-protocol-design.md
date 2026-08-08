# 机器可读输出协议 —— 对 RFC #379 的核对与修正

> 状态:设计待 review。核对基准 mcpp 2026.8.8.3(本机构建),xlings 2026.8.8.1。
> 本文不复述 #379,只写**核对结果**与**它需要改的地方**。

## 0. 结论先行

RFC #379 的分析基本成立,逐条实测核对见 §1。但它有一个**结构性遗漏**,会让阶段 0
到阶段 2 全部落空:

> **未知选项的报错走 stdout,退出码 1,stderr 为空 —— 而且这段代码在依赖包
> `mcpplibs.cmdline` 里,不在 mcpp。**

RFC 花了整个阶段 0 去规定「未知**格式值**怎么办」,但客户端在真实使用中先撞到的是
「未知**选项**」——因为它要探测的正是「这个版本认不认 `--format`」。而那条路径今天
把人类文本打进了协议要独占的通道。

由此推出的第二件事更关键:**RFC 提议的协商入口 `mcpp --protocol-version`,解决不了
它被设计来解决的那个问题**(§2.2)。

---

## 1. 逐条核对

方法:全部在本机对已构建的 mcpp 实跑,不看代码推断。一处我用 grep 得出的结论是错的
(以为 `cache list --json` 不存在),实跑后推翻 —— flag 注册不带横线,grep `"--json"`
搜不到。**下面每一条都以实跑为准。**

| RFC 主张 | 核对 | 结果 |
|---|---|---|
| §1.1 `--json` 是已发布的事实拼写 | `xpkg parse --json` ✓、`cache list --json` ✓ 均有输出 | **成立** |
| §1.3 `self env` 无 JSON | `--format json` / `--json` 均报 unknown option | **成立** |
| §1.4 `xpkg parse --json` 无 schemaVersion | 输出 `{"namespace":"","name":"7zip","form":"A"}` | **成立** |
| §1.5 `xlings interface` 的 outputSchema 全空 | `--list` 20 个 capability,**20/20** 只有 `exitCode` | **成立** |
| §1.6 CDB 早于 ninja 写出 | `ninja_backend.cppm:1549` `write_compile_commands`,其后才起 ninja | **成立** |
| §1.1 `pack --format` 语义冲突 | `pack --format bogus` → rc=2,人类文本走 **stderr** | **成立** |

### 1.1 新发现 F1:未知选项污染 stdout

```
$ mcpp cache list --format json
stdout: Error: unknown option: --format
stderr: (空)
rc:     1
```

对照 mcpp 自己的错误路径:

```
$ mcpp xpkg parse /nonexistent.lua
stdout: (空)
stderr: error: cannot open '/nonexistent.lua'
```

**mcpp 自己的 `ui::error` 是对的**(`src/ui.cppm:327`,写 stderr)。洞恰好开在参数解析
的边界上,而且代码在依赖里:

```
mcpplibs-x-cmdline/0.0.2/cmdline-0.0.2/src/cmdline.cppm:127
    std::println("Error: {}", result.error().message);   // ← stdout
```

于是当前有**四种**未知输入行为,而 RFC 只识别出后两种(且它们都还只在 #372 里):

| 场景 | 通道 | rc |
|---|---|---|
| 未知**选项**(`cache list --format …`) | **stdout** | 1 |
| 未知**值**(`pack --format bogus`) | stderr | 2 |
| #372 `ide snapshot --format yaml` | stdout | 3 |
| #372 `ide configure --format json` | stderr | 2 |

---

## 2. 这改变了什么

### 2.1 阶段 0 的范围划错了

RFC 的阶段 0 只规定未知**值**。但客户端探测能力时发的第一个请求,在旧版本上命中的
是未知**选项**。只规定值,等于把最常走的那条路留在污染状态。

**修正:阶段 0 必须覆盖选项层,而这意味着要动 `mcpplibs.cmdline`**(改成写 stderr,
或让 mcpp 不用它内建的错误打印)。这是 RFC 的行数估算里没有的一项跨包工作。

### 2.2 协商入口解决不了它被设计来解决的问题

RFC 给 `--protocol-version` 的理由是:

> 客户端启动时判断该走哪条路,不用先 spawn 一个可能失败的命令再解析错误消息

实测,在**任何尚未实现它的 mcpp** 上:

```
$ mcpp --protocol-version
stdout: Error: unknown option: --protocol-version
stderr: (空)
rc:     1
```

它自己就是「那个可能失败的命令」,而且失败与成功**同通道**。客户端仍然只能靠解析
stdout 内容来判断 —— 正是要避免的事。

换 `--json` 拼写也一样:两种拼写在旧版本上都走同一条未知选项路径。**所以拼写之争
(`--format` vs `--json`)对发现机制毫无影响**,RFC 用「布尔无法表达未知值分支」来论证
`--format` 是对的,但不要把它当成解决了发现问题。

**修正:唯一对旧版本稳健的客户端规则是「正向识别」**——

> 读 stdout,尝试 JSON 解析;解析成功**且**含 `schemaVersion` 与 `kind` 才认为拿到了
> 协议输出。**不得**以退出码 0、或「命令没报错」作为判据。

这条规则要写进 `docs/spec/`,并且是 envelope 必须**自识别**的理由:客户端没有别的
可靠信号。`--protocol-version` 仍值得有(新版本上省一次 spawn),但它是优化,不是
契约地基。

### 2.3 `destructive` 字段:同意,但它落在错误的一侧

RFC 提议在 envelope 里放 `destructive`。这对**已经拿到输出**的客户端有用,但
VSCode 的 untrusted-workspace 门需要在**执行之前**知道 —— 而拿到 envelope 的时候,
副作用已经发生了。

**修正:`destructive` 必须同时出现在两个地方**——

- envelope 里(事后自述,便于日志与审计)
- `mcpp --protocol-version` 的输出里,以「命令 → destructive」的静态表形式给出

后者才是 untrusted 门能用的东西。这也顺带回答 RFC §七 给 @sunrisepeak 的第 4 问:
接受这个字段,但只放 envelope 不够。

---

## 3. 修正后的阶段划分

改动集中在阶段 0 与阶段 1,阶段 2 之后与 RFC 一致。

### 阶段 0(前置)—— stdout 归属,而不只是「未知格式」

1. **`mcpplibs.cmdline` 的解析错误改写 stderr**,并给出非 0 退出码。跨包改动,需要
   发一个 cmdline 版本;mcpp 侧同步 bump。
2. mcpp 侧统一:未知**值**与未知**选项**都走 stderr + rc=2(用法错误)。
   —— RFC 原文提议未知格式「走 stdout + envelope」。**建议改为 stderr + rc=2**:
   一个还不知道自己该输出什么格式的请求,不该往协议通道里写东西;客户端按 §2.2 的
   正向识别规则,stdout 无 JSON 即判定为「不支持」,语义完整且不需要额外约定。
3. 加一条 e2e:对**每个**声明支持 `--format` 的命令,断言未知值与未知选项在通道、
   退出码上一致。这是防止 §1.1 那张表重新长出来的唯一办法。

### 阶段 1 —— `mcpp.wire` envelope

与 RFC 一致(从 #372 的 `src/ide/model.cppm` / `snapshot.cppm` 提升),补两点:

- envelope **必须自识别**:`schemaVersion` + `kind` 是客户端唯一的正向判据(§2.2)
- `--protocol-version` 的输出**包含命令 → destructive 的静态表**(§2.3)

### 阶段 2–4

与 RFC 一致。`--json` 永久别名、入口一处归一、不打 deprecation 警告 —— 这套做法在
本仓库有先例(`src/toolchain/compat.cppm`),照搬即可。

`pack --format tar|dir`:建议走 (a)**声明为例外**。它没有 stdout 机器输出,不参与本
协议;加 `--layout` 别名是为一个不存在的一致性付迁移成本。

---

## 4. 必须守住的纪律(RFC §四)—— 同意,补一条

RFC 说「envelope 的价值全在 `data` 被真正版本化和文档化上」,并以 xlings 的
20/20 空 outputSchema 为反例。核对属实。

补一条**可执行**的约束,否则「只增不改」会像那 20 个 schema 一样退化成口号:

> 每个 `kind` 的 golden fixture 必须**在测试里被反向验证**:改一个字段名要让测试变红。
> 只有「跑通了」而没有「改坏了会红」的 fixture,和没有 fixture 等价。

这一条不是文风要求。本轮工作里我自己就写过三个「因为错误的理由而通过」的测试:
一个 e2e 分支够不到它要测的模式,一个判据选成「跑不跑得起来」(在任何有宿主工具链
的机器上恒真),一个 `ldd` 解释器行造成的误报。三个都是靠**主动把实现改坏、看测试
会不会红**才发现的。

---

## 5. 与 #372 的关系

同意 RFC §五 的 A/B/C 拆分。补充一点:A 部分里的
`write_fresh_compile_commands` + 原子替换,**独立于本协议就应该合入** —— 现状是
`ofstream` 截断写,clangd 可能读到半截 JSON。那是既有缺陷,不该等协议设计定稿。

---

## 6. 待确认(在 RFC 七问之外)

1. **`mcpplibs.cmdline` 的解析错误改走 stderr** —— 这是跨包改动且影响所有使用者,
   是否接受?若不动依赖,替代方案是 mcpp 不用 `App::run()` 的内建错误打印、自己接管
   `ParseResult`(mcpp 侧约 20 行,但要覆盖每个命令入口)。
2. 未知格式**走 stderr + rc=2**(本文 §3 阶段 0.2)还是 RFC 原案的 **stdout +
   envelope**?我倾向前者,理由见 §3;但若客户端强烈希望「任何情况下 stdout 都是
   JSON」,后者也自洽 —— 需要先定,因为它决定 §2.2 的客户端规则怎么写。
3. `destructive` 静态表放进 `--protocol-version` 输出 —— 是否接受(§2.3)?
