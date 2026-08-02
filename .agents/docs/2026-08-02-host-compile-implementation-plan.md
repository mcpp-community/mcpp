# 宿主编译单一生产者 — 实施计划

> 设计:`.agents/docs/2026-08-02-host-compile-single-producer-design.md`
> 基线:main @ 7169332(2026.8.2.1 已发布)· 本批次发 **2026.8.2.2**

**Goal:** 让「宿主编译 flag」只有一个生产者,token 是真源、字符串由 token 渲染;
`build.mcpp` 因此自动获得主构建的全部能力(方言、模块、引号、deployment target),
MSVC 下的模块门可以删除。

---

## Global Constraints

- **版本号只改两处**:`mcpp.toml:3`、`src/toolchain/fingerprint.cppm:21` → `2026.8.2.2`。
  `.xlings.json` 是 bootstrap pin,**发布并上架后**才单独 bump。
- **第 1 阶段是纯重构,必须零行为变化**:归一化 `build.ninja` diff 为空,
  且 `stdmod` 的 `std_build_commands` 逐字节相同 —— 否则所有用户的 std BMI 缓存
  失效重编(缓存目录名派生自含该命令的元数据,`stdmod.cppm:102,135`)。
- **新模块独立**:生产者放 `src/toolchain/hostflags.cppm`,**不往
  `build_program.cppm` 的匿名 namespace 加函数**(设计 §6.2)。
- **差异必须显式**:三处装配今天有真实分歧(见下表),共享后要成为**具名选项**,
  不是被抹平,也不是保留成无理由的开关。
- **每轮验证双工具链 + 清缓存**:`rm -rf target && mcpp cache clean`,
  gcc@16.1.0 与 llvm@22.1.8 各跑一遍;先在基线 commit 上做同法对照。

### 今天的三处分歧(必须保留并显式化)

| 关注点 | `flags.cppm` | `build_program.cppm` | `stdmod.cppm` |
|---|---|---|---|
| clang 有 cfg 时是否绕过 | **全平台绕过** | **只在 Linux 绕过**;macOS/Windows 信 cfg 并返回空(`:214-221` 有理由:macOS 链接要 `needs_explicit_libcxx` 那条路负责 libc++abi/unwind) | 全平台绕过(`:232` 说 `--no-default-config` 在所有平台安全) |
| binutils `-B` | 有(非 musl/mingw 且 libstdc++) | 有(非 musl) | 无 |
| `tc.linkRuntimeDirs` | 走 `depRuntimeLibraryDirs` 另一条路 | `-L` +(仅 ELF)`-rpath` | 无 |
| deployment target | 有 | 有(#332 才补) | 有 |

⇒ 选项面:`clangCfgBypass{Always,LinuxOnly}`、`binutilsB{bool}`、
`runtimeDirs{bool}`。**每个选项在头文件里必须写清"谁需要它、为什么"**,否则
就是把三处推导变成一处推导 + 三个魔法开关。

---

## Task 0:分支与版本号

- [ ] 建分支 `feat/host-compile-single-producer`
- [ ] `mcpp.toml:3` 与 `fingerprint.cppm:21` → `2026.8.2.2`;`.xlings.json` 不动
- [ ] `bash .github/tools/check_version_pins.sh` 通过
- [ ] 提交

## Task 1:基线快照(零 diff 的对照物)

在**动任何代码之前**先取基线,否则无法证明零 diff。

- [ ] **Step 1:** 对 gcc 与 llvm 各生成一次 `build.ninja` 并归一化保存

```bash
snap() {  # $1 = 标签
  rm -rf target; mcpp cache clean >/dev/null
  mcpp build >/dev/null
  find target -name build.ninja | head -1 | xargs cat \
    | sed -E 's#/[^ ]*/target/[0-9a-f]{16}#<FP>#g' > /tmp/ninja-$1.txt
}
snap base-gcc
sed -i 's|^default = "gcc@16.1.0"$|default = "llvm@22.1.8"|' mcpp.toml
snap base-llvm
git checkout -- mcpp.toml
```

- [ ] **Step 2:** 保存 `std_build_commands` 基线

```bash
find ~/.mcpp/build-cache -name '*.json' -path '*std*' \
  | xargs -I{} sh -c 'python3 -c "import json,sys;d=json.load(open(sys.argv[1]));print(sys.argv[1]);print(d.get(\"std_build_commands\"))" {}' \
  > /tmp/stdcmd-base.txt
```

## Task 2:token 优先的 seam(`linkmodel.cppm`)

**Files:** `src/toolchain/linkmodel.cppm`

**Produces:**
- `std::vector<std::string> LinkModel::compile_tokens(const PathEscape&) const`
- `std::vector<std::string> ClangDriverModel::compile_tokens(const PathEscape&) const`
- 现有 `compile_flags()` 改为 `render_tokens(compile_tokens(esc))`

- [ ] **Step 1:** 加 `render_tokens`(每个 token 前置一个空格后拼接)

```cpp
// token 是真源,字符串是渲染结果 —— 不是反过来。历史上只有字符串形态,
// 于是需要 argv 的消费者(build_program)只能自己重写一遍装配;
// #332 的 split_ws 是同一个形状的产物。
inline std::string render_tokens(const std::vector<std::string>& tokens) {
    std::string out;
    for (auto const& t : tokens) { out += ' '; out += t; }
    return out;
}
```

- [ ] **Step 2:** `LinkModel::compile_tokens` —— 把现有 `compile_flags` 逐句改成
      `push_back`,顺序不变:`--sysroot=<p>`、每个 `-isystem<p>` / `-idirafter<p>`
- [ ] **Step 3:** `ClangDriverModel::compile_tokens` —— `--no-default-config`、
      `-nostdinc++`(**两个独立 token**)、每个 `-isystem<p>`
- [ ] **Step 4:** 两个 `compile_flags` 改为 `return render_tokens(compile_tokens(esc));`
- [ ] **Step 5:** 单测:对同一 `LinkModel` / `ClangDriverModel`,
      `render_tokens(compile_tokens(esc)) == 旧实现的字符串`(把旧实现内联进测试当预言)
- [ ] **Step 6:** 提交

## Task 3:`hostflags.cppm` 生产者

**Files:** 新建 `src/toolchain/hostflags.cppm`

**Produces:**

```cpp
export module mcpp.toolchain.hostflags;

struct HostFlagOptions {
    // clang 自带 cfg 时是否绕过它。主构建全平台绕过(可复现、不依赖
    // 安装期生成物);build.mcpp 的宿主 helper 只在 Linux 绕过 —— macOS 上
    // 链接 libc++abi/unwind 由主构建的 needs_explicit_libcxx 负责,helper
    // 自己重复一遍会产生 undefined __cxa_*(build_program.cppm 原注释)。
    enum class CfgBypass { Always, LinuxOnly } cfgBypass = CfgBypass::Always;
    // binutils -B:GCC/libstdc++ payload 才需要(musl 与 mingw 自带 as/ld)。
    bool binutilsPrefix = true;
    // tc.linkRuntimeDirs 的 -L(+ELF 上的 rpath):让产物能加载私有运行库。
    bool runtimeLibDirs = false;
    std::string macosDeploymentTarget;   // 已解析值;空 = 不发
};

// 宿主编译 flag,token 形态。三个消费者的唯一生产者。
std::vector<std::string> host_compile_tokens(const Toolchain& tc,
                                             const HostFlagOptions& opt,
                                             const PathEscape& esc);
```

- [ ] **Step 1:** 实现,顺序**严格照抄 `flags.cppm:319-352` 的现有顺序**
      (dm → deployment → lm),因为顺序影响渲染出的字符串
- [ ] **Step 2:** 单测 `tests/unit/test_hostflags.cpp`:
      - 每个 `CompilerId` 都返回自洽结果(方言可取、无空 token)
      - `CfgBypass::LinuxOnly` 在非 Linux 上对 clang 返回空
      - deployment target 只在 macOS 且非空时出现
- [ ] **Step 3:** 提交

## Task 4:三个消费者接入(**零 diff 验收**)

- [ ] **Step 1:** `build_program.cppm::host_base_flags` → 调
      `host_compile_tokens(tc, {CfgBypass::LinuxOnly, !isMusl, true, dt}, plainEsc)`,
      函数体删空。注意 `plainEsc` 是恒等转义(argv 不需要 ninja/shell 转义)。
- [ ] **Step 2:** `stdmod.cppm:238-253` → `render_tokens(host_compile_tokens(tc,
      {CfgBypass::Always, false, false, dt}, shellEsc))`
- [ ] **Step 3:** `flags.cppm:319-337` 的**编译侧** → `render_tokens(
      host_compile_tokens(tc, {CfgBypass::Always, …, false, dt}, ninjaEsc))`;
      链接侧(`link_toolchain_flags` / `f.sysroot` / `llvmRootForStdlib`)保持不动
- [ ] **Step 4:** **零 diff 验收**

```bash
snap after-gcc;  diff /tmp/ninja-base-gcc.txt  /tmp/ninja-after-gcc.txt   # 必须空
snap after-llvm; diff /tmp/ninja-base-llvm.txt /tmp/ninja-after-llvm.txt  # 必须空
# std_build_commands 逐字节
diff /tmp/stdcmd-base.txt /tmp/stdcmd-after.txt                            # 必须空
```

非空即说明抽错了 —— 回到 Task 2/3 找顺序或分支差异,**不要**改基线迁就。

- [ ] **Step 5:** 单测 + e2e(89/92/110/111/112/124/125/143/144/145/164/168/179/181),
      gcc 与 llvm 各一遍
- [ ] **Step 6:** 提交

## Task 5:`host_program_argv` + 删 MSVC 模块门

**Files:** `src/toolchain/hostflags.cppm`(或新建 `hostprogram.cppm`)、
`src/build/build_program.cppm`

- [ ] **Step 1:** 把 build.mcpp 的 argv 组装(方言、`forceCxxLangArgv`、输出前缀、
      静态运行时、模块 BMI 处理)收进生产者侧,`build_program.cppm` 只负责
      「读源文件 → 判定 imports → 调用 → 执行 → 解析指令」
- [ ] **Step 2:** 模块处理按 `bmi_traits(tc)` 分派,含 MSVC 的
      `/interface /TP /ifcOutput` + `/reference <name>=<ifc>`
      —— 与主构建同一张表,不新写
- [ ] **Step 3:** **删除** `build_program.cppm` 里
      `import mcpp; / import std; not yet supported under MSVC` 那道门
- [ ] **Step 4:** `tests/e2e/180_msvc_build_mcpp.sh` 第三段:
      从「断言拒绝」改为「断言 `import std;` + `import mcpp;` 可用」
- [ ] **Step 5:** e2e 181 参数化补 MSVC 一列(`# requires: windows msvc`,
      单独脚本 183 或在 180 内)
- [ ] **Step 6:** 提交

## Task 6:能力矩阵守卫

- [ ] **Step 1:** `tests/unit/test_hostflags.cpp` 增一条:遍历所有已知
      `CompilerId`,断言 `host_compile_tokens` + `dialect_for` + `bmi_traits`
      三者都能给出完整答案(方言 id 非空、`forceCxxLangArgv` 非空、
      `outputExePrefix` 非空、bmi 的 `bmiDir`/`bmiExt` 非空)。
      **新增工具链族忘了接 = 测试失败**,而不是运行期 `not yet supported`。
- [ ] **Step 2:** 提交

## Task 7:文档

- [ ] `docs/07-build-mcpp.md` / `docs/zh/07-build-mcpp.md`:删掉
      「MSVC 下不支持 import」的说明,改为陈述已支持
- [ ] 设计文档 §5 能力矩阵勾掉 MSVC 一列
- [ ] 提交

## Task 8:PR → CI 全绿 → 合入

- [ ] 本机全量:`mcpp test`、`tests/e2e/run_all.sh`、`check_version_pins.sh`
- [ ] 开 PR,盯 CI(**重点看 macOS/Windows**:零 diff 只在本机证过两条工具链,
      Windows 的 MSVC 腿只有 CI 能验)
- [ ] `gh pr merge --squash --admin --delete-branch`

## Task 9:Release + 生态闭环

- [ ] 触发 release.yml,四平台产物
- [ ] 镜像 xlings-res 双端;失败则**本地 gtc 补传**
      (token 在 `~/.config/gitcode-tool/config.json`,用 repo 的 gtc + `/usr/bin/python3`)
- [ ] **两端独立 GET + sha256 核验**(不信 workflow 绿灯)
- [ ] xim-pkgindex bump PR(Sunrisepeak 账号合),sha256 逐个对照
- [ ] 隔离 workspace 真装 `2026.8.2.2`,跑 e2e 179/181/89/112
- [ ] **包上架后**才 bump `.xlings.json` bootstrap pin,单独 PR
      (索引 artifact 传播滞后会让这个 PR 假红:`not found` ≠ 回归,等传播后重跑)

---

## 实施记录(只有 CI 能发现的)

| 发现 | 教训 |
|---|---|
| **扩大 `build_program.cppm` 的匿名 ns 同样触发 clang 误编译** | 把 `build_mcpp_module` 在原地改写加大 → macOS 全部 build.mcpp e2e 段错误,和 PR#332 一模一样。约束不是「别加新函数」,是**「别再往那个 ns 加代码」**。修法=整块搬到 `src/build/hostprogram.cppm` |
| **「扫缓存比对字节等价」是假验证** | std 缓存共享且累积,两次快照都含**陈旧条目**,diff 恒为空 —— 我因此放过了一次真实的字符串改动(`-stdlib=libc++` 位置)。**把主张写成测试**:用合成的 `ClangDriverModel`/`ToolchainLinkModel` 直接断言渲染出的字面串 |
| **`-stdlib=libc++` 的位置是兼容面** | 它进 `std_build_commands` → 进 metadata → **决定 std 缓存目录名**。挪一个 flag = 让每个用户的 std BMI 全量失效 |
| **「信任 cfg」不等于「什么都不发」** | 生产者在 trust-cfg 分支提前 `return`,把 deployment target 也跳过了;而 macOS 上 build.mcpp 走的正是这条分支 → std BMI 配置不匹配。旧的手写实现把它放在**最前、无条件**,注释还专门写了 "FIRST and unconditionally" —— 重构时要读懂那句话为什么在 |
| **删掉能力门 = 死代码变活代码** | `-x none` 常年无条件发出,只因 MSVC 到不了那里才无害。门一删,cl 立刻 `D9002: ignoring unknown option '-x'`。**删门时要把门后所有「反正到不了」的分支重新审一遍** |

## Self-Review

**设计覆盖**:§4.1 token 生产者 → Task 2/3;§4.2 三种渲染 → Task 4;
§4.3 build.mcpp 一等消费者 → Task 5;§5 能力矩阵 → Task 5/6;
§6.1 字节等价 → Task 1 + Task 4 Step 4;§6.2 独立模块 → Task 3 约束;
§6.4 双工具链验证 → Global Constraints。

**已知风险**:`compute_flags` 服务每一次构建,是全仓库 blast radius 最大的函数
之一。缓解就是零 diff 硬约束 —— 它把"重构对不对"变成一个可机器判定的问题。
