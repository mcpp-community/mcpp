# mcpp 适配 HarmonyOS / OpenHarmony — 调研与设计

- Date: 2026-08-04
- Status: **RFC / 探针 PR(不合入)**;引擎改动已实现并本机 + CI 端到端验证
- 关联:
  - [issue #276](https://github.com/mcpp-community/mcpp/issues/276) 与
    `.agents/docs/2026-07-24-embedded-platform-support-design.md`(config② 定调)
  - `.github/workflows/cross-build-test.yml` 里那条 **"llvm/clang cross:待接线"** 注释
  - 参考项目:[TermonyHQ/Termony](https://github.com/TermonyHQ/Termony)
- 结构:**结论 → 实测事实 → 为什么只能这么设计 → 改了什么 → 验证边界 → 遗留与下一步**

---

## 0. TL;DR

**HarmonyOS 可以成为 mcpp 的一等目标,而且是「完整体验」——C++23 具名模块与
`import std` 都能跑通。代价是必须先把 config② 里一直没实现的那半(clang 交叉
通道)做出来。**

三句话:

1. **鸿蒙 SDK 的编译器永远用不了。** 实测 OpenHarmony SDK **6.1(API 23,
   2026-03 发布)自带 clang 仍是 15.0.4** —— 比 C++20 模块所需的
   `-fmodule-output`(clang 16)还差一代,比 `import std`(libc++ 19+)差四代。
   这不是「等厂商升级」能绕开的,它已经这样很多年。
2. **所以 HarmonyOS 是第一个「只能用 config②」的目标,而且逼出 clang 路线。**
   config②(mcpp 自带编译器 + 消费外部 sysroot)在别的平台是两种可行架构之一;
   在这里 GCC **根本没有 `ohos` target**,「自带匹配 libc 的 GCC」那半话写不出来
   —— 决策 #8 存档的 clang 路线在此变成唯一解。
3. **`import std` 不是能力缺口,是 payload 缺口。** 用 LLVM 源码为
   `aarch64-linux-ohos` 编一份 libc++(`-DLIBCXX_HAS_MUSL_LIBC=ON`),`std.cppm`
   就有了,`import std;` 立刻可用 —— 已在 qemu 下真跑通。

本 PR 交付的是**引擎**:让 clang 可被重定向(retarget)。libc++ payload 的分发
是后续的独立工作项。

---

## 1. 实测事实(先摆证据,再谈设计)

全部在本机 x86_64 Linux + OpenHarmony SDK 6.1.0.31 实测,不是从文档推断的。

| # | 事实 | 怎么测的 |
|---|---|---|
| F1 | **SDK 6.1 自带 clang = 15.0.4** | `native/llvm/bin/clang++ --version` → `OHOS (dev) clang version 15.0.4` |
| F2 | SDK 自带 libc++ = **15.004**,全树**无 `std.cppm`** | `grep _LIBCPP_VERSION .../libcxx-ohos/.../__config`;`find -name std.cppm` 零命中 |
| F3 | `aarch64-linux-ohos` 是**上游 LLVM 认识的 target** | 上游 clang 22.1.8 `--target=aarch64-linux-ohos -dM -E` 输出 `__OHOS__ 1` `__linux__ 1` |
| F4 | 上游 clang **能**用 SDK 的 sysroot 交叉编 + 链接 | 见 §3.2 的 flag 组合;产物 `ELF ... ARM aarch64 ... statically linked` |
| F5 | 产物在 **qemu-aarch64 下真的跑** | 静态产物直接 `qemu-aarch64 ./prog` 输出正确 |
| F6 | 鸿蒙 **动态**产物在 qemu 下段错误 | `QEMU_LD_PREFIX` 指向 SDK 的 `libc.so`(即 musl loader)后仍 SIGSEGV —— 静态是 CI 可验证的那条路 |
| F7 | OHOS 的 PT_INTERP 是 **`/lib/ld-musl-aarch64.so.1`** | `readelf -l` NDK 产物;即 libc 是 musl 派生且沿用 musl 的 loader 命名 |
| F8 | **libc++ 22 能为 ohos 从源码编出来**,含 `std.cppm` | `runtimes` 构建 + `-DLIBCXX_HAS_MUSL_LIBC=ON`(不加则 `__regex_word` 未声明,~1800 个对象后才炸) |
| F9 | **`import std;` 在鸿蒙目标上可用** | 用 F8 的 libc++ 编 `std` BMI + `std.o`,链接后 qemu 下 `std::println` 正常输出 |
| F10 | `libc.a`/`libm.a`/`libpthread.a` 都在 sysroot 里 | `native/sysroot/usr/lib/aarch64-linux-ohos/` —— 全静态可行 |

两个「差点掉进去」的坑,记在这里因为它们不会在文档里写:

- **libc++ 头有两棵树。** `native/llvm/include/c++/v1` 是**宿主**的(它的
  `__config_site` 是给 `x86_64-unknown-linux-gnu` 生成的),鸿蒙的那棵在
  `native/llvm/include/libcxx-ohos/include/c++/v1`。选错**不报错**,只是把目标
  代码按宿主 libc++ 配置编译。
- **`libunwind.a` 的 `.S` 对象会被编成宿主的。** CMake 的 ASM language 不继承
  `CMAKE_CXX_COMPILER_TARGET`,所以 runtimes 构建装出来的 `libunwind.a` 里是
  x86_64 对象,lld 报 `incompatible with aarch64linux`。用平台自带的
  unwinder 才是对的。

### 1.1 Termony 给了什么参考

[Termony](https://github.com/TermonyHQ/Termony)(鸿蒙上的 Termux 类终端)证明了
生态方向,但它解决的是**另一个问题**:把大量 C 工具链打包成 `.hnp` 塞进 `.hap`
装到设备上,并用 `qemu-aarch64`/`elf-loader` 绕开权限限制。它没有回答
「C++23 modules 怎么办」—— 因为它编的基本是 C 项目,用 SDK 自带 clang 15 足够。
mcpp 的处境正相反:模块图与 `import std` 是 mcpp 的全部价值,SDK 那个编译器
一行都编不了。**所以 Termony 的路线(用 SDK 编译器)对 mcpp 结构性不可用**,
它的价值在于确认了 qemu-user 跑鸿蒙 aarch64 产物这条验证路径是可行的(F5)。

---

## 2. 为什么只能是 config②(而且只能是 clang)

`2026-07-24-embedded-platform-support-design.md` 决策 #2 定调 config②:
**mcpp 自带(匹配 libc 的)交叉编译器 + 消费外部 sysroot**。决策 #8 又定
「先 GCC,clang 路线存档」。

HarmonyOS 把这两条撞在一起:

```
config② 需要:  一个能编 C++23 modules 的、面向 target 的编译器
GCC 能提供吗:  不能 —— GCC 没有 ohos target,一行都编不出来
SDK 能提供吗:  不能 —— clang 15.0.4(F1),比模块门槛差一代
剩下的:        mcpp 自己的 clang,用 --target 重定向
```

也就是说:**HarmonyOS 不是「适合用 clang」,而是「除了 clang 无路可走」。**
决策 #8 存档的 clang 路线(风险项 D)在这里被现实提前触发了。

这同时解释了为什么这条改动**不是鸿蒙专用**。仓库里早就写着这个缺口:

```
# .github/workflows/cross-build-test.yml
#   * llvm/clang cross  : clang is inherently a cross-compiler, but mcpp does not
#                         yet inject `-target <triple>` + a cross sysroot for a
#                         clang toolchain; cross `--target` resolves to gcc musl
#                         only. Wire the clang cross path first, then add a row.
```

本 PR 做的就是 "wire the clang cross path first"。鸿蒙只是**第一个非它不可**的
消费者;`aarch64-linux-gnu`(树莓派,决策 #3 的滩头)之后可以走同一条缝,
不必再造一套。

### 2.1 一个被否掉的替代方案

「让 SDK 的 clang 15 只做链接,mcpp 的 clang 做编译」—— 否掉。mcpp 的
compile/link 共用一个 `plan.toolchain.binaryPath`,拆成两个 driver 是比
retarget 大得多的改动;而 `-resource-dir` 已经能只在链接侧供给目标的
compiler-rt(§3.2),用一个 flag 换来两套 driver,不划算。

---

## 3. 机制

### 3.1 三段身份:`ohos` 是 env 不是 os

mcpp 的 triple 语言是 `arch-os[-env]`。鸿蒙**正好**落在这个语言里:
`aarch64-linux-ohos` = arch `aarch64` + os `linux` + env `ohos`。这也是上游
LLVM 自己的模型(`llvm::Triple::OpenHOS`)。

**这个选择是有后果的,而且后果是对的**:内核确实是 Linux,所以
`cfg(os = "linux")` / `cfg(family = "unix")` 必须继续匹配。若把 ohos 拼成一个
新 os,所有已有可移植包里的 `cfg(os = "linux")` 段会**静默失配** —— 一个包突然
在鸿蒙上少编一半源码,而且没有任何诊断。

反向的一条同样重要:**`is_musl()` 必须返回 false**。OHOS libc 确实是 musl 的
fork,但 mcpp 里 "musl" 处处指的是**上游 musl**(payload 选择、`abi:musl`
capability、`ld-musl-*.so.1`),鸿蒙产物与之不可互换。所以 ABI 模型里
`libc = "ohos"` 自成一维取值,而 loader 命名(F7)照旧走 musl 那条 —— **同一个
事实在两个问题上给出不同答案,这正是为什么它们得是两个函数**。

### 3.2 driver retarget:`Toolchain::crossTarget`

新的 `CrossTarget` 描述「这个 driver 正在被重定向」:

| 字段 | 内容 | 为什么单独存在 |
|---|---|---|
| `triple` | 请求的目标,覆盖 `-dumpmachine` | clang 的 `-dumpmachine` 永远答宿主 |
| `sysroot` | `<native>/sysroot` | → `CLibMode::Sysroot` |
| `cxxIncludes` | 目标 libc++ 头 | 与 sysroot **分开**:鸿蒙里它们在两棵树;而且只换这一对就能从「无 import std」升级到「有」 |
| `libDirs` | 目标 libc++/libc++abi/libunwind | 宿主 payload 的 `linkRuntimeDirs` 对交叉产物毫无意义 |
| `linkResourceDir` | SDK 的 clang resource dir | **只在链接侧**,见下 |
| `stdModuleSource` | 目标 libc++ 的 `std.cppm`,可空 | 由 provider 给,不由 driver 探 |

**`-resource-dir` 为什么只能在链接侧。** 上游 clang 的 OHOS driver 会去**自己的**
resource dir 找 `libclang_rt.builtins.a` 与 `clang_rt.crt{begin,end}.o` —— 它当然
没有 `aarch64-linux-ohos` 子目录,于是 `cannot open crtbeginT.o`。把
`-resource-dir` 指向 SDK 的那份就解决了。但同一个 flag 也换掉了**内建头**
(`stddef.h`/`immintrin.h`…),让 clang 22 去读 clang 15 的内建头是另一个**安静得多**
的 bug。mcpp 的 compile flags 与 link flags 本来就是两条串,所以只在后者发出。

**`stdModuleSource` 为什么不能探。** `clang::enrich_toolchain` 靠
`-print-library-module-manifest-path` 找 `std.cppm` —— driver 答的是**它自己的**
libc++,交叉时那是宿主的。拿它去编 `std` BMI **不会报错**,只会产出一个面向错误
平台的 BMI。所以交叉路径整段短路,由 provider 供给;供不出来就
`hasImportStd = false` + 明确提示,而不是编出个错的。

同理 `probe_payload_paths()` 在交叉时必须跳过:它会找到编译器旁边**宿主的**
glibc xpkg,而 `CLibMode::PayloadFirst` 会把宿主的 `crt1.o`/`libc.so`/loader
喂给一个外国目标。

### 3.3 SDK 发现:detection-first,先例是 `msvc@system`

mcpp 不能下发鸿蒙 SDK(~2.5 GB、厂商许可),这与 MSVC 的处境一样,所以走同一个
形状:探测 + 明确的「装不了,请这样设置」文案。

探测顺序:`$OHOS_NDK_HOME` → `$OHOS_SDK_NATIVE`(`setup-ohos-sdk` 导出的那个)
→ `$OHOS_SDK_HOME/native` → `$OHOS_SDK_HOME/<api>/native`(取最高 API,不取目录
迭代顺序的第一个)→ `$DEVECO_SDK_HOME/…` → `~/ohos-sdk/native` →
`/opt/ohos-sdk/native`。

判据不是目录名而是**mcpp 真正需要的两个文件**
(`sysroot/usr/include/stdlib.h` + `llvm/lib`)—— 一个「看起来对」但没有 sysroot
的路径必须在这里被拒,而不是十分钟后死在某条编译命令里。

### 3.4 `import std` 的升级位:`MCPP_OHOS_LIBCXX`

env 优先、索引最后,是刻意的:
`2026-08-03-index-availability-must-not-decide-mcpp-availability.md` 的教训是
**索引侧的东西不得决定 mcpp 是否可用**。SDK 自带的 libc++ 永远在、永远能用
(named modules 这一档),外部 libc++ 是**升级**而非前提。

---

## 4. 改了什么

| 文件 | 改动 |
|---|---|
| `src/toolchain/triple.cppm` | `ohos` env 解析(含 `ohoseabi*`/`openhos` 与 4 段 LLVM 拼写);`is_ohos()`;三行 known-target;`pins::kOhosLlvm` |
| `src/toolchain/abi.cppm` | `libc = "ohos"`(在 musl 判定**之前**) |
| `src/toolchain/model.cppm` | `CrossTarget` + `Toolchain::crossTarget` |
| `src/toolchain/ohos.cppm` | **新增** —— SDK 发现、`cross_paths()`、libc++ overlay、`install_guidance()` |
| `src/toolchain/linkmodel.cppm` | 交叉时强制 Sysroot 模式;`ClangDriverModel` 增加 `--target=`/`-resource-dir=` 产出;loader 命名认 ohos |
| `src/toolchain/hostflags.cppm` | 交叉 ⇒ 无条件 bypass cfg(cfg 是装机时按宿主生成的) |
| `src/toolchain/detect.cppm` | `detect(bin, cross)`;交叉时覆盖 triple、跳过 sysroot/payload 探测;非 clang 直接报错 |
| `src/toolchain/clang.cppm` | 交叉时不探宿主 libc++,改用 provider 供的 `std.cppm` |
| `src/toolchain/registry.cppm` | `host_can_serve()`:ohos 目标问「SDK 在不在」 |
| `src/build/flags.cppm` | ninja 字符串通道发交叉 token;`find_archive` 在交叉时只看目标 libDirs |
| `src/build/prepare.cppm` | ohos 目标 → 探 SDK → 组 `CrossTarget` → 喂给 `detect()` |
| `src/build/hermetic.cppm` | 白名单纳入 SDK 的 libDirs/includes/resourceDir |
| `tests/unit/test_ohos_target.cpp` | **新增** 16 个用例,全部 host-independent |
| `tests/e2e/103_…`, `104_…` | **新增** 两档端到端(qemu 真跑) |
| `.github/workflows/ci-harmonyos.yml` | **新增** 两个 job |
| `examples/05-harmonyos/` | **新增** 示例 |
| `README.md`, `docs/03-toolchains.md` | 平台表 + 完整章节 |

**宿主路径零行为变化**:所有分支都以 `tc.crossTarget` 为门,
`OhosCross.HostToolchainIsUntouchedByTheseChanges` 是这条的回归闸。

---

## 5. 验证边界(能证明什么,不能证明什么)

**能**(本机 + CI 都跑过):

- `mcpp build --target aarch64-linux-ohos` 产出 `ELF … ARM aarch64 … statically linked`
- 该产物在 `qemu-aarch64` 下**真的执行**并输出预期内容
- C++23 **具名模块**对着**原版 SDK** 可用
- 配上为目标编的 libc++ 后 **`import std;` 可用**
- 目标行在有 SDK 时显示 `available`、无 SDK 时不显示(`host_can_serve` 真的接上了)

**不能**:

- **qemu-user 跑的是指令集,不是 HarmonyOS。** 它对系统调用兼容性、设备行为
  一概不发言。
- **`.hnp`/`.hap` 打包与 `hdc` 安装完全没做。** 这是 Termony 那条线的活,属于
  「产物怎么上设备」,与「产物对不对」是两个问题。
- **没有链接平台 NDK 库**(`libace_napi.z.so`、`libhilog_ndk.z.so` …)。全静态
  产物本来也链不了它们(那些只有 `.so`)。真做鸿蒙应用要走动态链接,而 F6 说明
  动态产物在 qemu 下跑不起来 ⇒ **动态那档必须真机/模拟器验证,CI 给不了绿**。
- **模拟器没用上。** GitHub runner 无法跑鸿蒙模拟器(要 DevEco + 虚拟化 +
  厂商镜像);这是本次「利用 CI 各种 OS 资源」这条里唯一没兑现的部分,原因是
  客观不可得而非没做。

把这条写清楚是因为:mcpp 的 `verified` 档位定义是「CI builds **and executes**」。
本 PR 的 `aarch64-linux-ohos` 满足这个定义,但**满足的是与
`aarch64-linux-musl` 同级的那个断言,不是「鸿蒙 App 能上架」**。

---

## 6. 遗留 / 下一步(按价值排序)

1. **`ohos-libcxx` payload 化。** 现在 `import std` 要用户自己编一次 libc++。
   把它做成 xim payload(每个 LLVM 版本 × 每个 ohos target 一份)之后,
   `mcpp build --target aarch64-linux-ohos` 就是开箱完整体验。**这是本设计里
   唯一横在「能用」和「好用」之间的东西。**
2. **动态链接档位。** 真鸿蒙应用要动态链 + 链平台 NDK 库。需要:非静态默认的
   opt-in、`libc++_shared.so` 的部署、以及真机验证路径。
3. **`.hnp`/`.hap` 打包。** 属于 `mcpp pack` 的新 mode,可参考 Termony 的
   `sign.py` 与 hnp 结构。是否属于 mcpp 职责边界内需要维护者定调
   (对照决策 #1「不做发行版构建器」)。
4. **`x86_64-linux-ohos` 提到 verified。** 模拟器架构;SDK 里已经有这套库,
   CI 上甚至不需要 qemu(宿主同架构),成本最低的一个。
5. **把 clang cross 缝用到 `aarch64-linux-gnu`。** 决策 #3 的滩头(树莓派)现在
   可以不再等「自建低 glibc 的 GCC 16」——同一条缝 + 设备 sysroot 就够了。
   这条值得单独评估,因为它可能改变 #276 的 P0 排序。

---

## 7. 关键文件索引

- 目标身份:`src/toolchain/triple.cppm`(`is_ohos`、`kKnownTargets`、`pins::kOhosLlvm`)
- SDK 发现:`src/toolchain/ohos.cppm`
- 重定向模型:`src/toolchain/model.cppm`(`CrossTarget`)、
  `src/toolchain/linkmodel.cppm`(`ClangDriverModel::target_tokens`/`cross_link_tokens`)
- 应用点:`src/toolchain/detect.cppm`、`src/toolchain/hostflags.cppm`、
  `src/build/flags.cppm`
- 接线:`src/build/prepare.cppm`(ohos 分支)
- 验证:`tests/unit/test_ohos_target.cpp`、`tests/e2e/103_harmonyos_cross_qemu.sh`、
  `tests/e2e/104_harmonyos_import_std.sh`、`.github/workflows/ci-harmonyos.yml`
- 前置设计:`.agents/docs/2026-07-24-embedded-platform-support-design.md`(config②、决策 #2/#8)
