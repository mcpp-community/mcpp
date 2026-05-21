# Linux sysroot 缺少内核头文件导致 std module 预编译失败

## 现象

在用户机器上（非 CI），使用 LLVM 或 GCC 工具链执行 `mcpp run` / `mcpp build` 时，
std module 预编译失败：

```
/home/speak/.mcpp/registry/subos/default/usr/include/bits/local_lim.h:38:10:
fatal error: 'linux/limits.h' file not found
```

GCC 和 Clang 均受影响，问题是系统性的。

## 根因

### 直接原因：M5.5 sysroot 覆盖逻辑

`cli.cppm:1192-1203` 中的 M5.5 逻辑，将 `tc->sysroot` 强制覆盖为 mcpp 自己的
subos（`~/.mcpp/registry/subos/default`）：

```cpp
if (!isMuslTc) {
    if (auto cfg = get_cfg(); cfg) {
        auto mcppSubos = (*cfg)->xlingsHome() / "subos" / "default";
        if (std::filesystem::exists(mcppSubos / "usr" / "include")) {
            tc->sysroot = mcppSubos;
        }
    }
}
```

### 触发 commit

**`063fb6f`** — 将 MCPP_HOME 从 xpkgs 包目录改为 `~/.mcpp/`，使 M5.5
找到一个存在但不完整的 subos。

### CI/e2e 未发现的原因

CI 的 subos 由 `xlings self install` 完整初始化（含内核头文件），e2e 测试
通过 `_inherit_toolchain.sh` 继承宿主完整 subos。

---

## 设计分析：当前问题的本质

### 当前架构的矛盾

mcpp 的工具链管理存在一个架构层面的矛盾：

```
                  ┌─────────────────────────┐
                  │   xlings 下载 payload    │
                  │  ~/.xlings/data/xpkgs/  │
                  │  (cfg/specs 路径正确)     │
                  └──────────┬──────────────┘
                             │ copy (因 XLINGS_HOME 传播不可靠)
                             ▼
                  ┌─────────────────────────┐
                  │  mcpp sandbox 副本       │
                  │ ~/.mcpp/registry/xpkgs/ │
                  │  (cfg/specs 路径 stale!) │
                  └──────────┬──────────────┘
                             │
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
         GCC: specs     Clang macOS:    Clang Linux:
         fixup 修复      --no-default    没有修复 ← BUG
         路径 ✅         -config ✅      用 subos 补救 ✗
```

**三个平台/工具链的 stale path 问题，用了三种不同的解法：**

| 工具链 | stale path 来源 | 当前解法 | 状态 |
|--------|-----------------|---------|------|
| GCC (Linux) | specs 文件 | `fixup_gcc_specs()` 重写 specs | ✅ 正确但只修了 specs，没修 `-print-sysroot` |
| Clang (macOS) | clang++.cfg | `--no-default-config` + xcrun | ✅ 但只在 macOS |
| Clang (Linux) | clang++.cfg | M5.5 subos 覆盖 | ❌ subos 不完整 |

**根本问题：mcpp 复制了 payload 但没有统一处理 stale path。**

### mcpp 对 xlings 的依赖边界不清

当前 mcpp 对 xlings 有三层依赖：

| 层次 | 内容 | 应该依赖? |
|------|------|-----------|
| 包下载 | xlings 作为包索引 + 下载工具 | ✅ 是 |
| payload 路径 | xpkgs 下具体包的文件结构 | ✅ 是 |
| subos | xlings 的沙箱 sysroot | ❌ 不应该 |

M5.5 的问题就在于跨越了第三层边界——用 xlings 的内部实现细节（subos）
来补救 mcpp 自身的路径问题。

---

## 设计方案

### 核心原则

**mcpp 只把 xlings 当包索引 + 下载工具。工具链的编译环境由 payload 自描述，
mcpp 忠实读取，不替换、不覆盖。**

### 方案：Payload-first + 统一 stale path 处理

#### 1. 工具链 sysroot 来源：只从 payload 获取

```
┌─────────────────────────────────────────────────┐
│             sysroot 解析优先级                    │
│                                                  │
│  1. compiler -print-sysroot (GCC 原生支持)        │
│     → 路径存在则使用                              │
│                                                  │
│  2. payload cfg 文件解析 (Clang clang++.cfg)      │
│     → 解析 --sysroot= 行，路径存在则使用          │
│                                                  │
│  3. macOS: xcrun --show-sdk-path                  │
│                                                  │
│  4. 空 (不传 --sysroot，让编译器用自身默认值)      │
│                                                  │
│  ✗ 不再有 subos fallback                          │
└─────────────────────────────────────────────────┘
```

**改动**：
- 删除 `cli.cppm` M5.5 subos 覆盖代码
- `probe.cppm` 增加 cfg 文件解析作为第 2 优先级

#### 2. 复制 payload 时统一修复 stale path

当前只有 GCC 做了 specs fixup，Clang 只在 macOS 做了 `--no-default-config`。
应该统一为：**凡是复制了 payload，就修复其中的路径配置。**

```
                  copy payload 后
                       │
              ┌────────┼────────┐
              ▼        ▼        ▼
          GCC specs  Clang cfg  其他配置
              │        │
              ▼        ▼
        rewrite_gcc  rewrite_cfg
        _specs()     _paths()
              │        │
              ▼        ▼
         新 sysroot  新 sysroot
         新 rpath    新 -isystem
                     新 -L/-rpath
```

**具体做法**：在 `package_fetcher.cppm` 复制 payload 后（或在 `cli.cppm`
toolchain install 后），对 Clang cfg 做类似 `fixup_gcc_specs` 的路径重写：

```cpp
void fixup_clang_cfg(const std::filesystem::path& payloadRoot,
                     const std::filesystem::path& newSysroot) {
    auto cfgPath = payloadRoot / "bin" / "clang++.cfg";
    if (!std::filesystem::exists(cfgPath)) return;

    // 读取 cfg，将旧 sysroot/isystem/rpath 路径替换为 payload 实际位置
    // ...
}
```

**但这里有一个关键设计选择：新路径指向哪里？**

#### 3. 关于 sysroot 本身从哪来

工具链需要 C 库头文件（glibc headers + linux kernel headers）。来源有三种：

| 来源 | 说明 | 优劣 |
|------|------|------|
| 系统 `/usr/include` | 宿主机自带 | 简单，但不可控，不同发行版不同 |
| xlings subos | xlings 管理的沙箱 sysroot | 可控，但 mcpp 需依赖 xlings 内部结构 |
| payload 自带 | 工具链包自带 sysroot（如 musl-gcc） | 最干净，但需要上游包支持 |

**推荐策略**：

- **短期**：信任 payload 自身配置的 sysroot 路径。xlings 安装 GCC/LLVM 时
  已经配置好了 sysroot（指向 xlings 自己的 subos），mcpp 只需忠实读取。
  如果路径存在且有效，就用它。如果路径无效，不传 `--sysroot`，让编译器
  用系统默认路径。

- **中期**：推动 xlings 上游让 LLVM/GCC 包的 cfg/specs 使用相对路径或
  可配置路径，避免硬编码绝对路径。这从源头消除 stale path 问题。

- **长期**：mcpp 自带轻量 sysroot 管理（类似 Zig 的做法：打包 libc headers
  作为 mcpp 自身的资源），彻底不依赖宿主系统或 xlings 的 sysroot。但这是
  大工程，不急。

---

## 修复方案（基于以上设计）

### Phase 1：修复当前 bug（最小改动）

#### P1-1：删除 M5.5 subos 覆盖

**文件**：`src/cli.cppm:1192-1203`

**删除**整个代码块。工具链的 sysroot 由 payload 决定，mcpp 不介入。

同时删除 `cli.cppm:1001` 的 subos 注释和 `cli.cppm:1178` 的 "glibc subos" 注释。

#### P1-2：`probe_sysroot` 增加 cfg 解析

**文件**：`src/toolchain/probe.cppm:254-272`

`-print-sysroot` 失败后（Clang 不支持），解析 payload 中 `clang++.cfg`
的 `--sysroot=` 行：

```cpp
std::filesystem::path
probe_sysroot(const std::filesystem::path& compilerBin,
              const std::string& envPrefix) {
    // 1. -print-sysroot (GCC)
    auto r = run_capture(std::format("{}{} -print-sysroot {}",
                                     envPrefix,
                                     mcpp::xlings::shq(compilerBin.string()),
                                     mcpp::platform::null_redirect));
    if (r) {
        auto s = trim_line(*r);
        if (!s.empty() && std::filesystem::exists(s)) return s;
    }

    // 2. Parse payload cfg (Clang)
    auto cfgPath = compilerBin.parent_path()
                   / (compilerBin.stem().string() + ".cfg");
    if (std::filesystem::exists(cfgPath)) {
        std::ifstream ifs(cfgPath);
        std::string line;
        while (std::getline(ifs, line)) {
            constexpr std::string_view prefix = "--sysroot=";
            if (line.starts_with(prefix)) {
                auto val = trim_line(std::string(line.substr(prefix.size())));
                if (!val.empty() && std::filesystem::exists(val))
                    return val;
            }
        }
    }

    // 3. macOS: xcrun SDK
    if (auto sdk = mcpp::platform::macos::sdk_path())
        return *sdk;
    return {};
}
```

**Phase 1 效果**：
- Clang：cfg 中的 `--sysroot=~/.xlings/subos/default` 被正确读取，
  `tc->sysroot` 不再为空。stdmod.cppm 和 flags.cppm 传递正确的 sysroot。
- GCC：`-print-sysroot` 正常工作（如果路径存在）；若不存在则 sysroot 为空，
  GCC 用默认系统路径（`/usr/include`）。
- 不再依赖 subos。

### Phase 2：统一 Clang stale cfg 处理（消除隐患）

#### P2-1：`stdmod.cppm` — 所有有 cfg 的 Clang 都走 `--no-default-config`

**文件**：`src/toolchain/stdmod.cppm:103-116`

将 macOS 特有的 `--no-default-config` 逻辑泛化为"有 cfg 文件的 Clang"：

```cpp
std::string sysroot_flag;
if (is_clang(tc)) {
    auto cfgPath = tc.binaryPath.parent_path()
                   / (tc.binaryPath.stem().string() + ".cfg");
    if (std::filesystem::exists(cfgPath)) {
        // Bypass cfg (may have stale paths after payload copy).
        // Provide correct flags from payload structure directly.
        auto llvmRoot = tc.binaryPath.parent_path().parent_path();
        auto libcxxInclude = llvmRoot / "include" / "c++" / "v1";
        sysroot_flag = " --no-default-config";
        sysroot_flag += std::format(" -isystem'{}'", libcxxInclude.string());
        if (!tc.sysroot.empty())
            sysroot_flag += std::format(" --sysroot='{}'", tc.sysroot.string());
        else if (auto sdk = mcpp::platform::macos::sdk_path())
            sysroot_flag += std::format(" --sysroot='{}'", sdk->string());
    } else if (!tc.sysroot.empty()) {
        sysroot_flag = std::format(" --sysroot='{}'", tc.sysroot.string());
    }
} else if (!tc.sysroot.empty()) {
    sysroot_flag = std::format(" --sysroot='{}'", tc.sysroot.string());
}
```

#### P2-2：`flags.cppm` — 同步修改

**文件**：`src/build/flags.cppm:96-111`

同步 P2-1 的逻辑：将 `is_macos_clang` 条件改为"检测到 cfg 文件存在"。

**Phase 2 效果**：
- Linux 和 macOS Clang 走统一路径
- 不再依赖 cfg 中的路径碰巧有效
- mcpp 从 payload 结构推导出正确的 `-isystem` 和 `--sysroot`

### Phase 3（未来）：复制 payload 时重写 cfg 路径

在 `package_fetcher.cppm` 或 `cli.cppm` toolchain install 后，添加
`fixup_clang_cfg()`，类似 `fixup_gcc_specs()` 的做法：

```cpp
void fixup_clang_cfg(const std::filesystem::path& payloadRoot,
                     const std::filesystem::path& oldXlingsHome,
                     const std::filesystem::path& newRegistryHome) {
    // 重写 clang++.cfg 中的路径：
    //   --sysroot=<old> → --sysroot=<new>
    //   -isystem <old>  → -isystem <new>
    //   -L<old>         → -L<new>
    //   -rpath,<old>    → -rpath,<new>
}
```

这样即使不用 `--no-default-config`，cfg 路径也是正确的。
但需要 mcpp 管理自己的 sysroot 内容（确保完整性），所以这是更远期的方向。

---

## 修改总结

| Phase | 修改 | 文件 | 效果 |
|-------|------|------|------|
| P1-1 | 删除 M5.5 | cli.cppm | 去除 subos 依赖 |
| P1-2 | cfg 解析 sysroot | probe.cppm | Clang 获取正确 sysroot |
| P2-1 | 统一 --no-default-config | stdmod.cppm | 消除 stale cfg 隐患 |
| P2-2 | 同步 P2-1 | flags.cppm | 常规编译也用正确路径 |
| P3 | cfg 路径重写 | package_fetcher/cli | 从根源修复 stale path |

**Phase 1 修复 bug，Phase 2 消除隐患，Phase 3 完善架构。**

---

## 测试补充

### 新增 e2e 测试：无 subos 下的 import std

```bash
#!/usr/bin/env bash
# requires: import-std
# Test that import std works without mcpp's subos sysroot.
# Regression test: M5.5 subos override must not be required.
set -euo pipefail

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
export MCPP_INHERIT_SUBOS=0
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"

cat > mcpp.toml <<'EOF'
[package]
name = "sysroot_test"
version = "0.1.0"
EOF

cat > src/main.cpp <<'EOF'
import std;
int main() { std::println("sysroot ok"); }
EOF

"$MCPP" build
"$MCPP" run | grep -q "sysroot ok"
```
