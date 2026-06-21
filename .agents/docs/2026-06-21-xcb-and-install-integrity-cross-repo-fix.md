# 跨仓库修复方案:残缺/被删依赖被 `.mcpp_ok` 盲区放过

> 状态:草案 / 待实施
> 影响面:经 `imgui[docking-full]`(或任何 X11/libxcb 链)构建的项目;切换/重装 llvm 等工具链的用户
> 涉及仓库:`mcpplibs/mcpp-index`(配方层) + `mcpp`(核心层);附带 toolchain/ABI 可用性
>
> 本方案覆盖同一系统性病根的**两个实证**:
> - **案例 A**:`compat.xcb` 头文件生成空转 → 残缺包被标记完整(§2.2)
> - **案例 B**:llvm 工具链的 `xim-x-zlib` 运行时被严格清理删除且未还原(§2.4)
> 二者都是「`.mcpp_ok` 只证进程/布局、不证内容」+「strict marker-only 清理对 legacy 包先删后赌重装」的不同表现。

---

## 1. 现象

用户项目仅声明:

```toml
[dependencies]
imgui = { version = "0.0.6", features = ["docking-full"] }
```

`mcpp build/run` 在编译传递依赖 `compat.xcb`(libxcb)时报:

```
compat-x-compat.xcb/1.17.0/src/xcb.h:209:10: fatal error: xproto.h: No such file or directory
  209 | #include "xproto.h"
```

用户项目本身无问题,根因在依赖的打包与安装链路。

---

## 2. 根因(两层)

### 2.1 关键事实

`libxcb` 的协议头 `xproto.h` / `bigreq.h` / `xc_misc.h`(及对应 `.c`)**不是源码自带**,而是在 `install()` 阶段用 libxcb 自带的代码生成器 `c_client.py` 读取 `xcb-proto` 的 `*.xml` **现场生成**的。

注意:它与 `xorgproto` 提供的 `X11/Xproto.h`(大写、完全是另一个文件)无关——编译命令里挂的 `xorgproto` 的 `-I` 帮不上忙。

### 2.2 配方层缺陷(`mcpplibs/mcpp-index` · `pkgs/c/compat.xcb.lua`)

**Bug A — `resolve_python()` 兜底挑错文件。**
xim-python 的 `bin/` 同时存在 `python3.13` 和 `python3.13-config`。当 `python3`/`python` 符号链接不存在、走到兜底分支时:

```lua
local matches = os.files(path.join(python.bin, "python3.*")) or {}
table.sort(matches)
if #matches > 0 then
    return matches[#matches]   -- 取“最后一个”
end
```

glob `python3.*` 同时命中两者,`table.sort` 升序后 `["python3.13", "python3.13-config"]`,`matches[#matches]` 取最后一个 = **`python3.13-config`**(配置助手脚本,不是解释器)。把 `c_client.py` 喂给它:**退出码 0、不报错、零产出**。

**Bug B — `install()` 不校验生成产物。**
生成步骤即便空转也无人发现,只能等后续 `copy_public_headers` 的 `os.cp(src/xproto.h, ...)` 撞缺文件才崩;错误信息含糊,且在某些环境下不一定崩,从而把残缺包"装完"。

### 2.3 核心层盲区(`mcpp` · `src/pm/package_fetcher.cppm` + `src/fallback/install_integrity.cppm`)

完整性标记 `.mcpp_ok` 的写入条件是:

- `package_fetcher.cppm`:`xlings 退 0 且 verdir 存在` → 写标记;
- copy 兜底路径:仅凭 `looks_complete_legacy`(看目录里有没有 `src/` 之类的**布局启发式**)→ 写标记。

**它证明的是"安装进程跑完了 / 布局看起来像",而不是"产物正确"。** 于是配方退 0 但产物缺失的残缺包被打上 `.mcpp_ok`、被永久信任,问题被拖到编译期才以 `xproto.h not found` 爆出来。

这与既有的 `#120 空壳+缓存血统`、`包解析 identity-first 盲取首命中` 属同一类系统性问题。

### 2.4 案例 B:llvm 工具链 `xim-x-zlib` 运行时被删且未还原

**现象**:切到 llvm 后 `mcpp run` 报

```
'... clang++ --version' exited with status 127
# 真实 stderr:
clang++: error while loading shared libraries: libz.so.1: cannot open shared object file
```

`mcpp toolchain install llvm` 重装也不好。

**关键事实(实测)**:

- clang 的 ELF 解释器是 **xim 私有 glibc 加载器**(`xim-x-glibc/.../ld-linux`),**不搜系统 `/usr/lib`**,只认二进制 RUNPATH。clang 的 RUNPATH 把 `libz.so.1` 指向 `xim-x-zlib/<ver>/lib` 和 `subos/default/lib`。
- `xim-x-zlib/<ver>` 版本目录**整个被删**(只剩空壳,甚至出现嵌套 `xim-x-zlib/xim-x-zlib` 残骸);`subos/default/lib/libz.so.1` 软链**悬空**。
- clang 二进制本身完好——补上任意可用的 libz 路径后 `clang++ --version` 立即成功。
- `ldd clang` 却显示一切正常:因为 `ldd` 用**系统**加载器(能找到系统 libz),而 clang 实跑用**私有**加载器(不看系统路径)——这是排查时的迷惑点。

**根因**:这些 xim 运行时包(zlib/glibc/libxml2)都是**早于标记系统的 legacy 包,无 `.mcpp_ok`**。`install_integrity.cppm` 的 `is_install_complete` 是 strict marker-only,解析/安装路径上对无标记目录执行 `clean_incomplete_install`(**先删、再赌"一次性重装"**)。这次对 `xim-x-zlib` 的重装**没有还原成功**(典型:xlings 装到 `$XLINGS_HOME` 没落回 sandbox、或静默失败),于是 zlib 版本目录被删却没补回。clang 因此加载不到 `libz.so.1` → 127。

**为何 `toolchain install llvm` 无效**:它只重装 llvm 本体(有标记、会重解压),而 `libz.so.1` 来自**独立的 xpkg `xim-x-zlib`**,llvm 安装不会去还原这个缺失的兄弟包。

与案例 A 同根:`.mcpp_ok` 不证内容 + strict 清理对 legacy 包"先删后赌重装"。

---

## 3. 修复方案(按仓库)

### 3.1 仓库 `mcpplibs/mcpp-index` — `pkgs/c/compat.xcb.lua`(治本,优先)

**修 Bug A:兜底只保留真正的解释器。**

```lua
-- 兜底:只保留真正的解释器(python3 / python3.<minor>),
-- 排除 -config / -gdb 等同前缀助手脚本;取最规范(最短)的一个。
local interpreters = {}
for _, m in ipairs(os.files(path.join(python.bin, "python3*")) or {}) do
    if path.filename(m):match("^python3%.?%d*$") then
        table.insert(interpreters, m)
    end
end
table.sort(interpreters)
if #interpreters > 0 then
    return interpreters[1]
end
```

正则 `^python3%.?%d*$` 命中 `python3` / `python3.13`,拒绝 `python3.13-config`(含 `-config`)。

**修 Bug B:生成后显式校验,缺产物即 `return false`。**

```lua
os.cd(srcdir)
for _, name in ipairs({ "xproto", "bigreq", "xc_misc" }) do
    local cmd = string.format(...)   -- 不变
    os.exec(cmd)
    if not os.isfile(path.join(srcdir, name .. ".h"))
    or not os.isfile(path.join(srcdir, name .. ".c")) then
        log.error("c_client.py did not generate %s.{h,c} (python=%s)", name, python)
        return false
    end
end
```

两处合并后:不再挑错解释器;即便挑错,也在**安装期硬失败**,绝不产出残缺包。

> `pkgs/c/compat.xcb-proto.lua` 无需改:协议头本就设计成在 `compat.xcb` 的 `install()` 里生成,`xcb-proto` 只负责提供 `*.xml` + `xcbgen`,职责正确。

**验证**:删除已装的 xcb 版本目录触发重装,确认重装成功且 `src/` 下出现 `xproto.{h,c}` / `bigreq.{h,c}` / `xc_misc.{h,c}`,`include/xcb/` 下出现公共头,且写入了 `.mcpp_ok`;随后下游项目能编过 libxcb。

### 3.2 仓库 `mcpp` — 完整性标记应校验"内容"而非"进程退出码"(防御纵深)

目标:即便某个配方有 bug,残缺安装也不该被标记为完整。

- **让配方声明产物清单**:在 `mcpp` 段支持可选 `verify = { "include/xcb/xproto.h", ... }`(相对 install dir 的必存路径);`mark_install_complete()` 之前逐一 `exists` 校验,缺任一即判失败、清理目录、不写标记。
- **新装路径禁用 `looks_complete_legacy` 打标记**:布局启发式分不清"半成品"与"完整包",仅应用于"老包一次性收编",不应作为刚装完的完整性判定(见 `package_fetcher.cppm` copy-fallback 分支)。
- **legacy 包"先删后重装"必须可回滚(案例 B 核心)**:`clean_incomplete_install` 在 resolve 路径上对**无标记但内容完好的 legacy 包**直接 `remove_all` 是危险的——一旦后续重装没把它落回 sandbox(xlings 装到 `$XLINGS_HOME`、静默失败等),就把一个**本来能用**的包删没了。要求:
  - 删除前先**备份/暂存**(rename 到 `.bak`,而非直接 `remove_all`);
  - 重装确认还原成功(目标产物存在)后才删除备份;**还原失败则回滚备份并报错**,绝不留下"删了没补"的空洞;
  - 或更稳:对 legacy-complete 包采用**"原地收编"**(校验通过即补写 `.mcpp_ok`),避免无谓的删-装往返(见 §4 恢复:全局副本 `$XLINGS_HOME/data/xpkgs/...` 通常就是好的)。
- **被删依赖的连带破坏要可检测**:工具链 RUNPATH 指向的运行时兄弟包(zlib/glibc/libxml2…)被删后,`subos/default/lib` 会留**悬空软链**。`mcpp self doctor` 应扫描 toolchain RUNPATH 与 subos 软链,发现悬空/缺失即报可操作提示。

**改动点**:`src/pm/package_fetcher.cppm`(写标记前插入 verify 校验;legacy 删除改为 rename-备份+失败回滚)、`src/fallback/install_integrity.cppm`(新增"按清单校验"入口;`clean_incomplete_install` 增加可回滚语义;收紧新装路径)、`src/doctor.cppm`(RUNPATH/软链悬空体检)。

**测试(TDD)**:
1. "安装产出残缺包应被判失败、不得写 `.mcpp_ok`、并清理目录"(案例 A);
2. "legacy 无标记包在重装失败时必须回滚还原、不得被删空"(案例 B);
3. "doctor 能报出工具链 RUNPATH 指向的缺失运行时包"。

> 关系:3.1 的 Bug B 对**本包**已是充分修复(install 返回 false → xlings 非 0 → 核心层不写标记);3.2 是让**所有包**都不再被"退 0 但产物缺失"骗过,**且不再因清理把好包删坏**。

### 3.3 工具链 / ABI(环境配置 + `mcpp` 可用性)

现象:默认工具链一度解析成 `gcc 15.1.0-musl`,而 GUI 依赖(glfw / xcb 等)是 glibc 包 → `ABI mismatch: dependency 'compat.glfw' requires abi=glibc but the resolved toolchain ... is abi=musl`。

- **环境层(用户侧)**:glibc 依赖即用 glibc 默认工具链:`mcpp toolchain default gcc@<glibc-version>`。
- **`mcpp` 可用性(建议)**:
  1. ABI 报错改为**可操作**——直接提示应执行的 `mcpp toolchain default <glibc-toolchain>`;
  2. 排查 **global default 与实际解析不一致**:`mcpp toolchain default` 显示默认已是某 glibc 工具链,但 `mcpp run` / `toolchain list` 的 `*` 却解析/标记为 musl,疑似存在 project-local 或 resolved-context 覆盖,需厘清解析优先级。

---

## 4. 恢复手段(给已中招的用户)

当前没有 `mcpp reinstall <pkg>` 子命令;重装靠"标记缺失自动触发":

```bash
# 方式 A:只删标记,触发自清 + 重装
rm "$MCPP_HOME/registry/data/xpkgs/compat-x-compat.xcb/1.17.0/.mcpp_ok"

# 方式 B:删整个版本目录(更干净)
rm -rf "$MCPP_HOME/registry/data/xpkgs/compat-x-compat.xcb/1.17.0"

# 然后重建
mcpp build
```

> `$MCPP_HOME` 默认 `~/.mcpp`,`$XLINGS_HOME` 默认 `~/.xlings`。
> 注意:**裸重装会重跑同一个有 bug 的配方**,务必先落地 3.1 的配方修复(或先按 3.1 逻辑手动补齐生成产物)再重装,否则会再次产出残缺包。

**案例 B(工具链运行时被删,如 `xim-x-zlib`)**:全局 `$XLINGS_HOME/data/xpkgs/<pkg>/<ver>` 通常还保留着完好副本,直接还原回 sandbox 并打标记即可:

```bash
SRC="$XLINGS_HOME/data/xpkgs/xim-x-zlib/1.3.1"
DST="$MCPP_HOME/registry/data/xpkgs/xim-x-zlib/1.3.1"
rm -rf "$MCPP_HOME/registry/data/xpkgs/xim-x-zlib/xim-x-zlib"   # 清掉嵌套空壳残骸(若有)
cp -a "$SRC" "$DST"
printf '1\n' > "$DST/.mcpp_ok"                                  # 打标记,避免再次被严格清理删掉
test -e "$MCPP_HOME/registry/subos/default/lib/libz.so.1" && echo "subos 软链已恢复"
```

> 验证:`env LD_LIBRARY_PATH=<llvm>/lib <llvm>/bin/clang++ --version` 应正常输出版本,`mcpp run` 用 llvm 工具链可编译运行。

---

## 5. 实施顺序与验收

| 步骤 | 仓库 | 内容 | 验收 |
| --- | --- | --- | --- |
| 1 | `mcpplibs/mcpp-index` | 3.1 配方双修复(案例 A) | 删目录重装成功、生成产物齐全、下游编过 |
| 2 | `mcpp` | 3.2 产物校验 + 收紧新装标记 + legacy 删除可回滚 + doctor 体检 | 案例 A/B 两个失败测试 + doctor 测试通过;回归既有安装路径 |
| 3 | `mcpp` / 文档 | 3.3 ABI 报错可操作化 + 解析不一致排查 | 报错含修复命令;default 与实际解析一致 |

建议顺序:**先 1(治本、覆盖所有装该 X11 链的人),再 2(防御纵深,同时根治案例 B 的"删好包"风险),最后 3(可用性)**。

---

## 6. 经验沉淀

- "进程退出码 0" ≠ "产物正确";凡是 `install()` 里有**代码生成 / 外部子进程**的配方,都应在生成后**显式校验产物**并在缺失时 `return false`。
- 完整性标记(`.mcpp_ok`)只应在**内容校验**通过后写入;布局启发式不能用于新装的完整性判定。
- **清理 ≠ 可丢**:对无标记但内容完好的 legacy 包,"先删后赌重装"会把好包删坏;删除须可回滚,还原失败须报错,优先"原地收编"。
- xim 工具链是**自带私有 glibc 加载器**的可执行文件,只认 RUNPATH、不搜系统库;`ldd`(系统加载器)的结论不代表实跑——排查共享库问题以**实跑 stderr** 为准。
- 工具链 ABI 与依赖 ABI 必须一致;musl 工具链不能链接 glibc 预期的源码包。

---

## 7. 实施拆解(Implementation Breakdown)

本次目标:跨仓库**实现 → 打通 → 测试 → PR → CI 全绿 → 生态 OK**。聚焦两个问题:

- **问题 1(xcb)**:`compat.xcb` 头生成空转 → 残缺包 → `xproto.h not found`(配方层)
- **问题 2(mcpp run / llvm 报错)**:legacy 运行时包(`xim-x-zlib`)被严格清理删除且未还原 → `clang++` 加载 `libz.so.1` 失败 127(核心层)

### 7.1 问题 1 — `mcpplibs/mcpp-index`

**改动**:`pkgs/c/compat.xcb.lua`

| 项 | 内容 | 验收 |
|---|---|---|
| P1-a | `resolve_python()` 兜底:`os.files("python3*")` + `path.filename(m):match("^python3%.?%d*$")` 过滤,排除 `*-config`;取排序首项 | 单元/手测:在只有 `python3.13` + `python3.13-config` 的 bin 下返回 `python3.13` |
| P1-b | `install()` 生成循环后校验 `name..".h"` 与 `name..".c"` 存在,缺则 `log.error + return false` | 删 xcb 版本目录重装成功;故意误配 python 时安装**硬失败**而非产残缺 |

**测试**:
1. `lua5.4 -e "assert(loadfile('pkgs/c/compat.xcb.lua','t'))"`(CI lint 同款语法门)。
2. 本地:删 `xpkgs/compat-x-compat.xcb/<ver>` → `mcpp build`(imgui 项目)→ 确认 `src/xproto.{h,c}` 生成、`include/xcb/` 公共头齐、`.mcpp_ok` 写入、libxcb 编过。
3. CI:`validate.yml` 的 `smoke_compat_imgui.sh` / `smoke_compat_imgui_window.sh` 会构建 X11 链,直接覆盖。

**PR**:分支 `fix/xcb-codegen-python-resolve` → PR 到 `mcpplibs/mcpp-index` → 等 `lint` + `mirror-cn-reachable` + `smoke-linux` + `smoke-portable` 全绿。

### 7.2 问题 2 — `mcpp`(核心层)

**改动**:`src/fallback/install_integrity.cppm` + `src/pm/package_fetcher.cppm`(+ 可选 `src/doctor.cppm`)

| 项 | 内容 | 验收 |
|---|---|---|
| P2-a | `clean_incomplete_install` 增加**可回滚**语义:resolve 路径上对无标记目录,先 `rename → .mcpp_residue.bak` 而非 `remove_all`;调用方在重装确认成功后再删备份,失败则 `rename` 回滚 | 单元测试:重装失败时原目录内容必须原样还原 |
| P2-b | `mark_install_complete` 前可选**产物清单校验**(配方声明 `verify`);新装路径不再用 `looks_complete_legacy` 打标记 | 单元测试:残缺产物不得写 `.mcpp_ok` |
| P2-c(可选) | `mcpp self doctor` 扫工具链 RUNPATH 与 `subos/default/lib` 悬空软链 | doctor 能报出缺失的 `xim-x-zlib` |

**测试**:
1. 单元/集成测试(见 §3.2 TDD 三项):案例 A(残缺包判失败)、案例 B(重装失败回滚)、doctor 体检。
2. 本地 e2e:构造"无标记 legacy 包 + 重装失败"场景,验证目录被回滚而非删空。
3. 回归:既有安装路径(正常装包、copy-fallback)不受影响。

**PR**:分支 `fix/install-integrity-rollback` → PR 到 `mcpp-community/mcpp` → 等其 CI(构建 + 测试矩阵)全绿。

### 7.3 顺序与"生态 OK"判据

1. 先 **问题 1**(配方,解锁所有 X11 链用户),本地 + CI smoke 验证;
2. 再 **问题 2**(核心,根治"删好包"),单元 + e2e;
3. **生态 OK** = mcpp-index 的 imgui smoke 全绿 + mcpp 自身测试矩阵全绿 + 一个真实 imgui 项目 `mcpp run` 起得来 + llvm 工具链切换不再 127。

### 7.4 进度

- [x] 文档落 `.agents/docs/`,补实施拆解(本节)
- [x] P1-a / P1-b 实现(`compat.xcb.lua`)+ lua lint + 过滤逻辑单测
- [x] **问题 1 PR `mcpplibs/mcpp-index#43` — CI 全绿**(lint / mirror-cn / smoke-linux(imgui→xcb)/ smoke-macos / smoke-windows 全 ✓)
- [x] P2-a 实现:`InstallStash` 可回滚守卫(`install_integrity.cppm` + `package_fetcher.cppm`)+ 5 项 gtest 单测
- [x] 问题 2 本地构建 + `mcpp test` 22 passed;PR `mcpp-community/mcpp#137`(linux build+test ✓ / macOS e2e ✓;windows 首轮因测试持有文件句柄失败 → 已修 `read_first_line` 关闭句柄后重跑)
- [x] 生态 OK 复核:clang++ 20.1.7 无 127(zlib 已还原)、xcb 头齐、myapp glibc 构建+启动
- [x] **问题 2 PR `mcpp-community/mcpp#137` — CI 全绿**(linux build+test ✓ / windows build+test ✓ / macOS e2e ✓)

**收尾状态:两个 PR 的 CI 全部通过。** `mcpp-index#43`(问题 1)+ `mcpp#137`(问题 2)均三平台绿;后续项(P2-b 配方 verify 清单、新装禁用 looks_complete_legacy、P2-c doctor 体检)见 §3.2,留作独立 PR。

> 注:问题 1 的本地 e2e 被本会话反复删包导致的环境损坏("python build dependency not found",非配方问题)阻塞;已确认 main 的 `validate`(含 imgui→xcb smoke)在干净 CI 为绿,故以 PR CI smoke 为权威验证。
> P2-b(配方声明 `verify` 产物清单 + 新装禁用 looks_complete_legacy 打标记)与 P2-c(doctor 体检)留作后续 PR,本次先落最小且根治案例 B 的 P2-a。
