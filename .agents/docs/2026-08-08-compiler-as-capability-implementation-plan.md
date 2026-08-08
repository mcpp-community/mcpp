# 编译器是能力 —— 跨仓实施计划

> **For agentic workers:** 逐 task 执行,每个 task 自带 red→green→commit。**T1 是所有后续 task 的前提**(没有它,后面每一步的观测都不可信)。

**Goal:** 让 mcpp 停止依赖编译器的安装期配置,把「用哪份 libc」收敛成一个权威;顺带把已经污染的产物清干净,并让多 subos 构建真正可用。

**设计文档:**
- `.agents/docs/2026-08-07-xlings-as-runtime-substrate-design.md`(运行时底座;S1/S3 与撤销 `c_runtime` 的理由)
- `.agents/docs/2026-08-08-payload-version-and-contract-drift-design.md`(本计划的主体)

**Tech Stack:** C++23 modules,gtest(`tests/unit/`),shell e2e(`tests/e2e/`),Lua(mcpp-index)。

## Global Constraints

- **不改 payload**。`patchelf_walk`(让编译器自己能跑)保留;**写 payload 内配置的一律删掉**。
- **权威唯一**:`--runtime` → `[xlings] subos` 的 `subos_info.runtime` → **报错,不猜**。删掉「扫目录挑一个」。
- **平台差异下沉** `src/platform/`,`build/` 里不写 `#ifdef`。
- **每一处「注释描述行为」必须有对应断言**(R-C)。本轮已发现三处注释与代码不符。
- **跨仓 wire format 的 fixture 取自写入方**(R-A),不得手写。
- **一条测试若为覆盖某路径而存在,必须先断言走了那条路径**(R-B)。
- **不新增任何让产物链宿主 libc 的开关**(8-07 §5.2 已否)。

---

### T1: 三处「注释撒谎」先钉住 —— 后续观测的地基

**Files:** `tests/unit/test_linkmodel.cpp`(新断言)、`src/xlings.cppm`、`src/toolchain/post_install.cppm`(改注释)

本轮发现三处注释描述了代码没有的行为。**先让它们不能再撒谎**,否则后面每一步都可能在错误的前提上推理。

- [ ] **Step 1** 写失败单测:`find_sibling_tool` 的注释说「first (highest)」,断言**两个版本目录时返回的是排序后最高的那个**
- [ ] **Step 2** 跑,确认 FAIL(今天返回 readdir 第一个)
- [ ] **Step 3** 二选一并使注释与代码一致:①真的排序;②改注释为「未定义顺序」并让调用方不得依赖。**本计划选 ②**——T2 会让它不再被用于选版本
- [ ] **Step 4** 另两处:`fixup_gcc_specs` 的「Idempotent」(跨 home 不幂等)、`execute.cppm` reader 的「旧缓存视为 miss」(T5 修)——先把注释改成事实
- [ ] **Step 5** Commit

---

### T2: 权威降级链 —— 没有权威就不进 PayloadFirst

**Files:** `src/toolchain/probe.cppm`、`src/xlings/subos_info.cppm`、`src/build/prepare.cppm`
**Test:** `tests/unit/test_toolchain_probe.cpp`(新)

**Interfaces:**
```cpp
// probe 不再自己选版本;版本由权威给出
std::optional<PayloadPaths> probe_payload_paths(
    const std::filesystem::path& compilerBin,
    std::string_view runtimeBinding);   // "glibc@2.39";空 ⇒ 不进 PayloadFirst
```

- [ ] **Step 1** 单测:给定 `glibc@2.39` 且 home 内有 2.39/2.44 两份 ⇒ **必须返回 2.39**;给定空 binding ⇒ **返回 nullopt**(不猜)
- [ ] **Step 2** 跑,FAIL
- [ ] **Step 3** 实现;`prepare.cppm` 按 `--runtime` → `[xlings] subos` 的 `subos_info.runtime` 顺序解析,末级不猜
- [ ] **Step 4** 跑,PASS
- [ ] **Step 5** Commit

---

### T3: fingerprint 加入 runtime 轴 —— 多 subos 的前置

**Files:** `src/toolchain/fingerprint.cppm`、`tests/unit/test_fingerprint.cpp`

不加这条,切 subos 不改 fingerprint ⇒ `target/<fp>/` 被复用 ⇒ 里面是对另一份 glibc 编译的对象。**比今天更坏。**

- [ ] **Step 1** 单测:两个只有 `runtimeBinding` 不同的输入 ⇒ **fingerprint 必须不同**
- [ ] **Step 2/3/4** red → 加字段 → green
- [ ] **Step 5** Commit

---

### T4: GCC 也显式发 loader/rpath + clean specs

**Files:** `src/toolchain/linkmodel.cppm`、`src/build/flags.cppm`、`src/toolchain/post_install.cppm`(删 `fixup_gcc_specs`)
**Test:** `tests/unit/test_linkmodel.cpp`、`tests/e2e/201_gcc_clean_specs.sh`(新)

**已实测**:`-Wl,--dynamic-linker` 压得过 specs;`-specs=<无 `+` 定义>` 是替换;`g++ -dumpspecs` 给内建 specs(`/tmp` 路径 0 条);真实构建(模块 + `import std` + 共享库)RUNPATH 68→2、死路径 0、可运行。

- [ ] **Step 1** e2e:gcc 构建的产物 **RUNPATH 中不得有 `/tmp/tmp.`**,且 interpreter 是权威指定的那份,且能跑
- [ ] **Step 2** 跑,FAIL(今天有 34+ 条)
- [ ] **Step 3** ①去掉 `linkmodel` 的 `if (clangDriver)` 门;②构建期生成 clean specs 到 **build dir**,`-specs=` 传入;③**删掉 `fixup_gcc_specs`**(保留 `patchelf_walk`)
- [ ] **Step 4** 跑,PASS
- [ ] **Step 5** Commit

---

### T5: 并回 #377 的两处修复

**Files:** `src/xlings/subos_info.cppm`、`src/build/execute.cppm`、`tests/unit/test_subos_info.cpp`、`tests/e2e/200_subos_env_reaches_program.sh`

已在 `fix/fast-run-stale-cache-subos` 分支上完成并 18/18 CI 通过,**原样带过来**:

- [ ] **Step 1** `git cherry-pick` 该分支的三个 commit(wire format / 旧缓存 / e2e fixture)
- [ ] **Step 2** 跑单测 + e2e,确认仍绿
- [ ] **Step 3** Commit(或保留 cherry-pick 的原始提交)

---

### T6: D4 沙箱 xlings 版本比对 + D5 陈旧 sysroot

**Files:** `src/fallback/xlings_binary.cppm`、`src/doctor.cppm`、`src/fallback/probe_sysroot.cppm`

- [ ] **Step 1** 单测/e2e:vendored xlings 版本**低于 pin** ⇒ 被替换;**高于 pin** ⇒ 不动
- [ ] **Step 2** red
- [ ] **Step 3** 实现;doctor 增加两条 finding:①xlings 低于 pin;②`--sysroot` 指向当前项目之外
- [ ] **Step 4** green
- [ ] **Step 5** Commit

---

### T7: 文档 + 版本 + PR

- [ ] `docs/03-toolchains.md`:权威降级链、`[xlings] subos`、多 subos 构建
- [ ] `docs/05-mcpp-toml.md`:`[xlings] subos` 与 runtime 的关系
- [ ] 中文版同步
- [ ] `MCPP_VERSION` / `mcpp.toml` → `2026.8.8.2`;`kXlingsVersion` → 最新;`check_version_pins.sh` 通过
- [ ] 全量 `mcpp test` + 关键 e2e
- [ ] 开 PR

---

### T8: mcpp-index 图形栈重新落地(D1 修好之后)

**Files（mcpp-index）:** `pkgs/c/compat.glx-runtime.lua`、`pkgs/c/compat.glfw.lua`

- [ ] **Step 1** 新增 e2e/验证:**装 `xim:graphics` 前后**,各跑一次与图形无关的成员,断言产物 `PT_INTERP` 与 `readelf -V` 的 glibc 符号上界**不变**。这是上次事故真正缺失的那个测试
- [ ] **Step 2** 重新应用 `f44e896` 的两个文件改动(deps 放平台层)
- [ ] **Step 3** CI 全绿后合入

---

## Self-Review

| 设计文档条目 | Task |
|---|---|
| 8-08 §3.1 原则(编译器当能力) | T4 |
| 8-08 §3.2 权威降级链 / D1 | T2 |
| 8-08 §3.5① 去掉 clangDriver 门 | T4 |
| 8-08 §3.5② clean specs 进 build dir | T4 |
| 8-08 §3.5③ 删 `fixup_gcc_specs` / D6 | T4 |
| 8-08 §3.6 多 subos + fingerprint 前置 | T2 + T3 |
| 8-08 D4 沙箱 xlings / D5 sysroot | T6 |
| 8-08 D2 wire format / D3 旧缓存 | T5 |
| 8-08 §4.2 R-A/R-B/R-C | T1(R-C)+ 各 task 的测试形态 |
| 8-07 §1.5 图形栈迁移 | T8 |
| 8-07 §3-S2 不加链宿主 libc 的开关 | Global Constraints(不做) |

**未覆盖且是有意的**:8-07 §7「xlings 落盘 exports」—— 8-08 已说明它不再是关键路径(权威改为 `subos_info.runtime`)。Q6(交叉/musl/MinGW 下 `-specs=` 替换)在 T4 的 e2e 里只覆盖 native;**其余平台列为 PR 中显式声明的未验项**,不假装验过。
