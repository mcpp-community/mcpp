# 机器可读输出协议 —— 拆分实施计划

> 设计:`2026-08-08-machine-readable-output-protocol-design.md`(对 RFC #379 的核对与修正)
> 已定:未知格式走 **stderr + rc=2**;`pack --format` **声明为例外**。

## 顺序与理由

设计文档 §4.5 把 CDB 引号缺陷排在阶段 1 之前,理由是「协议做得再好,也救不了一个内容
本身就坏掉的出口」。照此排:

```
W0  CDB 引号(§4.5)          —— 今天就在坏,且与协议正交,先修
W1  stdout 归属(阶段 0)      —— 未知选项/未知值统一 stderr + rc=2
W2  mcpp.wire 模块(阶段 1)   —— envelope + destructive + --protocol-version
W3  --format 归一(阶段 2)    —— --json 永久别名,入口一处归一
W4  接入(阶段 3a)            —— self env / xpkg parse / cache list
```

W4 之后的 `metadata` / `--configure-only`(阶段 3b/3c)**不在本次范围** —— 它们是新增
能力而非契约统一,且依赖 #372 的拆分结论。

## W0 —— CDB 的 `arguments` 带 shell 引号

**File:** `src/build/compile_commands.cppm`

- [ ] **Step 1** 红:带空格路径 + llvm 的单测,断言
      ① 任何 token 不以引号开头/结尾;② 带空格的路径是**一个** token
- [ ] **Step 2** 跑,确认 FAIL(今天两条都不满足)
- [ ] **Step 3** 让 `split_flags` 认引号。**顺序陷阱**:ninja 的 `$ ` 反转义必须发生在
      引号**内**,否则被引号包住的空格先把 token 切断 —— 那正是现在的 bug
- [ ] **Step 4** 绿
- [ ] **Step 5** Commit

判据不能写成「clangd 能用了」——在不含空格的路径上恒真,正是它至今没被发现的原因。

## W1 —— stdout 归属

**Files:** `src/cli.cppm`、`src/main.cpp`(+ 依赖 `mcpplibs.cmdline` 的处置)

- [ ] **Step 1** 红:e2e 断言未知**选项**与未知**值**在通道(stderr)与退出码(2)上一致
- [ ] **Step 2** 跑,FAIL(今天:未知选项 → stdout/rc=1;未知值 → stderr/rc=2)
- [ ] **Step 3** 实现。`mcpplibs.cmdline:127` 用 `std::println` 写 stdout,在依赖里 ——
      **本次不改依赖**,改为 mcpp 侧接管 `ParseResult`:不走 `App::run()` 的内建错误
      打印,自己输出到 stderr 并返回 2
- [ ] **Step 4** 绿
- [ ] **Step 5** Commit

## W2 —— `mcpp.wire`(独立模块)

**New:** `src/wire.cppm`

- [ ] **Step 1** 单测:envelope 形状(`schemaVersion` / `kind` / `destructive` /
      `mcpp.version` / `mcpp.protocol{min,max}` / `data` / `diagnostics`)
- [ ] **Step 2** 实现。`Diagnostic/Position/Range/Severity` 与 envelope 构造从 #372 的
      `src/ide/model.cppm`、`src/ide/snapshot.cppm` 提升(设计文档 §5-B)
- [ ] **Step 3** `mcpp --protocol-version`:输出 `{min,max}` **加命令 → destructive 静态表**
      (设计 §2.3 —— untrusted 门要在执行前知道)
- [ ] **Step 4** golden fixture,且**反向验证过**(改字段名要变红,设计 §4)
- [ ] **Step 5** Commit

## W3 —— `--format` 归一

**Files:** `src/cli.cppm`(入口归一)、各 `cmd_*.cppm`

- [ ] **Step 1** 单测:`--json` 与 `--format json` 产出**逐字节相同**
- [ ] **Step 2/3** 入口一处 `--json` → `--format json`;核心只见 `--format`
- [ ] **Step 4** 绿;`--json` 永久保留、**不打 deprecation 警告**
- [ ] **Step 5** Commit

`pack --format tar|dir` 不动(已定为例外),文档写明。

## W4 —— 接入 envelope

**Files:** `cmd_self.cppm`(新增 `self env --format json`)、`cmd_xpkg.cppm`、`cmd_cache.cppm`

- [ ] **Step 1** 每个 `kind` 一个 golden fixture(反向验证过)
- [ ] **Step 2** `self env` 覆盖 mcpp-vscode#8 §2.4:MCPP_HOME / registry / xlings home /
      index repos / default toolchain
- [ ] **Step 3** `xpkg parse` / `cache list` 包进 envelope(`schemaVersion` 随之而来)
- [ ] **Step 4** 绿
- [ ] **Step 5** Commit

## 平台特化的去处

按要求:平台差异进 `src/platform/`,协议本体独立 `.cppm`。

- 协议本体 → **`src/wire.cppm`**(新模块,不依赖任何命令)
- 路径/引号的平台差异 → 已在 `src/platform/`(`env::path_list_separator` 等);W0 的
  引号识别是**平台无关**的(引号本身两平台都要剥),不新增平台分支

## 交付

- 全部并入 **PR #385**(与 docs 一起)
- 版本已是 2026.8.8.3(刚发布)⇒ 本次**跳过版本 bump**,除非 CI 要求
- xlings pin 已是 2026.8.8.1(索引最新)⇒ 无需再抬
