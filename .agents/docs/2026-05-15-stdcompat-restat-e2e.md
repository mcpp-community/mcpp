# std.compat 支持 + cxx_scan restat + 增量零重编 E2E

> 2026-05-15 — 收尾三个遗留项，一个 PR 完成

## 1. 三个任务

### 1.1 std.compat 模块支持

**现状**：`import std.compat` 在语法层面被识别（`is_std_module()` 返回 true），会触发 std BMI 构建。但 `std.compat.cppm`（Clang）/ `bits/std_compat.cc`（GCC）从未被发现和编译，没有独立的 BMI 产出。

**Clang**：`~/.mcpp/.../xim-x-llvm/20.1.7/share/libc++/v1/std.compat.cppm` 存在。需要 `--precompile` 编译为 `pcm.cache/std.compat.pcm` + `std.compat.o`。
**GCC**：`bits/std_compat.cc` 在当前 xlings GCC 16.1.0 包中**不存在**。GCC 的 `import std.compat` 由 `bits/std.cc` 隐式覆盖（编译 `std.cc` 自动产出 `std.compat` 的 gcm）。因此 GCC **不需要额外处理**。

**改动点**：
1. `Toolchain` 新增 `stdCompatSource`（可选路径）
2. `clang.cppm::find_libcxx_std_module_source` → 同时查找 `std.compat.cppm` 路径
3. `clang.cppm::enrich_toolchain` → 设置 `stdCompatSource`
4. `stdmod.cppm` → 若 `stdCompatSource` 非空，额外编译 `std.compat.pcm` + `std.compat.o`
5. `BuildPlan` 新增 `stdCompatBmiPath` + `stdCompatObjectPath`
6. `ninja_backend.cppm` → 当 Clang + 有 std.compat 时，stage `std.compat.pcm` 和 `std.compat.o`
7. `flags.cppm` → 若有 std.compat，加 `-fmodule-file=std.compat=<pcm>`
8. E2E 测试：在 `38_llvm_modules.sh` 或新增测试中用 `import std.compat` 验证

**GCC 不变**：GCC 的 `import std.compat` 已经通过 `bits/std.cc` 的隐式 gcm 工作，无需额外构建。

### 1.2 cxx_scan restat（P2 重新实现）

**之前失败原因**：经调查，CI 失败是 GitHub Actions 缓存 fallback restore（`restore-keys` 匹配到旧 commit 的 `target/`），导致 gtest `.o` 是旧的但 ninja 认为 up-to-date。与 restat 无关 — PR #35 第二次运行（无任何代码改动）就成功了。

**方案**：重新实现 backup-compare-restore + `restat = 1`，与 `cxx_module` 的 BMI 保留模式完全一致。

**改动点**：`ninja_backend.cppm` 的 `cxx_scan` rule。

### 1.3 增量零重编 E2E

**改动点**：增强 `tests/e2e/39_llvm_incremental.sh`，新增一个验证步骤：
- touch `greet.cppm` 但不改内容
- rebuild with verbose
- 验证 SCAN 跑了但 MOD/OBJ 没跑（restat 截断了级联）

## 2. 实施计划

所有改动在一个 PR 中，按以下顺序实施。

### Step 1：std.compat — 工具链层
- `model.cppm`：`Toolchain` 加 `stdCompatSource` 字段
- `clang.cppm`：`find_libcxx_std_compat_source()` + `enrich_toolchain` 填充
- `clang.cppm`：`std_compat_build_commands()` — 两步编译（同 std）
- `clang.cppm`：`std_compat_bmi_path()` / `staged_std_compat_bmi_path()`

### Step 2：std.compat — 构建管线层
- `plan.cppm`：`BuildPlan` 加 `stdCompatBmiPath` + `stdCompatObjectPath`
- `stdmod.cppm`：若 `tc.stdCompatSource` 非空，额外编译
- `cli.cppm`：`prepare_build` 中把 `StdModule` 的 compat 路径传入 plan
- `ninja_backend.cppm`：stage `std.compat.pcm` + `std.compat.o`（cp_bmi 边）
- `flags.cppm`：Clang 时加 `-fmodule-file=std.compat=<staged_pcm>`

### Step 3：cxx_scan restat
- `ninja_backend.cppm`：backup-compare-restore + `restat = 1`

### Step 4：E2E 测试
- 新增 `tests/e2e/41_llvm_std_compat.sh`：`import std.compat` + C 函数使用
- 增强 `tests/e2e/39_llvm_incremental.sh`：touch 零重编验证

## 3. 不做的事

- GCC std.compat 额外构建 — GCC 的 `bits/std.cc` 隐式覆盖
- 改 E2E 04_incremental.sh（GCC） — 已有且工作正常
- macOS / Windows — 远期
