---
subject: xlings
status: active
---

# mcpp 对 xlings 的依赖：Provider 端口、归属与位置、上游复用、系统级安装、payload 不可变、文件系统能力与长路径（#809）

- 日期：2026-10-16。状态：修订 3，待 review。
- 生态联动：xlings 侧的设计 `openxlings/xlings:.agents/docs/2026-10-16-luban-inside-experience-and-ecosystem-design.md`
  （下称 **xlings 设计**）：终端中继、Luban 系统内的路径与 index、系统自检与测试矩阵、aarch64、#652、#672，以及本文的 X1–X7。

**修订 3 的变化：**
- Luban 系统**内部**的 mcpp（core、agent-workspace 带 mcpp 与 gcc 16.1.0）：edition 带 `/etc/mcpp/config.toml`，
  `upstream = ["/xlings"]`（系统自己的 home），默认工具链为系统已带的 gcc，系统里第一次构建不再下载工具链（§9.5）。
  由 xlings 设计的 L7 检查与本文 §15 的 e2e 共同验证。
- 依赖 xlings#652 的修复：安装失败时 mcpp 收到原因（今天为空的 `E_INTERNAL`）。
- 发布顺序与 xlings 设计 §16 对齐：xim-pkgindex → xlings → mcpp → core 的下一版本带新 mcpp。
- 基线：`main` `dd3ab3aa`（mcpp 2026.10.11.1 之后；内置 xlings 2026.10.10.2）。
- 目标版本：mcpp **2026.10.16.1**。**一个 PR 完成**，不分阶段，不留临时方案。
- 前置：一次 xlings 发布，内容为 §14 的 X1–X7（通用基础设施，不含 mcpp 专属概念）。mcpp 的 PR 把内置 xlings 钉到这次发布。
- 证据：本地 Linux x86_64；openxlings/xlings `cf348cd`（2026.10.15.4）源码；mcpp#809 正文；
  probe PR #810 两轮运行（[38780724019](https://github.com/mcpp-community/mcpp/actions/runs/38780724019)、
  [38781807313](https://github.com/mcpp-community/mcpp/actions/runs/38781807313)），见 §2。

**修订 2 的变化：**
- O1–O3 由 probe 实测关闭（§2）。结论：xlings 2026.10.15.4 安装的 gcc、binutils、llvm 不经 mcpp 修改即可运行；
  借来的工具链（链接或 junction）构建的产物，加载器与 rpath 是 mcpp 钉的运行库，上游一个字节都没被写。
  原 X6（"工具链装完无需外部修改即可运行"）已由 2026.10.15.4 满足，从 xlings 需求中移除。
- 新发现 F3：mcpp 自己往 payload 目录写"已安装"标记（`.mcpp_ok`、`.mcpp-fixup.json`）。没有它，链接条目被当作未安装、
  重装覆盖；而只读上游写不进去。完整性改由 xlings 回答（§7.1、X4）。
- 新发现 F4：`fallback/xpkg_copy` 在安装失败时从 `~/.xlings` 复制 payload，是重复安装的一个直接来源，删除。
- 新增 §9 系统级安装（O5）：多用户共用的 mcpp 系统 home，管理员写、用户只读、用户侧无需 root。
- 新增上游信任边界（§8.2）：上游必须属于当前用户或 root（Windows：当前用户、Administrators、SYSTEM）。
- 新增借用的"每个 home 的配置"要求：借来的 payload 在借用方 home 中照样配置 subos 条目与 `abi` 链接（X3）。
- 新增路径预算检查（§11）：Windows 未开长路径且 MCPP_HOME 过深时，在配置加载时报告；probe 实测 182 字符的 MCPP_HOME
  在开关关闭时 xlings 克隆 index 与建暂存文件失败，且报为"internal error"（X6 要求 xlings 如实报告）。
- O4（dedupe）、O6（项目 index 放项目作用域）、D1（`mcpp.provider`）已确认。

## 0. 结论

1. **一个端口。** `mcpp.provider` 用 mcpp 的语言定义包提供者：index、包引用、payload、运行库、固定工具、事件。
   `mcpp.xlings` 降为它唯一的适配器；xlings 的命令、协议与布局只在适配器中出现，lint 保证这条边界。
2. **归属决定位置。** mcpp 的 index（mcpplibs、全局与项目 `[indices]`，git、artifact、本地路径、私有）及其 payload 归 mcpp：
   `~/.mcpp/indices/<name>`、`~/.mcpp/xpkgs`，项目级在 `<project>/.mcpp/`。xim 的 index 与 payload 归 xlings，
   位于适配器私有的 xlings home `~/.mcpp/registry`。
3. **payload 不可变。** mcpp 不再写任何 payload：删除 `post_install` 的 patchelf 与 cfg 重写、`.mcpp_ok` 与
   `.mcpp-fixup.json` 标记、从 `~/.xlings` 复制 payload 的回退。安装是否完整由 xlings 回答。
4. **复用是 store 的一条安装策略。** `[xlings] upstream` 给出只读上游（默认空，即自包含）。身份一致的 payload
   以链接条目（Windows junction）发布，整闭包一起，上游永不被写，失效时原路径自愈。通用层看不到这一点。
5. **系统级安装。** 管理员以 `mcpp --system` 维护一个系统 home（Linux 默认 `/var/lib/mcpp`），用户各自的 `~/.mcpp`
   把它作为上游；用户侧所有操作无需 root。Luban OS 以系统级配置开启。
6. **xlings 的来源由握手裁决。** `bundled`、`system`、路径三种来源都检查版本下限、协议、能力与 home 格式，不满足退回内置。
7. **#809。** 内置 xlings 升级；mcpp 自己 `scaffold_fs` 的同类缺陷（probe 实测 `mcpp new` 在 NFSv3 上失败）以文件系统能力表修复。
8. **长路径。** mcpp.exe 与 xlings.exe 声明 `longPathAware`；新目标键 `windows_long_paths`；配置加载时做路径预算检查；
   doctor 报告系统开关。
9. **index 刷新改为暂存再交换。** 删除 `index_snapshot` 的备份回滚。

## 1. 问题与证据

### 1.1 #809 与 mcpp 自己的同类缺陷

| 编号 | 观察 | 原因 | 状态 |
|---|---|---|---|
| F1 | NFSv3 上 `mcpp toolchain install gcc@16.1.0`：`cannot publish …/usr/include/asm: Invalid argument`（probe 复现） | xlings 用 `renameat2(RENAME_NOREPLACE)` 发布，NFSv3 返回 `EINVAL` | xlings 2026.10.15.4 修复（openxlings/xlings#671）；probe 中 mcpp 2026.10.11.1 改用它后，NFS 上安装、构建、运行都通过 |
| F2 | NFSv3 上 `mcpp new demo`：`atomic no-replace rename '…/.mcpp-new-…' -> '…/demo' failed: Invalid argument`（probe 实测） | `modules/platform/src/scaffold_fs.cppm:72` 直接 `renameat2(RENAME_NOREPLACE)`，无回退 | 本 PR 修复（§10） |

### 1.2 两套重复安装

本机：`~/.xlings/data/xpkgs` 60G，`~/.mcpp/registry/data/xpkgs` 14G，其中 `<ns>-x-<name>/<ver>` 相同的 **10.1G**
（gcc 638M、android-ndk 2.3G、emsdk 1.5G、libllvm 124M、glibc 122M 等）。mcpp store 的命名空间：xim 85、compat 46、
freedesktop 8、mcpplibs 11 等。

### 1.3 布局知识散落在适配器之外

| 知识 | 位置 |
|---|---|
| store 路径与 `<ns>-x-<name>/<ver>` 命名 | `src/toolchain/lifecycle.cppm:108,455,484,1136`；`src/doctor.cppm:243,270,291,529,539,657,1648`；`src/runtime/binding.cppm:253,289`；`src/build/prepare/toolchain.cpp:298-303,420`；`src/build/prepare/toolchain_decision.cpp:137`；`src/pm/package_fetcher.cppm:883,1057,1306`；`src/toolchain/probe.cppm:195,414`；`src/toolchain/msvc.cppm:1174,1448`；`src/cli/cmd_self.cppm:71`；`src/toolchain/post_install.cppm:772`；`modules/platform/src/env.cppm:237` |
| 从编译器路径向上找 store，并 canonical 化 | `src/build/hermetic.cppm:127,138`；`xpkgs_from_compiler` 的调用方 `src/toolchain/probe.cppm:420`、`src/toolchain/msvc.cppm:1171,1365`、`src/build/execute.cppm:3525`、`src/fallback/probe_sysroot.cppm:56,84` |
| 读用户自己的 xlings home | `src/fallback/xpkg_copy.cppm:40-41`（F4）；`active_home_xpkgs`/`find_home_tool` 的调用方 `src/toolchain/probe.cppm:424,489`、`src/toolchain/post_install.cppm:343` |
| `.xlings.json` 与 subos 目录 | `src/config.cppm:198,236,869`；`src/build/runtime_validation.cppm:344`；`src/build/execute.cppm:1836`；`src/build/prepare/graph_load.cpp:1077` |
| index 内部结构 | `src/xlings/xlings.cppm:723`（读 `pkgs/<首字母>/<name>.lua`）；`src/config.cppm:198`（探测 `xim-index-repos/`） |

38 个文件 `import mcpp.xlings`。进程调用已经集中在 `src/xlings/xlings.cppm`（12 处 `build_command_prefix`）。
xlings ≥ 2026.9.27.1 的 `install_targets` 事件对每个请求返回 `payload_dir` 与 `revision`（`src/xlings/xlings.cppm:186-235`），
下限 2026.10.10.2 高于它，但大多数调用方仍走路径语法。

### 1.4 mcpp 写 payload

| 编号 | 写入 | 位置 |
|---|---|---|
| W1 | gcc、binutils 全部 ELF 的 PT_INTERP 与 RUNPATH | `gcc_post_install_fixup`，`src/toolchain/post_install.cppm:453-486`（理由见 `:68-72` 的 TODO：xim 的 `elfpatch.auto` 曾扫到 0 个文件） |
| W2 | llvm `lib/` 的 RUNPATH | `llvm_post_install_fixup`，`:493-518` |
| W3 | 重写 payload `bin/*.cfg` | `fixup_clang_cfg`，`:276-379`（只为人直接运行 `clang++`；mcpp 构建用 `--no-default-config`） |
| W4（F3） | `.mcpp-fixup.json` | `:648` |
| W5（F3） | `.mcpp_ok` | `src/pm/package_fetcher.cppm:1112-1211`、`src/config.cppm:945,957`、`src/build/prepare/graph_load.cpp:729,807`；判据 `src/fallback/install_integrity.cppm` |
| W6（F4） | 从 `~/.xlings` 复制整个 payload 进 registry | `src/fallback/xpkg_copy.cppm`，调用方 `src/pm/package_fetcher.cppm:1209` |

同一个 `xim:gcc@16.1.0`，本机 `~/.xlings`（xlings 安装）的 `bin/gcc` 绑定 `xim-x-glibc/2.39`，`~/.mcpp/registry`（mcpp 修改后）
绑定 `xim-x-glibc/2.44.3`：被修改的 payload 不可能被复用。#273 的路径限制是因为修改 payload 才需要的。

### 1.5 index 的位置不由 mcpp 决定

`src/pm/index_snapshot.cppm` 头注释：`index_repos` 条目"没有版本，也没有目的地"，`xlings update` 原地改写，mcpp 只能备份、失败再回滚。

### 1.6 现有 `[xlings]` 配置是两个没有检查的半模式

`src/config.cppm:377-381`、`:765-767`、`:879-895`：`binary = "system"` 不检查版本；`home = "<dir>"` 不检查归属，
指向 `~/.xlings` 时 mcpp 会改写用户 home 的 index、mirror 与运行库钉版本。

### 1.7 xlings 2026.10.10.2 → 2026.10.15.4 中与 mcpp 相关的变化

| 变化 | 来源 | 对 mcpp |
|---|---|---|
| 文件系统能力探测、`publish_no_replace` | #671 | 修复 F1 |
| 每个环境一个默认运行库：payload 经 `<home>/abi/<family>` 绑定 libc 与 C++ 运行库，`.xlings.json` `runtime` 钉版本 | #667 C | W1、W2 不再需要（§2.1） |
| 接口协议 1.8 → 1.9 | #659、#667 | 适配器握手的受支持范围 |
| home 格式 layout 2 | 2026.10.4 起 | 握手要检查（§12） |
| system layer 与 borrow | #667 | §8 的基础；现状只支持模式为 `multi` 的 `/xlings`，借的是登记而不是本地条目，来源删除即报错 |

### 1.8 长路径的现状

| 平台 | 现状 |
|---|---|
| Windows | 引擎经手的路径走 `extended_length`（`modules/platform/src/fs.cppm:57-75`）；对象路径已用 `__pkg`/`__ext/<hash>`（`src/build/plan.cppm:1972` 起）；mcpp.exe 只声明 `activeCodePage`；**mcpp 与 xlings 都没有 `longPathAware`**；xlings 的 `GetModuleFileNameW` 用 `MAX_PATH` 缓冲；工作目录在任何写法下受 MAX_PATH 限制 |
| Linux | socket 路径 108 字节（xlings 有检查）；5.1 以前内核 shebang 127 字节 |

## 2. probe 实测（#810）

所有脚本只打印不断言。xlings 2026.10.15.4 建"上游 home"；mcpp 2026.10.11.1（发布版）建 MCPP_HOME；
"借用"以把 mcpp registry 中的条目替换为指向上游 payload 的符号链接（Windows junction）模拟。

### 2.1 O1：xlings 2026.10.15.4 安装的工具链不经 mcpp 修改即可运行（关闭）

| 平台 | 观察 |
|---|---|
| Linux x86_64、aarch64 | gcc、g++、cc1plus、binutils `ld`、clang++ 的 PT_INTERP 都是 `<home>/abi/linux-<arch>-glibc/ld-linux-*.so.*`，RUNPATH 含 `<home>/abi/linux-<arch>-glibc`；`abi.json` 记录 `glibc@2.44.3`、`gcc-runtime@15.1.0`。llvm 的 `libc++.so.1`、`libunwind.so.1` 的 RUNPATH 同样经 `abi` |
| Linux | clang++ 直接编译运行 hello 成功；产物绑定 `xim-x-glibc/2.44.3`（来自 xlings 生成的 cfg）。g++ 不带 sysroot 直接编译失败（`features.h` 找不到）：头文件经 subos 的 sysroot 提供，与 mcpp 无关（mcpp 总是传 `--sysroot`） |
| macOS arm64 | clang++ 只依赖 `/usr/lib` 的系统库，rpath 为 `@loader_path/../lib`；cfg 只有 `--sysroot=<SDK>` 与 `-fuse-ld=lld`；直接编译运行成功 |

xlings 生成的 gcc specs 与 clang cfg 中，产物的加载器与 rpath 写的是具体 glibc payload 路径；mcpp 构建不用它们
（`write_clean_link_specs`、`--no-default-config`），不受影响。

### 2.2 O2：借来的工具链构建 mcpp 的产物（关闭）

上游 store 先 `chown -R root`、`chmod -R a-w`（同时作为 O5 的只读系统层）。第 1 轮不预置标记，第 2 轮在上游 payload 中预置
`.mcpp_ok`（模拟"端口回答已安装"）。

| 平台与工具链 | 第 1 轮（无标记） | 第 2 轮（有标记） |
|---|---|---|
| Linux x86_64 gcc（变体 A 只借 gcc，变体 B 再借 binutils） | mcpp 判为未安装，xlings 重装，把链接换成实体目录，再执行 patchelf | 链接保留；`skip gcc fixup: … resolves outside this home`；构建运行成功 |
| Linux x86_64、aarch64 llvm | 同上 | 链接保留；构建运行成功 |
| macOS llvm | 同上 | 链接保留；构建运行成功；`clang++ -print-resource-dir` 经链接解析到上游真实路径 |
| Windows llvm（junction） | 同上；上游 369 个文件、字节数不变（重装没有穿过 junction 删除上游） | junction 保留；构建运行成功；资源目录解析到上游真实路径；上游不变 |

所有借用构建的产物：PT_INTERP 为 `<MCPP_HOME>/registry/data/xpkgs/xim-x-glibc/2.44.3/lib64/ld-linux-*.so.*`，
RUNPATH 为本地 store 路径（mcpp 钉的运行库）；上游目录树摘要前后一致。aarch64 上 mcpp 的 `gcc@16.1.0` 解析为
`xim:musl-gcc`，与上游所装不是同一个包，gcc 变体不适用。

结论：
1. W1–W3 不再需要。工具作为工具运行在它所在 home 的运行库上，产物由 mcpp 每次构建给出的参数决定。
2. W4、W5 是借用的障碍：完整性必须由 xlings 回答（X4），mcpp 不写标记。
3. 链接条目被替换时 xlings 只删除链接本身（四个平台都如此）；X3 把"删除条目永不穿过链接"写成要求，并由测试保证。

### 2.3 O3：Windows（关闭）

| 项 | 观察 |
|---|---|
| junction | 见 §2.2；`msvc@system` 与借来的 llvm 组合正常 |
| runner 的 `LongPathsEnabled` | 1 |
| 开关为 1，MCPP_HOME 157 字符 | 安装、构建、运行成功；最长路径 267 |
| 开关为 0，MCPP_HOME 182 字符 | `toolchain install llvm` 失败：xlings 克隆 index（`all index repo clone URLs failed`）与 `Failed to create staging file`，报为 "internal error … likely a bug" |

结论：深路径在开关关闭时首先在 xlings 失败；mcpp 需要在配置加载时做路径预算检查（§11），xlings 需要如实报告路径过长（X6）。

### 2.4 F1、F2（NFSv3）

见 §1.1。`renameat2(RENAME_NOREPLACE)` 在该挂载上返回 `EINVAL`。

## 3. 原则

| 编号 | 原则 |
|---|---|
| P1 | **依赖方向。** Luban OS → mcpp → xlings。xlings 只提供通用能力；是否复用、借哪些、放在哪里，是 mcpp 的配置与策略。 |
| P2 | **依赖能力，不依赖布局。** 通用层不拼 xlings 的路径、不读 xlings 的文件；payload 在哪里、是否完整由端口回答。 |
| P3 | **归属决定位置。** mcpp 的 index 与 payload 由 mcpp 决定放在 `~/.mcpp` 何处；xim 的东西属于 xlings。 |
| P4 | **payload 不可变。** payload 能否运行是它的主人（xlings 与 xim-pkgindex）的责任，发布前完成；mcpp 不写 payload，对工具链的定制只以每次构建的参数和 mcpp 自己目录中的生成文件表达。 |
| P5 | **store 路径不 canonical 化。** 本地 store 中的路径是 payload 的稳定身份，链接背后是谁与使用者无关。 |
| P6 | **上游只读。** mcpp 驱动的 xlings 只写 mcpp 的 home，从不写上游；删除条目永不穿过链接。 |
| P7 | **用户侧无需 root。** 任何用户操作都不需要提升权限；系统 home 只由管理员显式写入。 |

## 4. Provider 端口

### 4.1 分层

```
mcpp 通用层：build / toolchain / pm / runtime / doctor / pack / cli
      │  import mcpp.provider            （只用 mcpp 的概念）
      ▼
mcpp.provider        端口：类型与接口
mcpp.provider.fake   假实现：内存 index、临时目录 store（单元测试）
mcpp.provider.xlings 适配器（今天的 src/xlings/ 迁入 src/provider/xlings/）：命令、NDJSON、握手、布局
      │
xlings 运行时：bundled / system / 路径，经握手选出（§12）
```

### 4.2 接口

```cpp
export module mcpp.provider;
export namespace mcpp::provider {

struct ScopeRef { enum class Kind { Global, Project } kind; std::filesystem::path root; };

struct IndexDecl {
    std::string name;                     // 命名空间
    IndexSource source;                   // git url | artifact bases | 本地路径
    std::filesystem::path dir;            // index 数据目录（本地路径 index 即其本身）
    std::filesystem::path store;          // 这个 index 的 payload 放在哪里
};

struct PackageRef { std::string ns, name, versionReq; };

struct Payload {
    PackageRef resolved; std::string version; int revision;
    std::filesystem::path dir;            // 本地 store 中的路径，不 canonical 化（P5）
    enum class Origin { Local, Borrowed } origin;
};

struct Runtime { std::string family, id; std::filesystem::path loader, libDir, sysroot; };

struct ProviderInfo { std::string version, protocol, source; std::vector<std::string> capabilities; int homeLayout; };

class Provider {
public:
    ProviderInfo info() const;
    std::expected<IndexRevision, ProviderError> sync_index(const IndexDecl&, const std::filesystem::path& staging);
    std::expected<Plan, ProviderError> plan(ScopeRef, std::span<const PackageRef>);
    std::expected<std::vector<Payload>, ProviderError> install(ScopeRef, const Plan&, EventSink&);
    std::optional<Payload> find(ScopeRef, const PackageRef&) const;   // 只查询；只返回完整的 payload
    std::expected<void, ProviderError> remove(ScopeRef, const PackageRef&);
    std::expected<Runtime, ProviderError> runtime(ScopeRef) const;
    std::expected<std::filesystem::path, ProviderError> tool(PinnedTool);  // ninja / patchelf / nasm
};

Provider& provider();                     // 进程内一个，由配置与握手构造
}
```

- "编译器旁边的 binutils"、"linux-headers 在哪"改为 `find(scope, xim:binutils@…)`；`xpkgs_from_compiler`、
  `find_sibling_*`、`active_home_xpkgs`、路径语法回退全部删除。
- 删除 payload 只经 `remove`（适配器交给 xlings），mcpp 不对 store 条目调用 `remove_all`。
- 事件类型是端口的类型；NDJSON 解析在适配器内。

### 4.3 边界由 lint 保证

`.github/tools/check_provider_boundary.py`（接入 CI）：`src/provider/xlings/**` 之外，
不得 `import mcpp.xlings*`；不得出现 `"xpkgs"`、`"-x-"`、`xim-x-`、`".xlings.json"`、`"subos"`、`"xim-index-repos"`、
`.mcpp_ok`；不得对 `Payload::dir` 调用 `canonical`/`weakly_canonical`/`remove_all`。

### 4.4 契约测试

`tests/provider/`：适配器对内置钉版本与最新发布 xlings 各跑一遍（CI 已用最新发布版构建，接在同一处）。

## 5. 归属与位置

### 5.1 布局

```
~/.mcpp/
  config.toml
  indices/<name>/         mcpp 的 index 数据：mcpplibs、全局 [indices] 的 git/artifact index
  xpkgs/                  mcpp 的 store：mcpp index 所服务命名空间的 payload
  registry/               适配器私有：mcpp 的 xlings home（xim index、subos、abi、xim store）
    data/xpkgs/           xim 的 payload；复用时其中可有链接条目
  build-cache/ cache/ provisioned/ …   不变

<project>/.mcpp/
  indices/<name>/         项目 [indices] 的 git index
  xpkgs/                  项目 index 所服务命名空间的 payload（项目作用域，D4）
```

- 本地路径 index 原地使用。私有 index 与公开 index 没有区别，只是来源不同；凭据由 git 与 xlings 的传输层按现有方式处理。
- 一个 index 服务哪些命名空间由 index 自己声明（X2）。

### 5.2 配置

```toml
[indices.myteam]
url   = "https://git.example.com/team/index.git"
# dir / store 省略时取 ~/.mcpp/indices/myteam 与 ~/.mcpp/xpkgs
```

`mcpplibs` 是内置默认，可不写。项目 `mcpp.toml` 的 `[indices]` 不接受 `dir`、`store`。

### 5.3 存量 home

| 存量 | 处理 |
|---|---|
| `registry/data/mcpplibs` 等 mcpp index 数据 | 首次运行移动到 `~/.mcpp/indices/<name>`（同文件系统 rename，跨文件系统复制后删除） |
| `registry/data/xpkgs` 下 mcpp 命名空间的 payload | 不移动（已有产物与 compat 共享库的 RUNPATH 指向它们）。适配器 `find` 先查新 store 再查旧位置，`COMPAT(internal:legacy-registry-store)`；新安装进新 store；`mcpp cache clean --legacy` 列出并可删除新 store 已有的旧条目 |
| 已被 mcpp 修改过的 xim payload | 保持原状，仍可运行，不再被修改 |
| payload 中的 `.mcpp_ok`、`.mcpp-fixup.json` | 不再读也不再写；`mcpp cache clean --legacy` 可清除 |

## 6. index 刷新：暂存再交换

1. `sync_index(decl, staging)`：xlings 把 index 拉到 `<dir>.staging-<pid>`（X1）。
2. mcpp 在暂存目录上检查 `index.toml` 契约（`min_mcpp`）。
3. 通过则用 `exchange_paths`/`rename_no_replace`（§10）原子交换到 `dir`；不通过则删除暂存目录，旧 index 保持可用。

`index_snapshot.cppm` 的备份与回滚删除；离线优先策略（`index_refresh.cppm`）不变。

## 7. payload 不可变

### 7.1 W1–W6 的去向

| 写入 | 处理 |
|---|---|
| W1、W2 | 删除。工具链经 `<registry>/abi/<family>` 绑定运行库（§2.1） |
| W3 | 删除。llvm.lua 的安装 hook 生成 cfg；mcpp 构建用 `--no-default-config` |
| W4 | 删除，随 `ensure_post_install_fixup` 与 `kFixupRev` 一起 |
| W5 | 删除写入。完整性由端口 `find`/`install` 的结果回答（X4：xlings 报告 payload 完整与否，含 #533 那类"报告成功但未安装"的检测）。mcpp 保留只读校验 `payload_is_substantive`，不满足时报错而不是写标记 |
| W6 | 删除 `fallback/xpkg_copy`。从 `~/.xlings` 获益的正确方式是上游（§8） |

随之删除：`patchelf_walk`、`gcc_post_install_fixup`、`llvm_post_install_fixup`、`fixup_clang_cfg`、`ensure_post_install_fixup`、
`escapes_containment`/`containment_root`（#273 的限制失去对象）、`InstallStash` 中针对 payload 目录的改名（由 xlings 的原子发布替代）。
`patchelf` 仍是固定工具：`mcpp pack` 与 `loader_contract` 只对 **mcpp 自己的产物** 使用它。

### 7.2 工具链运行在哪个运行库上

适配器在 bootstrap 时把 mcpp 选定的 `RuntimeBinding`（如 `glibc@2.44.3`）写为 registry home 的运行库钉版本
（xlings `config --runtime`，经接口调用）。自包含模式下工具链因此运行在 mcpp 选定的 glibc 上；借来的工具链运行在上游 home 的
运行库上（§2.2 证明这对产物没有影响）。

### 7.3 产物不变

产物的加载器、rpath、specs、sysroot 由 `linkmodel` 与 `write_clean_link_specs` 每次构建给出，引用本地 store 路径（P5）。

## 8. 上游复用

### 8.1 配置与优先级

```toml
[xlings]
upstream = []                        # 默认：自包含
# upstream = ["~/.xlings"]           # 普通机器
```

读取顺序：用户 `~/.mcpp/config.toml` → mcpp 系统级配置（Linux/macOS `/etc/mcpp/config.toml`，Windows
`%ProgramData%\mcpp\config.toml`）→ 空。条目可以是一个 xlings home（只提供 xim），也可以是一个 mcpp home
（§9 的系统 home：其 registry 提供 xim，其 store 提供 mcpp 的命名空间），由目录中的标记判别。

### 8.2 信任边界

上游必须属于当前用户或 root（Windows：当前用户、Administrators、SYSTEM），且对当前用户不可写或属于当前用户本人。
不满足时在配置加载时报错：借用另一个普通用户的 home 等于执行对方可以随时替换的程序。

### 8.3 语义（X3，mcpp 只给声明）

适配器把 `upstream` 写为 registry home 的上游声明：xlings home 限定命名空间 `["xim"]`；mcpp 系统 home 的 registry 限定 `["xim"]`，
其 store 限定为 mcpp index 所服务的命名空间。安装一个 payload 时：

| 情况 | 结果 |
|---|---|
| 本地 store 已有完整的 | 用本地 |
| 某个上游有同一身份（命名空间、名字、版本、revision）且完整，运行库闭包也可借 | 在本地 store 发布链接条目（Linux/macOS 符号链接，Windows junction），整闭包一起；并在借用方 home 中照常配置（subos 条目、`abi` 链接） |
| 否则 | 下载、安装到本地 |

- 上游永不被写；删除或替换本地条目只删除链接本身（§2.2 已观察，X3 写成要求）。
- 上游的文件系统不支持符号链接时不借。
- mcpp 的 index 只从 mcpp 的 home 借（自己的系统 home），永不从 xlings home 借，即使用户的 `~/.xlings` 也注册了 mcpplibs。

### 8.4 失效与自愈

上游删除了被借的 payload 后，本地条目悬空；`find`/`install` 视为未安装，xlings 在同一路径上安装实体，已构建产物引用的路径不变。
离线时（`--offline`）无法自愈，报错并说明哪个上游条目失效。

### 8.5 存量重复（O4，已确认）

`mcpp self store dedupe [--apply]`：列出本地 xim payload 中与上游身份一致的条目，`--apply` 时以链接条目逐条原子替换
（`exchange_paths`）。默认只列出。

## 9. 系统级安装（O5）

### 9.1 部署形态

| 形态 | mcpp 入口 | 用户的 MCPP_HOME | 系统 home | 写入者 |
|---|---|---|---|---|
| 自包含（默认） | 任意位置 | `~/.mcpp` 或 `MCPP_HOME` | — | 用户 |
| 用户 + xlings 上游 | 同上 | `~/.mcpp` | — | 用户；上游为 `~/.xlings` |
| 系统 | 包管理器或 `sudo xlings install --system mcpp` | 每个用户的 `~/.mcpp` | `/var/lib/mcpp`（macOS `/Library/Application Support/mcpp`，Windows `%ProgramData%\mcpp`；`/etc/mcpp/config.toml` 的 `[system] home` 可改） | 管理员 `mcpp --system …`；用户只读 |
| Luban OS | `/xlings` 中的 `xim:mcpp` | 每个用户的 `~/.mcpp` | 同上；系统 home 自己的上游为 `/xlings` | 同上 |

系统 home 的布局与用户 home 相同（§5.1），另有标记 `.mcpp-home`（`mode = "system"`）。它不含 `build-cache`。

### 9.2 写：管理员

- `--system` 是作用域标志，只用于维护命令：`self init`、`toolchain install|remove|default`、`index update`、
  `self store gc|dedupe`、`self doctor`。`build`、`run`、`test`、`new`、`add` 等带 `--system` 时拒绝：
  以 root 构建会把 root 所有的文件写进用户的项目。
- 判断依据是对系统 home 的写权限，不是 uid：管理员也可以把系统 home 交给一个组维护。没有写权限时报错并提示 `sudo`。
- 写入时 umask 为 `022`，保证所有用户可读；写入持有系统 home 的排他锁。
- 在 `sudo` 下忽略调用者的 `HOME` 与 `MCPP_HOME`，系统 home 只取自系统级配置。
- 系统级配置可以给出默认工具链（`[toolchain] default = "gcc@16.1.0"`），用户配置可覆盖。

### 9.3 读：用户（无需 root）

- 系统级配置给出 `[xlings] upstream = ["/var/lib/mcpp", "/xlings"]`（Luban）或 `["/var/lib/mcpp"]`。
- 用户有自己的 index 数据（很小，刷新无需 root）；系统 home 中的 payload 只在身份一致时被借。用户的 index 解析出更新的版本或
  revision 时装进自己的 `~/.mcpp`。
- `build-cache`（含 std 模块 BMI）每个用户一份，不跨用户共享：跨用户的构建缓存是投毒面。
- `mcpp self update` 在系统形态下拒绝，提示由包管理器更新。

### 9.4 Luban OS

Luban 的 edition 提供 `/etc/mcpp/config.toml`（X7），并在系统镜像制作时以 `mcpp --system toolchain install` 预置默认工具链。
系统 home 的 registry 以 `/xlings` 为上游，所以 xim 的 payload 在机器上仍只有一份。

### 9.5 Luban 系统内部的 mcpp

上面是 Luban **宿主**上的多用户形态。一个 Luban 系统（`luban new`）**内部**也带 mcpp（core、agent-workspace）：

- 系统的 `/xlings` 是它自己的 home 与 store，进入后在 `/` 下可见（xlings 设计 §5.1）；在系统内 root 所有，满足 §8.2。
- edition 的 factory `/etc` 带 `mcpp/config.toml`：`[xlings] upstream = ["/xlings"]`，默认工具链写明目标：
  `gcc@16.1.0` 加 `<arch>-linux-gnu`（edition 已带的 glibc 版 `xim:gcc`）。probe 实测 mcpp 在 aarch64 上把裸写的
  `gcc@16.1.0` 解析为 `xim:musl-gcc`（§2.2），不写目标会与系统已带的 gcc 身份不一致而再次下载。系统里的 `~/.mcpp` 借用系统的 gcc、glibc，第一次 `mcpp build` 不下载工具链。
- 系统里的普通用户（desktop 默认以普通用户登录）：xlings 设计修订 2 §8 给普通用户一个叠加在 `/xlings` 之上的用户层 home；
  mcpp 的上游仍是 `/xlings`（root 所有，满足 §8.2），用户的 `~/.mcpp` 只写自己的目录，无需 root。
- 系统换代（升级 gcc）后，被借条目随上游的身份变化：旧身份的链接悬空即按 §8.4 自愈，新版本按 mcpp 的工具链选择决定。

## 10. 文件系统能力（#809 同类）

`modules/platform` 增加与 xlings #671 同构的能力表：

- `platform::fs_caps(dir)`：每个文件系统每进程探测一次（no-replace rename、exchange、硬链接、符号链接、大小写、类型名），按设备缓存。
- `platform::rename_no_replace`、`exchange_paths` 按能力选择策略：原生 → 普通文件以硬链接发布 → 目录 rename 到不存在的名字。
- `scaffold_fs.cppm` 改用它（修复 F2）；§6 的交换、§8.5 的替换也用它。
- `doctor` 在 MCPP_HOME 的文件系统不完全原生时说明 mcpp 在那里如何写入。

## 11. 长路径

| 项 | 处理 |
|---|---|
| mcpp.exe | manifest 增加 `longPathAware`（与 `activeCodePage` 同一份） |
| 目标键 | `windows_long_paths = true \| false`，与 `windows_code_page` 同构：宿主工具默认 `true`；用户程序默认不写（程序能否处理长路径由作者声明）；库目标拒绝 |
| xlings.exe | X6 |
| 引擎路径 | 继续经 `extended_length`；store、index、交换操作同样经过它 |
| 路径预算 | 配置加载时：Windows 上 `LongPathsEnabled = 0`，且 MCPP_HOME 长度加上 registry 内已知最深后缀（以契约测试中测得的值为常量）超过 259 时，degraded：impact"安装可能因路径过长失败"，并给出开启开关或缩短 MCPP_HOME 的办法 |
| 系统开关 | `doctor` 报告 `LongPathsEnabled`；mcpp 不替用户修改注册表 |

## 12. xlings 的来源与握手

`[xlings] binary = "bundled" | "system" | <路径>`（已有键）。握手（X5）读取版本、协议、能力清单与支持的 home layout：

| 检查 | 不满足时 |
|---|---|
| 版本 ≥ `kXlingsVersion` | 退回内置，note |
| 协议在适配器支持的范围内 | 同上 |
| 能力包含 `install_packages` 与 X1–X5 | 同上 |
| 能写 registry home 当前的 layout | 同上；内置也不能写时报错，指出 home 由更新的 xlings 写过 |

`[xlings] home` 仍是 registry 的位置；指向不是 mcpp 建立的 xlings home 时在配置加载时拒绝，提示改用 `upstream`。

## 13. 诊断分级

按 `src/diag.cppm`：做得比要求少，必须报错或 `degraded`（带 impact）。

| 情形 | 等级 |
|---|---|
| `[xlings] home` 指向非 mcpp 的 home | 配置加载时报错 |
| 上游不满足信任边界（§8.2） | 配置加载时报错 |
| `--system` 用于构建类命令 | 报错 |
| `--system` 而无写权限 | 报错，提示 `sudo` |
| 选定的 xlings 不满足握手，内置可用 | note |
| 内置 xlings 也不能写 registry 的 layout | 报错 |
| 上游不存在或不可读 | degraded：impact"这次不复用"；`--strict` 下为错误 |
| 上游 payload 身份不一致 | verbose（正常结果） |
| 链接条目悬空、原路径重装 | note；离线时报错 |
| 上游文件系统不支持符号链接 | degraded：impact"不复用" |
| Windows 路径预算超出且开关关闭 | degraded（§11） |
| MCPP_HOME 的文件系统不完全原生；`LongPathsEnabled` 未开 | doctor 信息行 |

## 14. xlings 侧需求（前置发布）

| 编号 | 能力 | 说明 |
|---|---|---|
| X1 | index 数据目录由调用方指定；拉取到指定目录 | `index_repos` 条目增加 `dir`；接口能力 `sync_index` 带 `dest` |
| X2 | 按 index 指定 store | `index_repos` 条目增加 `store`；index 声明自己服务的命名空间 |
| X3 | home 的只读上游 | `.xlings.json` `upstreams: [{home, namespaces}]`；来源可为任意模式的 home；身份（含 revision）与完整性校验；闭包；本地链接条目（Windows junction）；借用方 home 照常配置 subos 条目与 `abi` 链接；删除或替换条目只删链接；悬空自愈；从不写上游。推广现有 system layer 的 borrow |
| X4 | 完整性与查询 | 接口能力返回 `payload_dir`、`revision`、`origin`、`complete`，不触发安装；`install_targets` 同样给出 `complete`；"报告成功但未安装"（mcpp#533 一类）由 xlings 判为不完整 |
| X5 | 握手 | 接口描述返回版本、协议、能力清单、支持的 home layout |
| X6 | Windows 长路径 | xlings.exe `longPathAware`；`GetModuleFileNameW` 可增长缓冲；路径过长报为路径过长（今天报 "internal error … likely a bug"） |
| X7 | Luban OS | 宿主：系统级配置 `/etc/mcpp/config.toml` 与镜像制作时预置 mcpp 系统 home（§9.4）；系统内部：edition 的 factory `/etc/mcpp/config.toml` 以 `/xlings` 为上游（§9.5）。由 xim-pkgindex 的 edition 新版本提供 |

另依赖 xlings#652：安装失败的原因进入 `InstallStatus.message`，适配器把它交给 mcpp 的诊断。

发布顺序（与 xlings 设计 §16 一致）：xim-pkgindex PR（edition、terminfo 等）→ xlings PR（含 X1–X6 与 #652）→ xlings 发布 →
mcpp PR 钉该版本 → mcpp 2026.10.16.1 → xim-pkgindex bump mcpp，core 的下一版本带它。

## 15. 测试与 CI

| 层 | 内容 |
|---|---|
| 单元 | `mcpp.provider.fake` 驱动 toolchain、pm、runtime 的测试；`fs_caps` 各策略以声明的能力驱动；配置优先级；握手判定；信任边界判定；路径预算 |
| 契约 | `tests/provider/`：内置钉版本与最新发布 xlings 各一遍；测得 registry 内最深后缀，作为路径预算常量的依据 |
| e2e 自包含 | 全新 MCPP_HOME 安装 gcc、llvm：payload 中没有任何 mcpp 写入的文件；工具链二进制的 PT_INTERP 经 registry 的 `abi`；构建运行 |
| e2e 复用（Linux、macOS、Windows） | 上游 home（root 所有、只读）装好工具链；`upstream=[它]`：断言未下载、本地为链接条目、构建运行、产物加载器在本地 store；删除上游 payload 后原路径自愈；上游摘要前后一致 |
| e2e 系统级（Linux） | `sudo mcpp --system self init` 与 `toolchain install`；两个普通用户（`useradd`）各自构建运行，系统 home 摘要不变；`mcpp --system build` 被拒绝；无写权限的 `--system` 被拒绝 |
| e2e NFS | NFSv3 上的 MCPP_HOME 与项目目录：`toolchain install gcc`、`mcpp new`、构建运行（§2.4 的复现转为回归） |
| e2e Windows 深路径 | 开关为 0 时路径预算的 degraded 出现；开关为 1 时 157 字符 MCPP_HOME 构建成功；mcpp.exe manifest 含 `longPathAware` |
| e2e 迁移 | 2026.10.11.1 建立的 home：index 移动、旧 store 兼容读、已构建产物可运行、旧标记被忽略 |
| lint | `check_provider_boundary.py` |

新增 e2e 在上一个发布版上必须失败（复用、系统级、NFS 的 `mcpp new`、manifest）。合并前 dispatch mcpp-plugins CI。

## 16. 文档与规范

- 新文档 `docs/25-home-indices-and-store.md`（与 `docs/zh/`）：`~/.mcpp` 布局、归属、复用、系统级安装、信任边界、文件系统要求、Windows 长路径。
- `docs/04-mcpp-toml.md`：`windows_long_paths`；`[indices]` 位置规则。
- config.toml 文档节：`[xlings] upstream`、握手、`[indices.<name>] dir/store`、`[system] home`。
- `docs/09-commands-by-scenario.md`：`--system`、`self store dedupe`。
- `docs/specs/toolchain-management.md`：payload 不可变；工具链运行库由 registry home 钉版本；修订号与变更记录。
- `docs/specs/package-identity.md`：payload 身份含 revision；store 位置由 index 决定；修订号与变更记录。
- `docs/91-toolchain-internals.md`：删除 post-install 修改的描述。
- CHANGELOG 2026.10.16.1。

## 17. 提交顺序（一个 PR 内）

1. `platform`：`fs_caps`、`rename_no_replace`、`exchange_paths`；`scaffold_fs` 改用它们。
2. 内置 xlings 钉到 X 发布；`kXlingsVersion` 说明增加第八条；`.github` 钉版本随之更新。
3. `mcpp.provider` 端口、`fake`、适配器迁入 `src/provider/xlings/`；握手；lint。
4. 按领域迁移调用方并删除布局知识：toolchain → build/prepare 与 hermetic → runtime → pm → doctor 与 cli → fallback。
5. index 位置与暂存交换；存量迁移与兼容读。
6. payload 不可变：删除 W1–W6；registry 运行库钉版本。
7. 上游复用：配置、信任边界、`self store dedupe`、doctor。
8. 系统级安装：`--system`、系统级配置、`.mcpp-home` 标记。
9. 长路径：`windows_long_paths`、mcpp.exe manifest、路径预算、doctor。
10. 测试与 CI lane。
11. 文档、规范、CHANGELOG、版本号 2026.10.16.1。

## 18. 决策

| 编号 | 决策 | 状态 |
|---|---|---|
| D1 | 端口模块 `mcpp.provider`；`mcpp.xlings` 降为适配器 | 已确认 |
| D2 | 归属按 index 划分 | |
| D3 | mcpp 的 index 与 store 在 `~/.mcpp/indices`、`~/.mcpp/xpkgs`；`registry/` 为适配器私有 | |
| D4 | 项目 index 的数据与 payload 在 `<project>/.mcpp/` | 已确认（O6） |
| D5 | index 刷新改为暂存再交换 | |
| D6 | mcpp 不写 payload：删除 W1–W6 与 #273 限制 | 依据 §2.1、§2.2 |
| D7 | 工具链运行库由 registry home 的运行库钉版本决定 | 依据 §2.1 |
| D8 | 复用：`[xlings] upstream`，默认空，用户配置 > 系统级配置 > 空；链接条目；上游只读；悬空自愈 | |
| D9 | store 路径不 canonical 化；兄弟包与删除经端口 | |
| D10 | `[xlings] home` 仍是 registry 位置，拒绝非 mcpp 的 home | |
| D11 | 所有 xlings 来源经握手，不满足回退内置 | |
| D12 | mcpp 平台层的文件系统能力表与 xlings 同构 | |
| D13 | `windows_long_paths`；mcpp 自身与宿主工具默认开；路径预算检查 | |
| D14 | `mcpp self store dedupe`，默认只列出 | 已确认（O4） |
| D15 | 系统级安装：`--system` 只用于维护命令；以写权限判断；用户侧无需 root；构建缓存不跨用户 | |
| D16 | 上游信任边界：当前用户或 root 所有 | |
| D17 | 一个 PR，mcpp 2026.10.16.1；前置 xlings 发布 X1–X7 | |

## 19. 开放问题

| 编号 | 问题 | 建议 |
|---|---|---|
| O7 | 系统 home 的默认位置：Linux `/var/lib/mcpp`（FHS 的可变状态）还是 `/opt/mcpp` | `/var/lib/mcpp`；Luban 可在系统级配置中改 |
| O8 | `--system` 是否也允许 `index update` 以外的 index 管理（`index add`）：系统级私有 index | 允许，写入系统级配置的 `[indices]`，用户可见但不可改 |

## 20. 自我 review

| 角度 | 发现与处理 |
|---|---|
| 架构 | 修订 1 只删除 patchelf 与 cfg，漏了 `.mcpp_ok`、`.mcpp-fixup.json` 与 `xpkg_copy` 三处写入或复制（probe 第 1 轮暴露）；已补为 W4–W6 |
| 借用的完整性 | 修订 1 的借用只发布链接，没有说明借用方 home 的 subos 条目与 `abi` 链接；mcpp 的 sysroot 是 registry 的 `subos/default`，缺了它头文件与 crt 会缺失；已写入 X3 |
| 安全 | 修订 1 没有信任边界：上游可以是任意目录；已加 §8.2。系统级安装不共享构建缓存（§9.3） |
| 删除 | `std::filesystem::remove_all` 作用于 junction 的行为依赖实现；规定删除只经端口交给 xlings，lint 禁止对 `Payload::dir` 调用 `remove_all`；§2.2 观察到 xlings 只删链接，X3 写为要求并由 e2e 断言 |
| 并发 | 两个进程同时借同一条目：发布走 `rename_no_replace`，后者看到已存在即使用；自愈与借用同一路径同样如此 |
| 离线 | 悬空条目在离线时无法自愈，报错而不是静默降级（§8.4） |
| 跨平台 | junction 不能指向网络共享：Windows 上上游在网络路径时不借（degraded）；macOS 上 clang 经真实路径找资源目录，身份一致保证与上游 payload 内部自洽（§2.2） |
| 兼容 | 存量 home 的旧位置、旧标记、已修改的 payload 都保持可用（§5.3）；默认仍是自包含，已安装的 mcpp 行为不变 |
| 长路径 | 修订 1 只有 manifest；probe 显示开关关闭时首先失败在 xlings，增加路径预算检查与 X6 的如实报告 |
| 范围 | 内容多，但都是"mcpp 对 xlings 的依赖"一个主题的组成部分；按 §17 的顺序在一个 PR 内完成 |
| 生态（修订 3） | 修订 2 只考虑了 Luban 宿主，没有考虑 Luban 系统内部同时带 gcc 与 mcpp 而 mcpp 再下载一份的情形；已补 §9.5 与 X7 |
| 生态（修订 3） | mcpp 在系统里的失败诊断依赖 xlings#652；已列为依赖 |
| 生态（修订 3） | aarch64 上 mcpp 的 `gcc@16.1.0` 指 musl 版，core 带的是 glibc 版；edition 的 mcpp 配置写明目标（§9.5） |
| 发布（修订 3） | edition 是 mcpp 新能力的使用者，必须在 mcpp 发布之后才 bump 进 core；顺序已与 xlings 设计对齐 |

## 21. 发布

release PR `release/2026.10.16.1`；CI 全绿后自我 review，dispatch mcpp-plugins CI，合并后 `release.yml`。
检查每个归档内置的 xlings 版本等于 X 发布的钉版本；clean-room 中分别验证自包含、`upstream=["~/.xlings"]`、系统级三种形态。
probe PR #810 保留到发布完成后关闭。
