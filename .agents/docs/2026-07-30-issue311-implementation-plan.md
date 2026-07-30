# BMI staging 原语 + BMI 缓存根收敛 — 实施计划

配套设计：`2026-07-30-issue311-bmi-staging-and-cache-root-design.md`
关联 issue：#311
建议目标版本：**2026.7.30.1**（常规迭代）

单 PR 交付。阶段有依赖顺序：**P1 → P2 → P3**（P3 的 ninja 文本依赖 P2 的子命令存在，
P2 的判据依赖 P1 收敛后的缓存根不再随 cwd 漂移）。P4/P5/P6 可与 P3 并行收尾。

---

## P1 — 单一 home 解析器（`mcpp.home`）

**新建** `src/home.cppm`：

```cpp
export module mcpp.home;
import std;
import mcpp.platform;

export namespace mcpp::home {
std::filesystem::path root();      // MCPP_HOME
std::filesystem::path bmi_root();  // root()/bmi
}
```

- `root()` = 把 `src/config.cppm:313-353` 的 `default_mcpp_home()` + `home_dir()`
  **逐字搬迁**（含 `USERPROFILE` 分支、self-contained 探测、`target/` 与 `data/xpkgs/`
  两条 disqualify）。两个函数都是纯函数、无副作用，搬迁是安全的。
- `bmi_root()` = `root() / "bmi"`。

**改动点（4 处调用方 + 3 处旧实现）**：

| 文件 | 位置 | 改动 |
|---|---|---|
| `src/config.cppm` | :313-353 | 删除两个本地函数 |
| `src/config.cppm` | :500 / :510 | `cfg.mcppHome = mcpp::home::root()`；`cfg.bmiCacheDir = mcpp::home::bmi_root()` |
| `src/toolchain/stdmod.cppm` | :179-187 | 整体替换为 `return mcpp::home::bmi_root();`（新增 `import mcpp.home;`） |
| `src/build/prepare.cppm` | :2826-2832 | 内联 lambda → `mcpp::home::root()` |

**编译系统**：`mcpp.toml` 若显式列源文件需同步；当前是 `src/**` 推导（`mcpp build -v`
里 `Inferred sources`），无需改。

**无环性核对**（改前必须自查一次）：`mcpp.home` 只 import `std` + `mcpp.platform`。
`mcpp.config` 的闭包内无 `mcpp.toolchain.*`，故 `mcpp.toolchain.stdmod → mcpp.home` 不成环。

**测试**（`tests/unit/test_config.cpp` 扩写，或新建 `tests/unit/test_home.cpp`）：

| 用例 | 断言 |
|---|---|
| `MCPP_HOME` 优先 | 设环境变量 → `root()` == 该值；`bmi_root()` == `<it>/bmi` |
| Windows 无 HOME | `is_windows` 下 `USERPROFILE` 生效（用 `if constexpr` 分支或跳过非 Windows） |
| 兜底形态 | 两个变量都不设时，路径以 `.mcpp` 结尾、**不再**以 `.mcpp-bmi` 结尾 |
| 与 config 一致 | `load()` 后 `cfg.bmiCacheDir == mcpp::home::bmi_root()`（同一进程同一环境下必须相等） |

最后一条是这次的核心不变量，**必须机器校验**——它就是 D2 的回归闸。

---

## P2 — `mcpp stage` 子命令

**新建** `src/build/stage.cppm`（`export module mcpp.build.stage;`），承载纯逻辑：

```cpp
struct StageResult { bool copied; };           // copied=false ⇒ 判等跳过
struct StageError  { std::string message; };   // 已含 hint 文案
std::expected<StageResult, StageError> stage_file(
    const std::filesystem::path& src,
    const std::filesystem::path& dst,
    bool verify_hash);
```

实现按设计 §S1 语义表：

1. `src` 不存在 → error。
2. `create_directories(dst.parent_path())`。
3. `dst` 存在 && `file_size` 相等 && (verify == Size || 逐字节相等)
   → **不写字节、不碰时间戳** → `StageOutcome{.copied = false}`。
   （对齐 mtime 会让 `restat` 失效并重新引发级联——实测过，见设计 §S1 的表）
4. 否则：`copy_file(src, dst.tmp.<pid>)` → `rename(tmp, dst)`。
5. rename 失败 → `copy_file(src, dst, overwrite_existing)`。
6. 4/5 均失败 → 睡 100 / 300 / 900 ms 重试整个 4-5 序列，共 3 轮。
7. 仍失败 → `StageError`，文案照设计 §S4 的模板（file / from / os error / hint 四段，
   hint 必须点名 clangd 与 `compile_commands.json` 的因果）。
8. 每次退出前清理残留 `dst.tmp.<pid>`。

`Verify::Content` 是**默认**；`--verify size` / `MCPP_STAGE_VERIFY=size` 才退回只比 size。实现是分块
逐字节比较（同 I/O 成本、无碰撞面、可提前退出），不引入 hash 依赖。

**CLI 接线**（照 `dyndep` 的形状）：

| 文件 | 改动 |
|---|---|
| `src/cli/cmd_build.cppm` | 新增 `export int cmd_stage(const ParsedArgs&)`，就近放在 `cmd_dyndep`（:187）旁 |
| `src/cli.cppm` | :496 附近新增 `.subcommand(cl::App("stage") ...)`，描述以 `(internal: invoked by ninja)` 开头，选项 `--output/-o`、`--verify`；`.action(wrap_rc(cmd_stage))` |
| `src/cli.cppm` | :547-551 的 `known` 白名单加 `"stage"`，**数组长度 22 → 23**（写死的模板实参，漏改即编译失败/静默拒命令） |

**测试**（新建 `tests/unit/test_build_stage.cpp`）：

| 用例 | 断言 |
|---|---|
| 目标不存在 | 复制发生，`copied == true`，内容一致 |
| 目标已存在且等长等内容 | `copied == false`，且**目标 mtime 一点没变**（不是"未变成 now"，是完全不变） |
| 等长但内容不同 + `Verify::Content` | `copied == true` |
| 等长但内容不同 + `Verify::Size` | `copied == false`（这是**有意的** fp 判据取舍，注释写清） |
| `src` 缺失 | error，message 含 src 路径 |
| 目标目录不存在 | 自动创建 |
| 只读目标（POSIX `chmod 444`） | 走 rename 分支成功；断言最终内容正确 |
| 错误文案 | 人为构造失败（目标是个目录）→ message 含 `clangd` 与 `hint:` |

---

## P3 — ninja 后端切到新 rule

**文件**：`src/build/ninja_backend.cppm`

1. **`mcpp` 变量提取**：把 :404 的
   `append(std::format("mcpp = {}\n", escape_ninja_path(mcpp_exe_path())))`
   移出 `if (dyndep)`（:403），改为无条件绑定；`scan_deps` 仍留在 `if (dyndep)` 内。
2. **rule 重写**（:411-419），跨平台单一形态、不再分叉 PowerShell/`cp`：

```
rule stage_file
  command = $mcpp stage --output $out $in
  description = STAGE $out
  restat = 1
```

3. **rule 更名**：`cp_bmi` → `stage_file`，四处 staging edge（:793/:795/:807/:810）与
   DLL 部署（:1080）同步改名。
4. `command_prefixes()`（:278-291）追加 `mcpp_exe_path()`。
5. `filter_ninja_output()`（:323-345）：`FAILED:` 不再整行丢弃，改为归一成
   `failed: <target>` 保留。

**文件**：`src/build/execute.cppm`

6. `read_ninja_command_prefixes()`（:181-205）白名单 key 加 `"mcpp"`。

**测试**（`tests/unit/test_ninja_backend.cpp`）：

| 用例 | 断言 |
|---|---|
| rule 文本 | 含 `rule stage_file`、`$mcpp stage --output $out $in`、`restat = 1`；**不含** `Copy-Item`、`cp -f` |
| `mcpp` 绑定 | dyndep 开/关两种 plan 下都出现 `mcpp      = ` 行 |
| staging edge | 四条 edge 的 rule 名是 `stage_file`；`std.compat` 仍有 `| pcm.cache/std.pcm` order-only 前置 |
| DLL 部署 | `runtimeDeployFiles` 非空时用同一 rule 名 |
| 过滤器 | `filter_ninja_output` 对含 `<mcpp路径> stage ...` 的回显行过滤掉、对 `failed:` 与 `hint:` 正文保留 |

---

## P4 — 兜底路径的可见性

| 文件 | 改动 |
|---|---|
| `src/scaffold/create.cppm` | :284-287 的 `.gitignore` 模板：`target/` + `.mcpp/` |
| `src/doctor.cppm` | :155/:192 附近：若 cwd 或工程根存在 `.mcpp-bmi/`，输出一行 `legacy BMI cache at <path> — safe to delete`（**不自动删**） |

`.gitignore` 模板变更需同步 e2e 中断言过 scaffold 产物的用例（`grep -rn "gitignore" tests/e2e`
先扫一遍）。

---

## P5 — e2e

**新建 `tests/e2e/170_bmi_staging_no_cascade.sh`**（全平台跑，锁 D3 + S1 步 2）：

1. `mcpp new` 一个 bin 工程 → `mcpp build`（产出 staged BMI）；
2. 从 `mcpp build -v` 的 STAGE 行或 `build.ninja` 解析出 staging edge 的 `$in`
   （**不要**用 `awk '{print $NF}'` 直接切 `rule` 行——本次调查里就踩过，取到的是 `cp_bmi`
   这个字面量；正确做法是匹配 `^build .*: (stage_file|cp_bmi) ` 的行再取最后一个字段）；
3. `touch "$in"` 让 edge 变脏；
4. `mcpp build -v` 断言：
   - 退出 0；
   - 输出**不含** `src/main.cpp` 的编译行（反级联，今天会失败）；
   - staged BMI 的内容与 `$in` 一致。

**新建 `tests/e2e/171_bmi_staging_locked_dest.sh`**：

- **Windows 分支**（`case "$(uname -s)" in *NT*|MINGW*|MSYS*)`）：用 PowerShell 在子进程里
  映射住 staged BMI，**不依赖 clangd**：

  ```powershell
  $f = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile(
        $path, [System.IO.FileMode]::Open)
  Start-Sleep -Seconds 30     # 持有期覆盖被测构建
  ```

  然后 `touch` 缓存侧 BMI（保持内容不变）→ `mcpp build` 必须**成功**（走判等跳过）。
  这就是 #311 的最小复现，且不需要装 clangd。
- **POSIX 分支**：`chmod 444` staged BMI + 让缓存侧内容真的不同（改用另一个 fingerprint 的
  BMI 或人为构造一份等长-不同内容的假文件）→ 断言走 rename 分支成功。
- 负例（两个平台）：把 staged BMI 换成一个**目录**同名占位 → 断言构建失败且 stderr 含
  `hint:` 与 `clangd`。

`tests/e2e/run_all.sh` 若是显式清单则登记两个新脚本；编号接 169(上游 169 已被 semver 用例占用)。

---

## P6 — 收尾

1. `CHANGELOG.md`：
   - fix(#311)：Windows 上被 clangd 映射的 std BMI 不再让构建失败；
   - **behavior change**：BMI 缓存根统一为 `$MCPP_HOME/bmi`（Windows 从 `<cwd>\.mcpp-bmi`、
     self-contained 安装从 `~/.mcpp/bmi` 迁走）；首次构建会重编一次 std（10–60 s），
     遗留目录可手动删除。
2. 版本号：`mcpp.toml` → `2026.7.30.1`（注意 `git status` 里 `mcpp.toml` 已有本地改动，
   提交前先核对那处改动是否该一起进）。
3. 发布闭环按既有 runbook 走（release → 镜像 xlings-res 双端 → xim-pkgindex → 真装验证 →
   bootstrap pin）。**bootstrap pin 与本次发布版本是两组，不要一起 bump**。
4. 不要在 issue #311 下评论（按本次任务要求）；发布后再回复。

---

## 自查清单（提交前逐条打勾）

- [ ] `grep -rn "\.mcpp-bmi" src/` 只剩 doctor 的遗留提示与注释，无路径构造
- [ ] `grep -rn "getenv(\"HOME\")" src/` 不再出现在 BMI/home 解析路径上
- [ ] `grep -rn "Copy-Item" src/` 归零
- [ ] `grep -rn "cp_bmi" src/` 归零
- [ ] `cli.cppm` 的 `known` 数组长度与元素数一致（22 → 23）
- [ ] `if (dyndep)` 之外能拿到 `$mcpp`（用 GCC 非 dyndep plan 生成一次 build.ninja 目视核对）
- [ ] 单测全绿 + `tests/e2e/170`、`171` 全绿（Linux 本机 + Windows CI）
- [ ] Windows CI 上确认 STAGE 行不再 spawn PowerShell（顺带的启动开销收益）
