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

## 4.5 追加优化项:CDB 的 `arguments` 带着 shell 引号(实测复现,不限 Windows)

用户报告 Windows 上 CDB 里的 `-fmodule-file=` 带多余双引号,每次构建后要手工删掉才能
配好 clangd。**是真问题,而且比报告的更严重** —— 在 Linux 上用带空格的项目路径复现了
同一形状:

```
'-fmodule-file=std=/tmp/.../my project/.../std.pcm       <- 闭合引号没了
'-fmodule-file=std.compat=/tmp/.../my project/.../
'-fprebuilt-module-path=/tmp/.../my project/.../pcm.cache'
```

前两条的**闭合引号被切掉了**:一个参数被从中间劈成两半,还带着一个孤立的开引号。
clangd 逐字 exec `arguments`(不经 shell),于是它拿到的是
`'-fmodule-file=std=/tmp/.../my` 和 `project/.../std.pcm'` 两个参数。

### 机制

四步,每一步单独看都合理:

1. `flags.cppm::shell_quote_arg` 的 `kNeedsQuote` 含反斜杠和空格 ——
   **Windows 路径必然含反斜杠**,所以在 Windows 上*每个*带路径的 flag 都被加引号;
   Linux 上只有路径含空格时才触发。这解释了为什么它被当成 Windows 专属问题。
2. 加引号是为了让 flag 在 ninja 的 `sh -c "<整条命令>"` 里存活 —— 对 ninja 是**对的**。
3. `compile_commands.cppm::split_flags` 把那条字符串重新切成 token,并撤销 ninja 的
   `$ ` / `$:` / `$$` 转义。
4. 但它**不认引号**:既不剥掉,也不把引号内的空格当作非分隔符。

### 这是同一个「注释写对了一半」

`split_flags` 自己的注释把原则写对了:

> CDB consumers like clangd exec the `arguments` array literally — no ninja
> involved — so escaped chars must be undone

它识别出**ninja 转义**要撤销,却漏了**同一个理由下**的 shell 引号。代码实现了原则的
一半。与本文档 §2 是同一形状。

### 判据与修法

**判据(必须是这个,不能是「clangd 能用了」)**:CDB 的 `arguments` 里,任何 token 都
不得以引号开头或结尾;且带空格的路径必须是**一个** token。前者防引号泄漏,后者防这次
发现的「劈成两半」。

两条路:

- **(a) 让 `split_flags` 认引号**(建议)。它本来就是那个分词器,分词器认引号是本分。
  注意顺序:ninja 的 `$ ` 反转义必须发生在**引号内**,否则被引号包住的空格会先把 token
  切断 —— 这正是现在发生的事。
- (b) 让 CDB 不再从「拼好的 shell 字符串」重新分词,而是让 argv 以 `vector<string>`
  一路传下来。更干净,但要动 `compute_flags` 的返回形状,范围大得多。

**回归测试**:带空格路径的项目 + llvm 工具链(gcc 不发 `-fmodule-file`,覆盖不到),
断言上述判据。这条在 Linux 上就能跑,不需要 Windows 机器 —— 而它至今没被发现,正是
因为所有 e2e 的项目路径都不含空格。

### 与本协议的关系

CDB 是 mcpp 已经在发的**机器可读输出**,只是没走 envelope。它今天就在破坏一个真实
客户端(clangd),而且用户在手工修补它。**建议把它排在阶段 1 之前**:协议做得再好,
也救不了一个内容本身就坏掉的出口。

## 5. 与 #372 的关系

同意 RFC §五 的 A/B/C 拆分。补充一点:A 部分里的
`write_fresh_compile_commands` + 原子替换,**独立于本协议就应该合入** —— 现状是
`ofstream` 截断写,clangd 可能读到半截 JSON。那是既有缺陷,不该等协议设计定稿。

---

## 6. 待确认(在 RFC 七问之外)

1. **`mcpplibs.cmdline` 的解析错误改走 stderr** —— 这是跨包改动且影响所有使用者,
   是否接受?若不动依赖,替代方案是 mcpp 不用 `App::run()` 的内建错误打印、自己接管
   `ParseResult`(mcpp 侧约 20 行,但要覆盖每个命令入口)。
2. `destructive` 静态表放进 `--protocol-version` 输出 —— 是否接受(§2.3)?

## 7. 已定(2026-08-08)

- **未知格式走 stderr + rc=2**(不是 stdout + envelope)。客户端按 §2.2 的正向识别
  规则,stdout 无 JSON 即判定为「不支持」,语义完整,且一个还不知道该输出什么格式的
  请求不往协议通道写东西。
- **`pack --format tar|dir` 声明为例外**,不加 `--layout` 别名。它没有 stdout 机器
  输出,不参与本协议;为一个不存在的一致性付迁移成本不值。

---

# 第二轮 review(2026-08-08,综合 @wellwei / @Ximiaw 反馈)

## R0. 先承认一件事:第一轮**没有做需求侧分析**

第一轮全部是供给侧 —— 核对 mcpp 现在的行为,找实现层缺陷。**没有**读
mcpp-vscode#8 / #5 的实际需求,也没有验证「提议的接口是否闭合了它们」。

wellwei 指出的四条里,**两条正是需求侧分析才会抓到、而第一轮漏掉的**(R1、R3)。
这一节存在的意义不是自责,而是记下判据:**契约设计里,「我方能提供什么」和
「对方需要什么」是两次独立的核对,做了前者不等于做了后者。**

## R1. `--json` 的 payload 兼容 —— 第一轮把它当纯拼写别名,错了

实测:

```
mcpp cache list --json   顶层键 = ["entries", "root"]        ← 没有 envelope
mcpp xpkg parse --json   顶层 = {"namespace","name","form"}  ← 没有 envelope
```

第一轮跟着 RFC 写「入口一处归一化,核心只见 `--format`」,默认了**拼写兼容 ⇒ payload
兼容**。不成立:如果核心随后统一输出 `{schemaVersion, kind, data, diagnostics}`,任何
依赖旧顶层结构的消费者都会断,**包括本仓库的 e2e**。

**修正 —— 三选一必须先定,否则 wire v1 不能冻结:**

| 方案 | 含义 | 代价 |
|---|---|---|
| **(A) `--json` 永久 legacy payload,`--format json` 才是 envelope** | 两条出口,语义不同 | 每个命令两份序列化;但**没有任何现存消费者会断** |
| (B) 老命令保留旧顶层,只做增量字段;envelope 只用于新命令 | 老命令永远拿不到 `schemaVersion` | mcpp-vscode#8 §7.1 的请求落空 |
| (C) 有意 breaking,给迁移窗口 | 干净 | 与「`--json` 永久保留、不打警告」的既定纪律冲突 |

**倾向 (A)**,理由是它与本仓库既有范式一致(`compat.cppm`:旧拼写永久接受,核心只见
规范形式),而这里要永久接受的是**旧 payload**,不是旧拼写。代价是明确且有界的。

## R2. `destructive` —— 与第一轮结论一致,但 wellwei 补了一刀

第一轮已指出它做不了 preflight 门(§2.3),wellwei 独立确认,并补充:
**单个 bool 也不够** —— 「只写 CDB」「写全局缓存」「可能触网」「执行工作区代码」是四种
不同的边界,VSCode 的门需要区分。

**修正**:`--protocol-version` 的静态表不是 `destructive: bool`,而是**效应集合**:

```jsonc
"commands": {
  "self env":       { "effects": [] },
  "xpkg parse":     { "effects": [] },
  "metadata":       { "effects": ["read-project"] },
  "metadata --resolved": { "effects": ["read-project","write-cache","network"] },
  "build --configure-only": { "effects": ["read-project","write-project","write-cache","exec-build-script"] }
}
```

`exec-build-script` 单独成项,因为 `build.mcpp` 会执行工作区里的代码 —— 那是 untrusted
门唯一真正在乎的一条。

## R3. `self env` 首次运行会初始化 —— 真问题是**作用域**,不是真假

wellwei 指出 `env_report()` 调 `config::load_or_init()`,不是只读。核实,并把两次测法
都记下来,因为**第一次测法不足以定责**。

第一次:只在全新 `MCPP_HOME` 上跑一次 `self env`,看到 6 个文件就下结论 —— 没有排除
「任何 mcpp 命令的首次初始化」。加对照后:

| 命令 | 全新 home 上创建项数 |
|---|---|
| `mcpp --version` | 0 |
| **`mcpp xpkg parse <f> --json`** | **0** |
| **`mcpp cache list --json`** | **0** |
| `mcpp self env` | **6**(`config.toml` `registry` `cache` `bin` `build-cache` `log`) |
| 已初始化 home + `self env` | **无变化,只读** |

两条结论:

1. **「首次运行必然初始化」不成立。** mcpp-vscode#8 实际消费的两个命令
   (`xpkg parse` / `cache list`)**什么都不建**。所以按「任何写盘都算」的口径,
   `destructive` **仍然携带信息** —— 它不会退化成恒真。
2. `self env` 是首次运行触发一次性初始化,**不是每次都写**。

**因此真正的分叉是作用域,不是真假。** untrusted-workspace 门在乎的是「碰不碰用户的
项目」「执不执行项目代码」;mcpp 建**自己的 home** 不属于这一类。两条路:

- **(a) 效应集合,作用域写在名字里**(倾向)。`self env` = `["init-mcpp-home"]`,
  客户端自己决定要不要在意;既不把它打成「危险」,也不让 `false` 被读成「什么都不写」。
- (b) 直接定「作用域 = 工作区」,`self env` 标 `false`。更简单,但 spec 必须写死
  「本字段不涵盖 mcpp 自身 home 的初始化」,否则 `false` 有歧义。

方法教训单独记:**「跑一次看有没有文件」不足以给副作用定责,必须有一个不走同一路径的
对照命令。** 这一处若不加对照,就会得出「所有命令都会初始化,所以这个字段没意义」——
一个由测法而非事实支撑的结论。

## R4. 阶段 0 的边界比第一轮画的更宽

实测退出码:

```
未知子命令      rc=127   (cli.cppm:610,stderr,正确)
未知选项        rc=2     ← 本轮 W1 已修(原 rc=1 + stdout)
未知值(pack)   rc=2     (stderr,原本就对)
未捕获异常      rc=70    (main.cpp,stderr)
```

W1 修掉了最坏的一条。但 wellwei 说得对:**光接管 parse error 不够**,还要把
usage / runtime / internal 的 rc 映射写成契约,并覆盖异常边界 —— 否则客户端仍然要靠
猜。这条现在是 `docs/spec/` 的内容,不是代码。

## R5. 全局 `schemaVersion` 把不相关的 kind 耦合在一起

wellwei 的观察成立。一个全局版本号意味着 `mcpp.env` 的字段变更会推高 `mcpp.xpkg` 的
版本,客户端无从判断哪个 kind 真的变了。

**修正**:envelope 版本与 kind 数据版本分开;并且 `--protocol-version` 不能只回
`{min,max}`,要回**它支持哪些 kind 及各自版本**:

```jsonc
{ "envelope": { "min": 1, "max": 1 },
  "kinds": { "mcpp.env": 1, "mcpp.xpkg": 1, "mcpp.cache": 1 } }
```

## R6. Ximiaw 的实测修正:supported-key 词汇表只覆盖部分段

他实测 24 段 + 源码核对,结论是「parser 已掌握每段 supported keys」**只对少数段成立**:

- 有白名单的:`[build]`、`[resources]` 等 6 处常量(`src/manifest/toml.cppm`)
- 开放词汇段(dependencies / indices / toolchain …):无键清单,只校验值形态
- **其余固定段(package / profile / runtime / xlings / workspace / pack):未知键被静默
  吞掉**,合法键散在解析逻辑里

所以「顺带导出词汇表」不是现成的序列化。**但第三档的收敛有独立价值**:把「打错键名
毫无提示」变成警告,对 mcpp 自身就是健壮性收益 —— 这一条应当独立于本协议推进,不该
被 wire v1 阻塞。

## R7. 修正后的范围

| | 第一轮 | 本轮修正 |
|---|---|---|
| W0 CDB 引号 | 做 | **不变**(已完成) |
| W1 stdout 归属 | 做 | **已完成**,但 rc 映射契约要补进 spec(R4) |
| W2 `mcpp.wire` | envelope + `destructive: bool` | envelope + **效应集合**;`--protocol-version` 回 **kinds 及各自版本**(R2/R5) |
| W3 `--format` 归一 | 纯拼写别名 | **先定 payload 兼容策略(R1),否则不能冻结 v1** |
| W4 接入 | self env ~15 行 | **先拆只读 resolver**(R3);`cache list` / `xpkg parse` 按 R1 的结论决定走哪条 |
| 新增 | — | manifest 固定段的 supported-key 收敛(R6),**独立推进** |

**结论:W2/W3/W4 在 R1 定案前不应动手** —— 那是唯一会决定 wire v1 形状的分叉。W0 与
W1 与它正交,已完成。
