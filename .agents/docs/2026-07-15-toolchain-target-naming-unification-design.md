# 工具链 × 目标 命名统一 — 设计方案(实现 + 显示 + 使用)

> 2026-07-15 · 基于 0.0.92(mingw-cross 已发布)代码审计 + 跨生态调研(rustup/cargo、Zig、Go、Clang、GCC 世界、xmake、CMake、Swift SDK、Bazel、Nix)
> 姊妹文档:`2026-07-15-mingw-linux-cross-windows-design.md`(本方案的直接诱因)、
> `2026-07-13-toolchain-backend-abstraction-msvc-mingw-design.md`(CommandDialect 抽象层)、
> `2026-06-30-target-bare-alias-sugar-design.md`(`[target.linux]` 糖,本方案需兼容)
> 记忆:[[mingw-linux-cross-windows]]、[[toolchain-dialect-and-mingw]]、[[msvc-system-toolchain]]

## 0. 一句话

把「**toolchain = family@version** × **target = triple(arch-os[-env])**」立为唯一的二轴身份模型:
musl / mingw / cross 都**不再是工具链名字**——变体(gnu/musl/msvc)进 triple 的 env 段,"cross" 只允许
出现在 xim 分发包名层(那里它是合法的,业界先例一致);实现上把四个平行的 triple 解析器收敛为单一
`triple.cppm`,全部旧拼写的识别/归一收进单一 **`compat.cppm`**(核心代码只见 canonical 形),
`--target` 从未校验自由文本变为封闭词汇表 + 逃生舱;显示与 CLI 按 rustup 的 toolchain/target
两轴模型重排。**单 PR 一次落地(0.0.93)**,沿用 0.0.90/0.0.91 的 single-PR 惯例,
PR 内按 §7 的八步各自编译绿、e2e 门禁收口。

> **决策已定**(2026-07-15 review,含二次 review):D1 = `x86_64-windows-gnu` canonical;
> D2 = mingw/mingw-cross 降级为别名(family 只剩 gcc/llvm/msvc);D4 = **单 `toolchain` 名词 +
> `--target` 选项,不设 `mcpp target` 子命令**(二次 review 改定,理由见 §6.1);D8 = 不分期,
> 整套单 PR;D9 = 兼容层独立 `src/toolchain/compat.cppm`。其余(D3/D5/D6/D7)按推荐执行。

---

## 1. 问题定性 — 为什么 `mingw-cross` 让人疑惑(case study)

从 mcpp 用户视角,`mcpp toolchain install mingw-cross 16.1.0` 有三重错位:

**① 同一概念、两个名字,按 host 劈开。** 「GCC 家族、产出 Windows PE(GNU CRT)」这一个概念,
在 Windows host 上叫 `mingw`,在 Linux host 上叫 `mingw-cross`——名字随用户站在哪台机器而变。
但用户的意图始终是 target 语义(「我要产出给 Windows」),却被迫在 toolchain 名里表达 host 关系。
`available_toolchain_indexes()`(registry.cppm:281-294)按 host `if constexpr` 分流两个名字,
就是这个错位的代码形态。

**② "cross" 是 host 相对属性,不是身份。** 行业调研结论(§3):Rust 官方生态**零** "cross" 命名
——装交叉目标是 `rustup target add <triple>`(target 操作,rustc 本体不动);Zig/Go 里 native 与
cross 是同一条命令改参数;GNU 传统里 "cross compiler" 是描述词(host≠target 的那条链)而非名字。
"cross"/"native" 后缀合法出现的**唯一**场合是分发包命名层:musl.cc 的
`aarch64-linux-musl-cross.tgz` vs `-native.tgz`、Debian 的 `g++-mingw-w64-x86-64`——因为分发包
确实要区分「这个包的宿主是谁」。**mcpp 的 xim 包 `mingw-cross-gcc` 恰好就在分发层,命名是对的;
错误在于让它漏进了用户 spec 层**(`mingw-cross@16.1.0` 成了用户要打的字、`toolchain list` 里的一行)。

**③ 同一语义轴、四种编码。** 「target 环境变体」这一个轴,现状有四种写法:

| 变体 | 编码位置 | 例 | 证据 |
|---|---|---|---|
| musl | 编译器名前缀 | `musl-gcc@15.1.0` | registry.cppm:155 |
| musl | 版本号后缀 | `gcc@15.1.0-musl` | registry.cppm:156 |
| musl | triple 命名编译器 | `aarch64-linux-musl-gcc@16.1.0` | registry.cppm:157,171-178 |
| mingw(msvcrt) | 编译器名 | `mingw@16.1.0` / `mingw-cross@16.1.0` | registry.cppm:186,191 |

业界的正典位置只有一个:**triple 的 env/abi 段**(Rust `-gnu/-musl/-msvc`、Zig 第三段、Clang env 段,§3 收敛点 3)。

---

## 2. 现状审计(file:line)

### 2.1 三层命名 + 有损反向映射

| 层 | 例 | 定义处 |
|---|---|---|
| 用户 spec | `gcc@16.1.0`、`gcc 15.1.0-musl`、`mingw-cross@16.1.0`、`msvc@system` | `parse_toolchain_spec` registry.cppm:135-159 |
| xim 包名(`xim-x-<name>`) | `gcc`、`musl-gcc`、`mingw-cross-gcc`、`llvm` | `to_xim_package` registry.cppm:161-207;目录形成 xlings.cppm:553-559 |
| 显示 label | `gcc 15.1.0-musl`、`mingw-cross 16.1.0` | `display_label` registry.cppm:240-248 |

`display_label` 是从 xim 名**反推**用户名的有损映射;`matches_default_toolchain`
(registry.cppm:250-274)已积累四条特例(musl/msvc/mingw/mingw-cross)。每加一个变体 =
`to_xim_package` + `display_label` + `matches_default_toolchain` + `available_toolchain_indexes` +
`frontend_candidates_for` 五处 if——**线性增长的特例链,是命名模型错位的实现代价**。

### 2.2 四个平行 triple 解析器,词汇表已分叉

| 解析器 | 位置 | os 词汇 | 备注 |
|---|---|---|---|
| `cfgpred::context_for` | prepare.cppm:62-87 | **`macos`** | substring 匹配;`env=gnu\|musl\|msvc` |
| `abi_profile` | abi.cppm:67-96 | **`darwin`** | 同一概念、不同拼写;prepare.cppm:59-61 注释声称「vocabulary is consistent」——os 维恰恰不一致 |
| `model.cppm` 谓词 | model.cppm:99-110 | — | `is_musl_target`/`is_msvc_target`/`is_mingw_target` 独立 substring |
| registry musl 三信号 | registry.cppm:155-157 | — | `toolchain_frontend(binDir, compiler)` 重载(:219)只判其一,漏两信号、不传 triple——同一规则两处实现已经分叉 |

目前潜伏(无代码互比两个 os 值),但谁把 `cfg(os="macos")` 接到 abi 约束上就会静默匹配失败。

### 2.3 `--target` 是未校验自由文本

- 无合法 triple 表、无校验;只有两条硬编码约定:`*-musl` 后缀(prepare.cppm:651-662,host-aware
  分 native/cross)与精确 `x86_64-w64-mingw32`(prepare.cppm:673-679)。
- **打错字 `--target x86_64-linux-mus` 不报错,静默 fall through 到宿主默认工具链**——用户以为在交叉,
  实际编了本地。这是最坏的失败模式。
- 对照:`[package] platforms` **有**固定词汇表校验(prepare.cppm:607-617,`--strict` 硬错)。
  同一个 target 系统,一头严一头全放。
- 命名冲突:`mcpp run <target>` 的位置参数是**二进制名**(cli.cppm:244),与 triple 无关。

### 2.4 显示层缺陷

- **排序 bug**:lifecycle.cppm:322-329 版本字典序降序,注释自认「single-digit segments」前提——
  GCC 11/13/15/16 全是两位主版本,实际输出 `9.4.0` 排在 `15.1.0` 之前。
- **分组割裂**:排序主键是内部编译器名(`gcc` < `llvm` < `musl-gcc`),显示却是 label,
  `gcc X-musl` 行被 `llvm` 劈开。
- **可发现性缺口**:`aarch64-linux-musl-gcc` 装得上(parse/to_xim_package 全支持)但
  `available_toolchain_indexes` 不列它——用户只能读 CI yml 才知道存在。

### 2.5 版本 pin 散落

`16.1.0` 在 prepare.cppm:659,661,676;默认三元组在 :813-818;help/错误串里还有
cli.cppm:346-347、lifecycle.cppm:472、config.cppm:652、doctor.cppm:149。升默认版本要改七八处,
文档漂移(docs/03 说 Linux 默认 `gcc@15.1.0-musl`,代码是 x86_64→`gcc@16.1.0` glibc)是必然结果。
README 平台表(README.md:234-241)MSVC 仍标 planned、mingw 两列整体缺失——表的维度(OS×编译器)
装不下 host≠target。

### 2.6 已经对的部分(方案必须保住)

| 资产 | 位置 |
|---|---|
| 输出目录按 triple 键控 `target/<triple>/<fp>` | prepare.cppm:161-167 |
| cfg() 按 **resolved target** 求值(非 host) | prepare.cppm:51-54,687-705 |
| PE 链接模型按 target(`is_mingw_target`) | linkmodel.cppm:231-236、model.cppm:107-110 |
| `[target.linux]` bare-alias 糖(alias 无 dash,与 triple 无歧义) | prepare.cppm:142-157 |
| `msvc@system` detection-first、pin-verify(`msvc@19.44`) | lifecycle.cppm:476-495 |
| 平台能力常量(`supports_full_static` 等)替代 #ifdef | platform/common.cppm:110-112 |
| arch 扩展是数据不是代码(loader triple 表 + glob 兜底) | linkmodel.cppm:139-186 |
| `aarch64` 为唯一 arch 拼写(GNU triple 系,可与 triple 拼接;`arm64` 属 xlings 资产命名空间) | common.cppm:76-92 |
| static 不进 triple:`*-musl`/mingw32 target **默认** `linkage=static`,`[build]` 可翻转 | prepare.cppm:663-666,677-678 |

最后一条与业界完全一致(Rust `crt-static` 是 target 默认属性 + flag 翻转,static 普遍不进 triple)
——本方案不动它。

---

## 3. 行业模型对照(调研摘要,详见调研记录)

| 生态 | toolchain 身份 | target 身份 | cross 表达 | 变体位置 |
|---|---|---|---|---|
| Rust (rustup+cargo) | `channel-<HOST-triple>`(名字里的 triple 是 **host**) | 4 段 LLVM triple,tier 分级 | `rustup target add` + `cargo build --target`;**全生态零 "cross" 命名** | triple 第 4 段(`-gnu/-musl/-msvc`);static=target 默认属性+flag |
| Zig | 无(编译器即全部,零安装 target) | **3 段 `arch-os-abi`(砍掉 vendor)**;glibc 版本点号后缀 `-gnu.2.17` | `-target` 一个参数,native/cross 零差别 | abi 段 |
| Go | 无 | `GOOS`/`GOARCH` 二元组 | 改环境变量;cgo 一介入即退化回 GCC 世界 | 无 libc 维(被消灭);arch 子变体=另一个环境变量 |
| Clang | 一个多 target 编译器;`<triple>-clang` 调用名→隐式 `--target` | LLVM Triple(所有 triple 的事实源头) | `--target=` 只切 codegen;sysroot 靠 `.cfg` 事后捆绑 | triple env 段 + cfg 两层(公认模糊地带) |
| GCC 世界 | **一条链=一个 target**,链名=triple(`x86_64-w64-mingw32-g++`) | GNU triple | 用户必须感知(换整套前缀命令);"cross compiler"=host≠target 描述词 | triple + **溢出到打包层**(MSYS2 `mingw-w64-ucrt-*`) |
| xmake | 具名 toolchain 注册表(含一个叫 `cross` 的通用链) | `--plat` × `--arch` 两参数;**mingw 被提升为独立 plat** | 切 plat | plat 之别(windows vs mingw)+ SDK 选择,分裂 |
| CMake | toolchain file(自由脚本) | **无身份**——不可枚举/比较/分发,公认痛点之首 | file 内隐式(`CMAKE_SYSTEM_NAME` 即 cross) | file 内任意变量 |
| Swift SDK | 编译器不变,per-target 装 artifact bundle | triple(`x86_64-swift-linux-musl`) | `swift sdk install` + `--swift-sdk <triple>` | musl 进 triple;static 进 SDK 产品名 |
| Bazel | toolchain 声明 exec/target 兼容性 | platform=constraint 集合(可扩维,无全球命名) | `--platforms=` 触发自动 resolution | 自定义 constraint |
| Nix | pkgsCross 属性集 | **规范身份=triple,人机接口=短别名**(`pkgsCross.mingwW64` → `config="x86_64-w64-mingw32"`) | buildPlatform/hostPlatform/targetPlatform 三平台显式建模 | triple + 别名层 |

**收敛点(设计公理的来源)**:
1. target 规范身份 = triple(不用的只有 Go 二元组——cgo 一来就装不下 libc 维;和 CMake——反面教材)。
2. **"cross" 不是 target/toolchain 命名成分,是 host≠target 的相对状态**;例外全在分发/打包层
   (musl.cc `-cross` 后缀、Debian 包名、第三方 cross-rs)。
3. 变体正典位置 = triple env/abi 段;**static 普遍不进 triple**(target 默认属性 + flag 翻转)。
4. toolchain 与 target 正交;multi-target 编译器使「装 target」退化为「装 std/sysroot」
   (rustup target add / swift sdk install);单 target 编译器(GCC)才被迫把 target 烙进链名——
   **mcpp 用 GCC 系工具链但不必继承 GCC 的命名宿命:管理层完全可以呈现 rustup 模型,
   「per-target 装的是整条 GCC 链」只是 acquisition 的实现细节**(重量级版的 rust-std 下载)。
5. CLI 收敛为单参数选 target、默认=host,用户不感知 host。
6. 分歧点里对 mcpp 有立场的:vendor 段——新设计趋势是砍(Zig 3 段);mingw 转正为一等平台
   (xmake)vs 普通 target(Rust)——**取 Rust 侧**,转正污染正交性。

---

## 4. 统一模型

### 4.1 公理

- **A1** target 身份 = mcpp 自有 triple 语言 **`arch-os[-env]`**(Zig 式三段,砍 vendor)。
  mcpp 已在用 `x86_64-linux-musl`(三段、无 vendor)——这是**扩展既有约定,不是换语言**。
  prepare.cppm:607-608 已声明「mcpp owns the target/triple system」,本方案兑现它。
- **A2** toolchain 身份 = **family@version**,family ∈ {gcc, llvm, msvc},
  version ∈ semver | `system`。source(managed-xim / system-detected)由 (family,version) 派生,
  不进名字(`msvc@system` 既有形态保持)。
- **A3** **cross 是关系不是名字**:host≠target 由 (host, target) 二元组派生,仅允许出现在
  ① xim 分发包名(`mingw-cross-gcc`——保持不动)② 显示层的状态注记(如 `(cross)`)。
  用户 spec、manifest、CLI 参数中不得出现。
- **A4** 变体 = triple env 段:`gnu | musl | msvc`。static **不是**变体,是 target 的默认链接属性
  (现状已对,见 §2.6)。
- **A5** 人机别名层允许存在(Nix 先例:规范身份=triple、接口=短别名),但一律 normalize 到
  canonical 并单行提示,别名永久可解析(不搞 breaking 废弃)。

### 4.2 target 封闭词汇表(数据,不是代码)

| canonical triple | env | 状态(tier) | 别名(永久接受) | payload 来源(§4.3) |
|---|---|---|---|---|
| `x86_64-linux-gnu` | gnu | **verified**(ci-linux) | `x86_64-linux`(裸 os 省略 env→host 默认) | xim:gcc / xim:llvm |
| `x86_64-linux-musl` | musl | **verified**(ci-linux、release) | — | xim:musl-gcc |
| `aarch64-linux-musl` | musl | **verified**(cross-build-test qemu、ci-aarch64) | — | xim:musl-gcc(host=aarch64)/ xim:aarch64-linux-musl-gcc(cross) |
| `x86_64-windows-gnu` | gnu(MSVCRT) | **verified**(102_e2e wine、cross-build-test) | **`x86_64-w64-mingw32`**(GNU 正典拼写,0.0.92 已发布,永久别名) | xim:mingw-gcc(host=win)/ xim:mingw-cross-gcc(host=linux) |
| `x86_64-windows-msvc` | msvc | **verified**(ci-windows msvc 原生) | — | system(vswhere 探测) |
| `aarch64-macos` | — | **verified**(ci-macos) | `arm64-apple-darwin` 系拼写 | xim:llvm |
| `riscv64-linux-musl` | musl | planned(等 xim 包,loader 表已备 linkmodel.cppm:145) | — | xim:riscv64-linux-musl-gcc |
| `aarch64-linux-gnu` | gnu | planned(glibc gcc 未发布 aarch64) | — | — |
| `x86_64-macos` | — | planned | — | — |

tier 语义借 Rust:**verified** = CI 端到端真跑(编译+执行,qemu/wine 也算);
**published** = 包已发未进 CI;**planned** = 登记未接。词汇表是 `triple.cppm` 里的一张
constexpr 表 + 每行的 e2e 引用注释——README 平台表从这张表**生成或人工对照**,根治 §2.5 的漂移。

**windows-gnu 的 canonical 拼写决策(D1)**:取 mcpp 自有 `x86_64-windows-gnu`,
`x86_64-w64-mingw32` 降为永久别名。理由:①与既有 `x86_64-linux-musl` 三段式一致(GNU 拼写
`w64` vendor 段 + os 段叫 `mingw32` 是历史残留——64 位也叫 mingw32);②cfg 谓词
`os="windows", env="gnu"` 与 triple 字面直接对应,substring 匹配的两个特判(`mingw`→windows,
prepare.cppm:69;`mingw32` 判 PE,model.cppm:107-110)变成结构化字段读取;③Zig 同拼写先例
(`x86_64-windows-gnu`)。**代价**:`target/` 输出目录名随 canonical 变(S3 迁移,别名输入
normalize 后仍进同一目录);`Toolchain::targetTriple`(dumpmachine 报告的
`x86_64-w64-mingw32`)保持为**独立内部字段**——用户 Target 与编译器自报 triple 本就是两个概念
(Zig 的 -target vs LLVM triple 同构)。

### 4.3 ToolchainSpec v2 + payload 映射数据表

```cpp
struct ToolchainSpec {                    // registry.cppm 重构
    Family      family;    // gcc | llvm | msvc(枚举,不再是自由串)
    std::string version;   // "16.1.0" | "system";partial "16" 保持现有 resolve
    Triple      target;    // 空 = host;唯一的变体载体(A4)
};
// 删除:isMusl(bool 硬编一个变体)、compiler 自由串
```

**payload 映射 = 一张数据表**(family, target.env, target.arch, host)→ xim 包名 + 前端候选:

| family | target | host | xim 包(**分发层,"cross" 在此合法,零改名**) | 前端 |
|---|---|---|---|---|
| gcc | x86_64-linux-gnu | linux-x86_64 | `gcc` | `g++` |
| gcc | \*-linux-musl(arch==host) | linux | `musl-gcc` | `<triple>-g++` |
| gcc | \*-linux-musl(arch≠host) | linux | `<gnu-triple>-gcc`(如 aarch64-linux-musl-gcc) | `<triple>-g++` |
| gcc | x86_64-windows-gnu | **windows** | `mingw-gcc`(winlibs UCRT) | `g++.exe` |
| gcc | x86_64-windows-gnu | **linux** | `mingw-cross-gcc`(MSVCRT 交叉) | `x86_64-w64-mingw32-g++` |
| llvm | host | any | `llvm` | `clang++` |
| msvc | x86_64-windows-msvc | windows | —(system 探测,vswhere 链) | `cl.exe` |

这张表替换掉五处 if 链(§2.1):`to_xim_package`/`frontend_candidates_for` 查表;
`display_label`/`matches_default_toolchain` **整体删除**——显示从 Spec 结构渲染
(`gcc 16.1.0 → x86_64-windows-gnu`),default 匹配变成 Spec 相等比较。
`available_toolchain_indexes` 的 host `if constexpr` 分流(registry.cppm:287-292)也由表的
host 列取代——同一个 target 行,host 不同只是取不同包,**用户看到的名字不变**。

注意表里 winlibs=UCRT、交叉=MSVCRT:两条 CRT 目前都折叠在 `x86_64-windows-gnu`。
abi.cppm:76 现状 mingw→`msvcrt` 单值,与 0.0.92 选型一致;UCRT 细分若未来需要,
按 Rust 先例另立 env(`gnullvm` 式)或 abi 维度细化,登记为 follow-up,不在本期。

### 4.4 单一 `triple.cppm`(src/toolchain/ 或 src/target/)

```cpp
struct Triple {
    std::string arch;   // x86_64 | aarch64 | riscv64 | ...(GNU 拼写,永不 arm64)
    std::string os;     // linux | macos | windows(darwin 在 parse 时归一为 macos)
    std::string env;    // gnu | musl | msvc | ""(macos)
    // 派生只读:family() → unix|windows;is_pe();is_musl();is_msvc_env();is_host();
};
Triple parse(std::string_view);            // 接受别名(w64-mingw32、darwin、apple)→ canonical
std::optional<Triple> validate(...);       // 对 §4.2 封闭表;附 did-you-mean
```

四个现有解析器全部改为消费者:

| 消费者 | 现状 | 改后 |
|---|---|---|
| `cfgpred::context_for`(prepare.cppm:62-87) | substring 自解析 | 读 Triple 字段;cfg 词汇 `{os,arch,family,env}` 值域=Triple 值域,文档化 |
| `abi_profile`(abi.cppm:81-83) | os 吐 `darwin` | 吐 `macos`——**词汇分叉在源头消灭**;abi 五维结构保留 |
| `model.cppm:99-110` 谓词 | 独立 substring | `Triple` 派生方法的薄别名(或直接删,call site 改) |
| registry musl 三信号(:155-157)+ `toolchain_frontend`(:219)漏判 | 两处实现已分叉 | Spec v2 后自然消失(env 是唯一信号源) |

### 4.5 `--target` 校验

- 输入先 `triple::parse`(别名归一)再 `validate` 对封闭表。
- **逃生舱**:manifest 存在显式 `[target.<triple>]` 节的未知 triple 放行(自带 toolchain override
  的高级用法不被词汇表卡死)——否则**硬错** + did-you-mean(编辑距离):
  `unknown target 'x86_64-linux-mus' — did you mean 'x86_64-linux-musl'?`。
- `[target.linux]` bare-alias 糖不受影响(alias 无 dash,原有歧义消解规则保持,prepare.cppm:148-157)。
- 两条硬编码约定(`*-musl`、mingw32,prepare.cppm:651-679)改为查 §4.2 表的
  「target → 默认 toolchain pin」列,顺带完成 §2.5 的 pin 集中(见 §4.6)。

### 4.6 版本 pin 集中

新增单一常量表(如 `src/toolchain/pins.cppm` 或并入 triple 表):
first-run 默认三元组(prepare.cppm:813-818)、musl/mingw 约定 pin(16.1.0)、llvm 默认(20.1.7)。
help/错误串(cli.cppm:346-347 等)一律 `std::format` 引用。docs 侧在表旁注释「改此处须同步
docs/03、README 表」。

### 4.7 `compat.cppm` — 兼容层独立成模块(决策新增)

所有旧拼写的**识别与归一**收进单一 `src/toolchain/compat.cppm`,核心代码只见 canonical 形:

```cpp
export module mcpp.toolchain.compat;
// 职责(唯一知道旧拼写的文件;整体删除 = 只断旧输入,不断任何 canonical 路径):
//  1. spec 别名:musl-gcc@V / gcc@V-musl / <gnu-triple>-gcc@V / mingw@V / mingw-cross@V / clang@V
//     → 归一为 ToolchainSpec v2{family, version, target}(§6.2 表的实现载体)
//  2. triple 别名:x86_64-w64-mingw32 / darwin / apple 系拼写 → canonical Triple
//  3. 持久化状态迁移:config 里存量 default 串("gcc@15.1.0-musl"、"mingw-cross@16.1.0"、
//     manifest [toolchain] 旧值)在**读取路径**归一(可选:写回时重写为 canonical)
//  4. 归一提示:一次性单行 note(§5.2),提示文案只在此处
export std::optional<NormalizedSpec>  normalize_spec(std::string_view);   // 非别名→nullopt(走 canonical 解析)
export std::optional<Triple>          normalize_triple(std::string_view);
```

**依赖方向**:compat → 核心类型(Triple/ToolchainSpec);核心只在两个公共解析入口
(`parse_toolchain_spec`、`triple::parse`)的第一行调 `compat::normalize_*`,其余核心代码
零 compat 依赖。**xim 包名映射不在 compat**——`mingw-cross-gcc` 等是分发层现役身份(§4.3 数据表),
不是遗留拼写。命名说明:模块职责单一(legacy-spelling compatibility),
不是口袋模块([[naming-no-grab-bag-modules]] 约束仍满足)。

---

## 5. 显示/输出规范

### 5.1 `mcpp toolchain list` 重排(rustup 两轴模型)

```
Toolchains:
  *  gcc 16.1.0                (default)
     gcc 15.1.0
     llvm 22.1.8
System:
     msvc 19.44 (VS 2022 BuildTools)          [Windows host 才有此块,现状保持]

Targets:                                       TOOLCHAIN         STATUS
  *  x86_64-linux-gnu     (host)               gcc 16.1.0        installed
     x86_64-linux-musl    static               gcc 16.1.0        installed
     x86_64-windows-gnu   PE, cross            gcc 16.1.0        installed
     aarch64-linux-musl   static, cross        gcc 16.1.0        available
     riscv64-linux-musl   static, cross        —                 planned

Available toolchains (run `mcpp toolchain install <family> <version>`):
     gcc 15.1.0 / 13.3.0 / 11.5.0 / 9.4.0
     llvm 20.1.7
```

要点:
- **toolchain 块只剩 family@version**——`gcc 15.1.0-musl`、`mingw-cross 16.1.0` 这类行消失,
  它们变成 Targets 块的行(「变体是 target」的显示兑现)。
- `cross` 只出现在 STATUS 注记(A3 允许的第二处)——描述当前 host 关系,不是身份。
- **排序修复**:版本按数值 semver 降序(修 lifecycle.cppm:326 字典序 bug);
  family 分组(修 `gcc-musl` 被 `llvm` 劈开)。此两条**独立于大方案,S1 先行**。
- planned 行的展示给 target 词汇表一个用户可见出口(修 §2.4 可发现性缺口——
  `aarch64-linux-musl` 不再只活在 CI yml 里)。
- 路径缩写 `@mcpp/` 保持。

### 5.2 消息拼写规则

- 一切输出中 toolchain 用 **`family@version`**(`Resolved gcc@16.1.0 → x86_64-windows-gnu`);
  空格形式(`gcc 16`)只作输入接受。
- 别名 normalize 时单行提示一次:
  `note: 'mingw-cross@16.1.0' is now 'gcc@16.1.0' targeting 'x86_64-windows-gnu'`——
  提示不是 warning,不入 stderr 告警级。

---

## 6. CLI / 使用规范

### 6.1 单名词 CLI:`mcpp toolchain` + `--target` 选项(D4 二次 review 改定)

**不引入 `mcpp target` 子命令**。rustup 的 target/toolchain 双名词有个 mcpp 不具备的前提:
rustc 是多 target 编译器,`rustup target add` 只下载 rust-std、编译器本体不动——两个名词对应
两种物理实体。mcpp 是 GCC 世界,**每个 (family, target) 组合 = 装一整条链**,「加 target」和
「装 toolchain」底下是同一个动作;两个名词、一种实体才是概念冗余。且 mcpp 已有 build 时
autoInstall——交叉的主 UX 本来就是 Zig 式零仪式:

```bash
mcpp build --target x86_64-windows-gnu    # 主路径:缺链自动装,一条命令
```

显式命令面(低频:CI 预热/离线准备/切默认):

```bash
mcpp toolchain install gcc 16                                # host target(不变)
mcpp toolchain install gcc 16 --target x86_64-windows-gnu    # 装该 target 的链
mcpp toolchain install --target aarch64-linux-musl           # 省略 family → 词汇表 pin(gcc)
mcpp toolchain default gcc@16 --target x86_64-linux-musl     # 默认=(family@ver, target) 一对;
                                                             #   --target 省略 = host
mcpp toolchain remove gcc@16 --target x86_64-windows-gnu
mcpp toolchain list                                          # §5.1 两块式,Targets 块即词汇表出口
```

**顺带收益**:`mcpp run <target>`(二进制名,cli.cppm:244)与顶级 target 名词的冲突自然消失,
help 文案无需改动。词汇表的发现职责由 `toolchain list` 的 Targets 块(含 planned 行)+
README 生成表承担,无损失。

### 6.2 兼容别名(永久,查表解析;实现载体 = §4.7 `compat.cppm`)

| 旧拼写 | normalize 到 |
|---|---|
| `musl-gcc@V` / `gcc@V-musl` | `gcc@V` + target `<host-arch>-linux-musl` |
| `<gnu-triple>-gcc@V`(如 aarch64-linux-musl-gcc) | `gcc@V` + target 对应 triple |
| `mingw@V`(Windows host) | `gcc@V` + target `x86_64-windows-gnu` |
| `mingw-cross@V`(Linux host) | 同上(host 只影响 payload 选取,§4.3 表) |
| `--target x86_64-w64-mingw32` | `--target x86_64-windows-gnu` |
| `clang@V` | `llvm@V`(现状已有,保持) |

别名解析**不设废弃时限**(解析成本一张表,breaking 无收益);§5.2 的单行提示随本 PR 上线(compat 内一处文案)。

### 6.3 manifest 面

```toml
[toolchain]                 # 值域收紧为 family@version(+ msvc@system / msvc@<prefix> pin-verify)
linux   = "gcc@16"
windows = "gcc@16"          # 旧值 "mingw@16.1.0"/"msvc@system" 永久兼容(6.2 表)

[build]
target = "x86_64-linux-musl"   # 新增:默认构建 target(≙ cargo build.target)。
                               # 「默认就要 musl 静态」从 toolchain 名字挪到这里——语义归位:
                               # 全静态是产物属性,不是编译器家族属性
```

`[target.<triple>]` override、`[target.'cfg(...)'.build]`、bare-alias 糖全部不变。

---

## 7. 单 PR 实施计划(0.0.93;沿用 `single-pr-090/091-implementation-plan` 惯例)

**决策(D8 定案)**:不分期,整套一个 PR。理由:①分期的中间态(S2「校验用新表、拼写还是旧 canonical」)
本身是额外的一致性负担;②改动集中在 registry/prepare/lifecycle 三文件 + 两个新模块,分期会让
同一批行被改两遍;③repo 已有 0.0.90(MSVC 原生)、0.0.91(c++fly)单 PR 落大特性的先例与节奏。
**风险对冲**:PR 内按下列八步组织 commit,每步独立编译绿 + 单测绿;e2e 全量门禁收口;
第 1、2 步是纯新增(先立地基),破坏性重写集中在第 3-5 步。

| # | 步骤 | 内容 | 验证 |
|---|---|---|---|
| 1 | `triple.cppm`(纯新增) | Triple 类型 + parse/validate + §4.2 封闭词汇表 + §4.6 pin 集中表 | 单测:parse/canonical/别名矩阵、did-you-mean |
| 2 | `compat.cppm`(纯新增) | §4.7 全部旧拼写归一 + 提示文案 | 单测:§6.2 别名表全行 round-trip;存量 config 串归一 |
| 3 | ToolchainSpec v2 + payload 数据表 | registry.cppm 重写:删 `isMusl`/自由串 compiler、删 `display_label`/`matches_default_toolchain` if 链、`available_toolchain_indexes` host-constexpr 分流并入表;两个解析入口首行接 compat | 单测:spec 相等比较替代 default 匹配;既有 33 单测迁移 |
| 4 | 四解析器改 Triple 消费者 | cfgpred(prepare.cppm:62-87)、abi `darwin`→`macos`(abi.cppm:81-83)、model.cppm 谓词薄化、registry 三信号删除(随步 3);canonical 即切 `x86_64-windows-gnu`(target/ 目录名随之;compat 归一保证 `--target x86_64-w64-mingw32` 进同一目录) | e2e 85/91(cfg/bare-alias)全绿;abi e2e 全绿 |
| 5 | `--target` 校验 + 约定表化 | validate + did-you-mean + `[target.X]` 逃生舱;prepare.cppm:651-679 两条硬编码约定改查 §4.2 表 | 新增 e2e:typo 硬错 + did-you-mean;28(musl)、102(wine,**双拼写各跑一遍**)全绿 |
| 6 | CLI/显示 | `toolchain install/default/remove` 接 `--target` 选项(§6.1,单名词面);list 两轴重排 + 数值 semver 排序(§5.1);`[build] target` | 新增 e2e:`install --target`→build 闭环、`build --target` autoInstall 闭环、list 输出断言(排序/分组/planned 行) |
| 7 | 文档 | README 平台表从 §4.2 表重画(补 MSVC=✅、windows-gnu 行、host≠target 维);docs/03 三处漂移(Linux 默认、mingw-cross→target 章节改写)+ docs/zh 同步;CHANGELOG | 文档 review;prepare.cppm:643 注释顺改 |
| 8 | 全量收口 | e2e run_all(基线 102+新增);ci-linux/macos/windows/aarch64/cross-build-test 五工作流全绿;`95-97/99`(msvc/mingw)兼容别名路径断言 | PR 门禁 |

**PR 外的 follow-up(不阻塞)**:UCRT env 细分、riscv64 行转 verified、clang cross
(`-target`+sysroot 注入)——三者都只是往 §4.2/§4.3 的表里**加行**,这是本方案的可扩展性验收标准。
别名单行提示(§5.2 note)随本 PR 直接上线(compat 内一处文案)。

---

## 8. 决策清单(2026-07-15 review 已定案)

| # | 决策 | 定案 | 当时的备选及代价(存档) |
|---|---|---|---|
| D1 | windows-gnu canonical 拼写 | ✅ **`x86_64-windows-gnu`**(Zig 式,与 linux-musl 三段一致);`x86_64-w64-mingw32` 永久别名 | 保持 GNU 拼写为 canonical:少一次 target/ 目录迁移,但 cfg 词汇与 triple 字面永久错位、vendor 段特例永存 |
| D2 | `mingw`/`mingw-cross` 降级为别名,family 只剩 gcc/llvm/msvc | ✅ **是**(§1 三重错位的根治) | 保留 mingw 为 family(xmake 路线):高频路径少打一个 --target,但正交性破坏、§2.1 特例链保留 |
| D3 | musl 出 toolchain 名、入 target;「默认 musl」= `[build] target` | ✅ 按推荐 | 保留 `-musl` 版本后缀为 canonical:改动小,但同轴四编码之一转正、其余三个仍需特判 |
| D4 | CLI 名词面 | ✅ **单 `toolchain` + `--target` 选项**(2026-07-15 二次 review 改定,§6.1):mcpp 里「加 target=装整条链」,双名词是概念冗余;主 UX 是 `build --target` autoInstall(Zig 式零仪式);`run <target>` 冲突顺带消失 | 原推荐 rustup 式 `mcpp target add/list/default`:业界心智模型贴合,但其前提(target=只装 std、编译器不动)mcpp 不具备 |
| D5 | 词汇表:os 统一 `macos`(abi 的 darwin 消灭)、arch 保持 `aarch64` | ✅ 按推荐 | — |
| D6 | `--target` 硬校验 + `[target.X]` 逃生舱 | ✅ 按推荐(静默编错是最坏失败模式) | 仅 warning:不破坏任何脚本,但错误继续静默 |
| D7 | tier 标注(verified/published/planned)进 `toolchain list` Targets 块与 README | ✅ 按推荐(Rust tier 先例;README 从表生成对照,根治漂移) | 不标:表更短,但 planned target 继续只活在 CI 注释里 |
| D8 | 落地节奏 | ✅ **不分期,整套单 PR**(§7;canonical 随 PR 一次切换) | 原推荐两期(S2 别名+校验、S3 切拼写):分摊风险,但中间态本身是一致性负担、同批代码改两遍 |
| D9 | 兼容层位置 | ✅ **独立 `src/toolchain/compat.cppm`**(§4.7;核心只见 canonical) | 散在 registry/triple 各解析点:少一个模块,但旧拼写知识扩散、无法整体衡量/删除 |

## 9. 非目标

- **不改 xim 分发包名**(`mingw-cross-gcc`/`musl-gcc`/`aarch64-linux-musl-gcc` 保持)——
  分发层的 "cross"/triple 命名与业界先例一致(musl.cc、Debian),且改名牵动 xlings-res/xim-pkgindex
  发布链,零收益。
- 不引入 vendor 段(Zig 论据:实践证明无信息量)。
- 不做 clang cross(sysroot 注入)——cross-build-test.yml 已登记 planned,与本方案正交,
  落地时只是 §4.3 表加行。
- 不动 static 语义(target 默认属性 + `[build]` 翻转,现状即业界正解)。
- 不做 UCRT/MSVCRT env 细分(登记 follow-up,§4.3)。
- `msvc@system` 模型不动(detection-first 是对的;它已经天然符合 A2——family=msvc、
  version=system、target=x86_64-windows-msvc)。
