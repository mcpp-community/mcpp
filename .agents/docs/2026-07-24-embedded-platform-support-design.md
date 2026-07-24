# 嵌入式平台支持 — 方案设计 (Embedded Platform Support)

- Date: 2026-07-24
- Status: 设计定调(维护者决策已锁),待实施
- 关联: [issue #276](https://github.com/mcpp-community/mcpp/issues/276) — feat/RFC: 支持 Buildroot/OpenWrt/Yocto 等嵌入式 Linux SDK 的工程化集成
- 结构:**背景与定位 → 决策点(为什么,前置)→ 工作清单 → 详情(现状核实/可行性,后置)**

---

## 0. TL;DR

mcpp 进入嵌入式 Linux 生态的方案:**mcpp 自带匹配-libc 的交叉编译器(最新 GCC),消费开发者提供的外部 sysroot;不做发行版构建器、不逐个适配厂商 SDK。**

- 核心只需开一个 **sysroot 缝**(机制已在),其余(pkg-config、qemu test-run)是 xlings provisioning + 薄接线。
- 首发滩头 = **树莓派 aarch64**。
- 这个架构把 #276 最难的「厂商旧编译器 vs C++23」问题用**「根本不用旧编译器」**直接溶掉——mcpp 永远用自己的现代编译器,设备只提供库。

---

## 1. 背景与现状(为什么需要这个方案)

### 1.1 issue #276 要什么

一份 RFC,讨论 mcpp 对 hosted 嵌入式 Linux(Buildroot / OpenWrt / Yocto / uClibc·musl·glibc + 厂商定制工具链)的定位。三个核心问题:

1. 厂商 SDK 锁死旧编译器(如 GCC 8.3、uClibc 绑定)与 mcpp 要 C++23 的矛盾;
2. 与 Buildroot/Yocto/OpenWrt SDK 的集成方式(外部 `TARGET_CC`、staging sysroot、作为 build backend);
3. 第三方库管理与**动态链接**(如 ALSA:必须用设备那份、带插件/配置/ABI 耦合)。

诉求一句话:mcpp 能否从「自成体系、自管依赖、hermetic 静态倾向」转向能「消费外部 SDK 的交叉工具链 + 外部 sysroot + 外部第三方库」。

### 1.2 mcpp 今天的模型 vs 嵌入式的需要

| | mcpp 今天 | 嵌入式需要 |
|---|---|---|
| 工具链 | 自带、download-managed | 现代交叉编译器(可自带) |
| sysroot | 仅内部(自家 payload) | 消费**外部**设备 rootfs |
| 第三方库 | 索引下载、源码构建 | 消费设备已有(动态、pkg-config) |
| 链接 | hermetic(拒绝宿主库) | 需对着外部 sysroot 链 |

**gap = 缺入口(通用缝),不是缺能力**:`CLibMode::Sysroot` 已能发 `--sysroot`、hermetic 白名单已含 `tc.sysroot`——只差把外部路径喂进去的配置通道。

---

## 2. 定位与职责边界

mcpp **不**做发行版构建器(≠ Yocto/Buildroot),**不**逐个适配厂商 SDK。它是「构建现代 C++23-modules 应用的最佳工具」。职责四分:

```
mcpp 拥有:      编译器 + C++ 构建/包模型 + 通用交叉缝(target / sysroot / pkg-config)
库作者/社区拥有: 把每个库从源码打包给 mcpp(compat.*),一次,通吃所有目标
平台/厂商拥有:   设备 rootfs/sysroot(含专有 + 运行期耦合库)
mcpp 永不拥有:   逐 SDK 适配、变成发行版构建器
```

---

## 3. 决策点 + 理由(核心,前置)

维护者(sunrisepeak)在设计评审中锁定的定调。**每条都附「为什么」。**

| # | 决策 | 为什么 |
|---|---|---|
| 1 | mcpp = C++ app 构建工具,不做发行版构建器/不逐 SDK 适配 | 造整个平台是 Yocto/Buildroot 的活(几千 recipe + BSP + 十年生命周期),且平台 99% 是 C 而非 C++23-modules,mcpp 的模块图/import std 优势完全用不上;战略应聚焦本职 |
| 2 | **架构 = config②**:mcpp 带(匹配 libc 的)交叉编译器 + 消费外部 sysroot | ABI 沿缝解耦——`--sysroot` 管设备 libc、静态 libstdc++ 自包含 C++ 运行期、C-ABI 管第三方;让 mcpp 用自己的现代编译器,设备只提供库;把 C++23 门槛搬到 mcpp 掌控的编译器一侧 |
| 3 | 滩头 = **树莓派 aarch64**(后续 Jetson…) | 三轴全占最优(mainline ISA + hosted + 浅锁 + glibc);出货最大、社区最广、mcpp 已有 aarch64 交叉基建;打通几乎白送 Jetson(同为 Ubuntu/aarch64/glibc) |
| 4 | 核心单点特性 = **开 sysroot 缝(L2)** | 90/10 的一刀,解锁标准「对着 rootfs 交叉」工作流(所有嵌入式 Linux C++ 开发的通用姿势);机制已在,最小改动 |
| 5 | **C++ 只源码;二进制只 C-ABI + 封装层** | C++ 二进制 ABI 跨编译器/版本天生不安全(名字修饰、libstdc++ ABI、vtable、异常);源码编 = ABI 一致;C-ABI 是唯一稳定的二进制互操作契约。拒绝预编译 C++ `.so` = 正确性,非限制 |
| 6 | 绑定质量/类型安全 = **封装作者的领域知识**,非 mcpp 机制 | schema-ownership 原则(mcpp 定机制、领域专家定语义);保持核心小,绑定可经索引一次写、人人用 |
| 7 | 运行期耦合/专有库 = **dlopen** 或有 sysroot 时**直接链** | dlopen 让链接期只碰 libc/libdl(完全 hermetic、无需 sysroot),自包含二进制 + 运行期用设备库;有 sysroot 时直接链更类型安全。绑定(源码)随索引、二进制留设备。`[runtime] dlopen_libs` 钩子已存在 |
| 8 | 编译器:**先 GCC** | GCC 的 import std(`bits/std.cc`)比 clang 实验实现成熟,且 mcpp 已以 GCC 为默认。clang 路线(像 rustc,一个编译器 `--target` 切 libc,少维护 payload)存档待议 |
| 9 | 厂商预编译 **C++-only** 库 = 用户用**厂商 ABI 建 C 桥 `.so`**,mcpp 经 C ABI 消费 | 把 ABI 匹配负担搬进桥(用厂商工具链编、纯 C 边界、catch 住 C++ 异常),mcpp 只见 C ABI,政策 #5 不破——所以是**支持**,非 non-goal |
| 10 | glibc 基线 = **造 payload 时 pin 低 glibc** + 一次实证 | GCC 版本 × glibc 版本**解耦**,可「最新 GCC + 低 glibc」;工具链自带的静态 libstdc++ 的 glibc 基线 `--sysroot` 压不下去,但 **payload 是 mcpp 自己造的** → 造时 pin 低即可。payload 用**最新 GCC(当前 GCC 16.1 = mcpp 默认)**,GCC 15 只是 import-std 能力 floor,不是 payload 版本 |
| 11 | 无 `.pc`:**开发者自解决** | 生产 rootfs 常剥掉 `-dev`(头 + `.pc`);mcpp 有 `.pc` 就用 pkg-config、无则退回手写 `ldflags`(永远可用);mcpp 不造 `.pc` |
| 12 | sysroot 获取/管理/pinning = **开发者的事** | 产品专属 sysroot 是某个产品的指纹,只有「造了产品的东西」能产出;mcpp 消费不制造。可复现 pinning 仅量产需要(而量产后端不做,见 #14) |
| 13 | sysroot 布局:**支持 3 主流覆盖大部分** | ① Debian/Ubuntu multiarch `/usr/lib/<triple>/`(滩头)② 纯 `/usr/lib`+`/lib`(musl/Buildroot/OpenWrt)③ `/usr/lib64`(RPM 系/部分 Yocto);靠工具链自带搜索 + 少量 `-L` 兜底 |
| 14 | **L4 后端模式(被 Yocto/Buildroot 驱动):不做** | 与「自管闭环」价值观张力最大;只在量产为 Yocto/Buildroot recipe 时才需;开发者交叉工作流不需要 |
| 15 | 裸机 / freestanding-modules / ESP32:**出范围 / 推迟** | 需 freestanding modules(无 hosted import std)核心特性;ESP32 三难合一(定制 ISA Xtensa + 裸机 + 专有 IDF),且 512KB RAM MCU 上 C++23-modules 优势无关 |
| 16 | qemu 交叉 test-run + pkg-config = **xlings provisioning 免费 + 薄接线** | xlings 能装 qemu/pkgconf(同装工具链机制),不是新子系统;qemu 覆盖逻辑测试,碰硬件的测试仍需真机(模拟固有边界,非缺陷) |
| ~~17~~ | ~~「编译器不支持 modules/C++23」明确诊断~~ → **取消** | config② 下 target 编译器永远是 mcpp 自带的最新 GCC(16+),该场景**架构上构造不出**;唯一残留(显式 `[toolchain]="system"` 指向旧编译器)已被现有 `hasImportStd` 门 + import-std 诊断(`prepare.cppm:3419`)兜住 |

**元结论**:config②(mcpp 带编译器)把 #276 **核心问题 1(厂商旧编译器 vs C++23)整个溶掉**——靠「根本不用旧编译器」,而非「诊断旧编译器」。对 #276 那句「目标编译器不支持 C++23 时给诊断」的正确答复 = 「不需要,设备的编译器从不被调用」。用架构消解问题,比诊断问题更干净。

---

## 4. 净工作清单

| 优先级 | 项 | 性质 |
|---|---|---|
| **P0** | **sysroot 缝(L2)**:manifest/CLI 字段 → `Toolchain::sysroot` → `CLibMode::Sysroot`(已能发 `--sysroot`)+ hermetic 白名单(已含 `tc.sysroot`);含 3 种布局解析(#13) | 核心,小 |
| **P0** | 匹配 libc 的**最新 GCC(GCC 16)** payload、**pin 低 glibc**(aarch64-linux-gnu 首要,补 armv7/riscv64;glibc+musl 变体)+ **一次 glibc 地板实证** | 分发/数据,非引擎 |
| **P1** | pkg-config(pkgconf via xlings + `PKG_CONFIG_SYSROOT_DIR`/`LIBDIR` sysroot-aware) | 工效,薄 |
| **P1** | qemu 交叉 test-run(qemu-user via xlings + `QEMU_LD_PREFIX=<sysroot>`) | 工作流,薄 |
| 去掉 | L4 后端模式、sysroot pinning/管理、造 `.pc`、modules-不支持诊断 | — |
| 推迟 | freestanding modules(裸机)、clang 路线 | — |

**glibc 地板实证(P0 的唯一真实未知)**:自建 or 采纳 GCC 16 × 低 glibc(如 2.28/2.31)的 aarch64 交叉链,编一个 `import std;` 最小静态程序,在 qemu-user / 真机不同 glibc 版本下验证「import-std 静态产物的 glibc 地板能压多低」。注:Bootlin 2025.08 bleeding-edge 仍是 GCC 15.1,GCC 16 大概率要自建(mcpp 已有 musl-cross/mingw-cross 自建先例)或等 Bootlin 2026.x。该实证决定工具链选型(自建低-glibc GCC16 / 采纳 Bootlin 某档 / musl-static 兜底)。

---

## 5. 关键机制(怎么做)

### 5.1 config②:带编译器 + 消费 sysroot

mcpp 交叉 GCC 16(**匹配 libc**)+ 外部设备 sysroot(`--sysroot`)。运行期这样拆:

| 组件 | 来自 | 方式 |
|---|---|---|
| 机器码 | mcpp 的 GCC 16(ISA 代码生成) | — |
| libstdc++ / `import std` 的 `std.o` | mcpp 的 GCC 16 | **静态嵌入**(`-static-libstdc++` 默认)→ 设备老 libstdc++ 不参与 |
| glibc / CRT / 动态链接器 | **设备 sysroot** | `--sysroot`,匹配设备实际 loader |
| 第三方 C-ABI 库(ALSA 等) | **设备 sysroot** | 动态链接,ABI 与 rootfs 一致 |

### 5.2 sysroot 缝(L2)= 最小插入点

- 已有:`CLibMode::Sysroot`(`linkmodel.cppm`)`link_flags()` 发 `--sysroot=<root>`;hermetic 白名单(`hermetic.cppm:117-130`)已含 `tc.sysroot`。
- 只需:一个 manifest/CLI 字段把外部路径填进 `Toolchain::sysroot` → `resolve_link_model` 自动走 Sysroot 模式 → 链接对着设备 libc + 库,类型安全、hermetic(不用 `allow_host_libs`)。

### 5.3 三条溶解尾巴的政策

1. **C++ 只源码;二进制只 C-ABI + 封装。** 拒绝预编译 C++ `.so` = 正确性。
2. **绑定质量 = 封装作者的活。** mcpp 定机制、作者定语义。
3. **运行期耦合/专有库 = dlopen 或有 sysroot 直接链。** 绑定随索引、二进制留设备。

### 5.4 C++ 封装的两种形态

- **有源码/C-ABI 库**:全局模块片段 `#include` C 头 → `export module xxx` re-export(`generated_files`/`scan_overrides` 已支持)。
- **厂商 C++-only 二进制**:用**厂商 ABI 编译器**建一个纯 C 边界的桥 `.so`(内部调 C++、catch 住异常、只导出 `extern "C"`),mcpp 经 C ABI 消费。

### 5.5 dlopen vs 直接链(何时用哪个)

| | 直接链(需 sysroot) | dlopen |
|---|---|---|
| 编译期类型检查 | ✅(有头) | ✗(手动 dlsym,封装层内隐藏) |
| 链接期依赖 | 需 sysroot 里的头/桩 | **只需 libc/libdl,完全 hermetic** |
| 二进制形态 | 动态链设备库 | 链接期自包含(需动态可执行文件才能 dlopen) |
| 适合 | sysroot 齐全、要类型安全 | 版本韧性、避免构建期依赖设备库 |

注:**全静态可执行文件不能 dlopen**(musl 是失败桩、glibc 不可靠)→ 走 dlopen 需动态可执行文件 + libc 匹配设备 + 静态 libstdc++。

---

## 6. 详情:现状核实与可行性(后置)

### 6.1 硬边界:C++23-modules floor(不可协商,但交叉模式绕开)

mcpp 产物 = C++23 modules + import std + 链接 `std.o`,要求**编译器** GCC ≥ 15 / Clang ≥ 18–19。`hasImportStd` 是**能力门控非版本门控**(`gcc.cppm:66-136`:只看有没有 `bits/std.cc`)。`std.o` 必须由 **target** 工具链的 `bits/std.cc` 编出(`stdmod.cppm`、`gcc.cppm:155-192`)。

→ **交叉模式下 target 编译器是 mcpp 自带的(满足),设备只需兼容运行期、不需现代编译器** → 「厂商 GCC 8.3」被绕开。真正出范围:裸机无 import std(需 freestanding-modules)。

### 6.2 现状核实(四条线,均代码验证)

- **工具链身份**:封闭 `Family{Gcc,Llvm,Msvc}`(`registry.cppm:30`);GCC/LLVM 一律走 `to_xim_package` → xim 下载(`registry.cppm:200-255`);manifest toolchain 值只接受 `family@version` 不接受路径。**唯一 detection-first「系统工具链」是 `msvc@system`**(`is_system_toolchain` 硬编码只认 MSVC,`registry.cppm:298-300`);`explicit_compiler` 路径通道已存在(`probe.cppm:234-242`)。
- **sysroot/链接模型**:`CLibMode{None,PayloadFirst,Sysroot}`(`linkmodel.cppm`);`Sysroot` 模式**已能发 `--sysroot`**,但来源写死(`probe_sysroot`:`-print-sysroot`/xlings remap/macOS SDK),**无外部输入通道**;hermetic 白名单**已含 `tc.sysroot`**(`hermetic.cppm:117-130`)。
- **pkg-config**:全仓库(src/docs/README/CHANGELOG)grep 零命中——完全不存在。
- **环境注入**:只 fallback 读 `CXX`(`probe.cppm:246`),不读 `CC/CFLAGS/LDFLAGS/AR/SYSROOT/PKG_CONFIG_*`。
- **C++23 门控**:默认 c++23;`import std` 路径有明确诊断(`prepare.cppm:3419`);modules-无-import-std 透传原始编译器错误。

### 6.3 限制清单(config② 下)

1. **libc 家族必须匹配**(glibc vs musl):`--sysroot` 换路径不换 libc 目标(GCC 编译期烙死);需同时发 glibc/musl 两种交叉工具链。clang 可用 `--target` 切(更像 rustc)。
2. **设备 glibc ≥ 工具链 glibc 基线**:预编译静态 libstdc++/libgcc 的基线 `--sysroot` 压不下去 → 造 payload 时 pin 低(决策 #10)。
3. **保持静态 libstdc++**:动态链设备旧 libstdc++.so 给不了 C++23 符号 → 崩;用默认(静态)。
4. **sysroot 里 C++-ABI 第三方**:名字修饰 + 两份 libstdc++ 隐患 → 需 C 桥(决策 #9);C-ABI 第三方无碍。
5. **运行期库闭包完整性**(libatomic 那课):静态兜底或确保设备有需要的运行期库。

### 6.4 版本图景(哪些真实工具链跨过 import-std floor)

| 工具链 / SDK | GCC | libc | 过 floor |
|---|---|---|---|
| **mcpp 默认** | **16.1** | glibc/musl | ✅ payload 用这个 |
| Bootlin 2025.08 bleeding-edge | 15.1 | glibc 2.41 / uClibc-ng / musl | ✅(但仍 15,GCC16 待自建) |
| ARM GNU 15.2.rel1 | 15.x | newlib/glibc | ✅ |
| Zephyr SDK | 14.3 | picolibc | ✗ |
| Yocto 5.0 Scarthgap(LTS) | 13.2 | glibc 2.39 | ✗ |
| OpenWrt / 厂商老 SDK | 12 / 8.x | musl / uClibc | ✗ |

**结论**:现代 import-std-capable 交叉工具链已存在(Bootlin GCC 15 覆盖三大 libc、ARM GNU 15),但厂商锁死的旧 SDK 仍在硬边界外——所以 config②「自带编译器」而非「消费厂商工具链」是对的。

### 6.5 平台可攻性三轴(为什么树莓派是滩头,ESP32 是远角)

可攻性 = **ISA 是否 mainline × hosted-or-裸机 × 厂商锁深度**。

- **树莓派**:mainline aarch64 + hosted + 浅锁 = 三轴最优 → 滩头。
- **Jetson**:同形状(Ubuntu/aarch64/glibc)→ 几乎白送。
- **ESP32**:定制 ISA(Xtensa,被迫用厂商工具链)+ 裸机(需 freestanding modules)+ 深度专有 IDF(拥有构建/烧录/WiFi blob)= 三轴最差 → 队尾,且 C++23 优势无关。

### 6.6 开放风险 / 需实证的点

- **A(头号,需实证)**:glibc 基线选型 —— 见决策 #10 + P0 实证。musl-static 可完全绕开(可移植性务实默认)。
- **B**:生产 rootfs 常剥 `-dev`(无头/`.pc`)→ 需 SDK staging sysroot;归开发者(决策 #11/#12)。
- **C**:aarch64-linux-gnu payload 还没有(只有 musl)→ 工作项非风险(决策 C)。
- **D**:clang vs gcc 战略分叉 → 先 GCC(决策 #8),clang 存档。
- **E**:厂商 C++-only 库 → C 桥支持(决策 #9)。
- **F**:sysroot 可复现/pinning → 归开发者(决策 #12)。
- **G**:multiarch 布局差异 → 支持 3 主流(决策 #13)。

---

## 7. 关键文件索引

- 工具链身份 / xim 映射 / `is_system_toolchain`:`src/toolchain/registry.cppm`(30, 41-62, 200-255, 298-300)
- `explicit_compiler` 路径通道(路径式编译器的种子):`src/toolchain/probe.cppm:234-250`、`detect.cppm:16-37`
- 链接模型 + Sysroot 模式(**最小切入点 L2**):`src/toolchain/linkmodel.cppm`(26-31, 80-95, 226-279)
- hermetic 断言(白名单已含 `tc.sysroot`):`src/build/hermetic.cppm:103-212`(117-130)
- sysroot 探测(**无外部入口 = 待补**):`src/toolchain/probe.cppm:271-380`、`src/fallback/probe_sysroot.cppm`
- C++23 能力门控 / std.o:`src/toolchain/gcc.cppm:66-192`、`stdmod.cppm`、`build/prepare.cppm:3419-3464`
- manifest schema(无 sysroot/pkg-config 字段 = 待补)、per-target `cfg` 条件:`src/manifest/types.cppm`、`toml.cppm`、`build/prepare.cppm:77-191`
- `[runtime] dlopen_libs` 钩子:`src/manifest/types.cppm`(RuntimeConfig)、`build/plan.cppm`
- detection-first 先例(msvc@system):`src/toolchain/msvc.cppm`、`.agents/docs/2026-07-13-msvc-system-toolchain-detection-design.md`
- hermetic link model 底座:`.agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md`
- 交叉工具链先例:`.agents/docs/2026-07-15-mingw-linux-cross-windows-design.md`、`2026-06-23-aarch64-musl-gcc-canadian-cross-rebuild.md`

---

## 附录 A:Rust/Cargo 如何解决同类问题(对照)

Rust 靠三个结构性属性,mcpp 有其二、缺其一:

1. **一个通用 LLVM 编译器 + rustup 管理器**(编译器与目标解耦,`rustup target add` 只下预编译 std)→ mcpp 已选「自管工具链」,但 GCC 非天生多目标(须每 arch 一套;clang 路线更接近)。
2. **`no_std` 分层 std**(裸机目标侧运行期需求为零)→ mcpp 的等价物 = freestanding C++23 modules,**尚未实现**(决策 #15 推迟)。
3. **静态优先 + `-sys` crate + pkg-config**(动态库尾巴走 `PKG_CONFIG_SYSROOT_DIR`)→ mcpp 静态优先已有,pkg-config 待补(P1)。

关键洞见:**Rust 没有消灭 sysroot/外部库问题——hosted Linux + 动态系统库场景 Rust 一样有,解法也一样(委托 C 交叉工具链 + pkg-config + 目标 sysroot,或走静态)。Rust 消灭的是「旧厂商编译器」问题(单一通用上游 + `no_std`)。** mcpp 的 config② + 自管工具链正是对标这一点。
