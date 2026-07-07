# 08 — 工具链机制内幕

> 本文详细描述 mcpp 工具链机制的内部工作原理,以及如何扩充新工具链、新架构乃至
> 嵌入式目标的支持。与面向用户的 [03 — 工具链管理](03-toolchains.md)(CLI 用法)
> 互补,本文面向贡献者与维护者。

## 1. 一张图看全模型

```
mcpp.toml [toolchain]  /  全局默认  /  `mcpp toolchain install`
        │  (三条入口路径 —— 共享同一条管线)
        ▼
解析 payload(沙箱里的 xim:gcc / xim:llvm / xim:musl-gcc xpkg)
        ▼
ensure_post_install_fixup()      ← 幂等收敛(marker 闸门)
        ▼
detect / probe                   ← triple、sysroot、payload 路径(glibc、linux-headers)
        ▼
ToolchainLinkModel(C 库轴的唯一解析器)
        ├──► flags.cppm        (主构建编译/链接 flags)
        ├──► stdmod.cppm       (`import std;` BMI 预编译)
        ├──► build_program     (build.mcpp 宿主编译)
        └──► cfg 再生          (供人类直接使用的 clang++.cfg)
        ▼
hermetic 链接校验(`-###` 干跑)  ← 断言 CRT/loader 全部解析进沙箱
```

贯穿一切的两条原则:

1. **沙箱工具链自包含。** 产物的 CRT 启动对象、libc、动态链接器全部来自沙箱
   payload——绝不静默落到宿主。在没有编译器、没有 `/usr/lib/**/Scrt1.o` 的机器
   (全新 WSL2、精简容器)上一切照常;在装了宿主工具链的机器上也不会有任何泄漏。
2. **每层路径知识只有一个属主。** 过去"如何对 payload glibc 链接"有四份漂移副本,
   现在收敛为一个解析器(`linkmodel`);过去 fixup 行为按入口路径各自为政,现在
   是一条管线。副本间漂移正是一整类 bug 的来源(issue #195)。

## 2. 工具链解析

工具链 spec(`gcc@16.1.0`、`llvm@22.1.8`、`gcc@15.1.0-musl`)映射为 xim 包
(`src/toolchain/registry.cppm`:`parse_toolchain_spec` → `to_xim_package`,
产出含 xim 包名、版本、前端候选的 `XimToolchainPackage`)。payload 经 xlings
后端解析/自动安装到沙箱
(`$MCPP_HOME/registry/data/xpkgs/xim-x-<name>/<version>/`)。

`detect`/`probe`(`src/toolchain/detect.cppm`、`probe.cppm`)随后推导:

| 字段 | 方式 |
|---|---|
| `targetTriple` | `<compiler> -dumpmachine` |
| `sysroot` | `-print-sysroot`(校验必须真带 libc 头);xlings 构建的 GCC 烙的是构建机路径,有 remap 回退 |
| `payloadPaths` | 兄弟 xpkg 发现:glibc payload(`include/` + `lib64|lib/`)与 linux-headers payload——即 *payload 优先* 的细粒度 sysroot |
| 运行库目录 | 工具链私有 lib 目录,用于产物的 `-L`/`-rpath` |

注意:probe 已**不再**从 clang cfg 挖 `--sysroot`——cfg 是这套机制的输出,
不是输入(见 §5)。

## 3. 链接模型(`src/toolchain/linkmodel.cppm`)

`ToolchainLinkModel` 只回答一个问题——*如何对该工具链的 C 库编译与链接*——
全部消费方从它派生 flags:

```
CLibMode::PayloadFirst   找到 glibc/linux-headers xpkg(bundled LLVM 与
                         无可用 sysroot 的 GCC 的常态)
                           编译:-isystem(clang)/ -idirafter(gcc)payload 头
                           链接:-B <glibcLib>   ← CRT 发现(Scrt1.o/crti.o/crtn.o;
                                                    driver 从不查 -L 找它们)
                                 -L <glibcLib> [clang 另加 -rpath 与 --dynamic-linker]
CLibMode::Sysroot        可用的 --sysroot(GCC include-fixed 世界、自包含 musl
                         sysroot、macOS SDK)
CLibMode::None           无可用来源——落宿主默认,由 hermetic 校验(§6)报告泄漏
```

`ClangDriverModel` 服务 bundled LLVM:mcpp 构建永远传 `--no-default-config`
(绕过装机生成的 cfg 以保证可复现),并显式补上 libc++ 头/库与
`-fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind`。

**loader 解析**是数据驱动、无硬编码:先查按架构的 triple 映射表
(x86_64 / aarch64 / riscv64 / loongarch64 / i686,glibc 与 musl 两种拼写),
再对 payload 做 `ld-*.so*` glob 兜底(覆盖映射表未收录的架构)。曾实现过第三个
来源——由安装器持久化的声明式元数据(`.xpkg-exports.json`)——经评估后**移除**:
其唯一读者就是本解析器,而上述两级已覆盖全部真实 payload(0.0.83 的完整验证
矩阵在该文件从未存在的情况下全绿),通用包管理器不应承载只有单一下游工具在读的
机制。若将来出现"安装态元数据库"的真实需求,应以 xlings 自身为第一消费者来
设计,届时 mcpp 再加回读取端即可。

## 4. 统一 post-install fixup 管线(`src/toolchain/post_install.cppm`)

沙箱 payload 是预编译 ELF 树。有两类打包期不可能预知、必须对齐到**本机沙箱**的
路径:二进制里的 `PT_INTERP`/`RUNPATH`,以及 GCC specs 里的 loader/rpath 行。
`ensure_post_install_fixup(cfg, payloadRoot, pkg)` 是这次对齐的**唯一入口**,
由三条入口路径(显式 install、默认 auto-install、manifest auto-install)共同
调用。

> 历史注记:0.0.83 之前各路径各自记得——或忘记——自己那份 fixup。manifest
> 路径什么都不跑:刚 auto-install 的 llvm 因此保留着陈旧、随装机环境漂移的
> cfg(issue #195);gcc 也曾因此产出找不到 `stdlib.h` 的沙箱。"用哪条命令装的"
> 绝不能决定"工具链好不好用"。

**触发语义——每次都问,只做一次:**

```
每次构建 → ensure() → 读 <payload>/.mcpp-fixup.json
                       marker == {schema, kind, rev, glibcLib}?  → return(毫秒级)
                       失配 → 执行该 kind 的 fixup,写 marker
```

marker 是**内容指纹化的缓存**,不是事件标志:它编码了 fixup 代码版本与所对齐的
glibc payload。"做"分支因此在每个 `(payload × fixup-rev × glibc 指纹)` 组合上
恰好执行一次——首次使用,外加两类确需重写的重新收敛事件(`kFixupRev` 升级、
glibc payload 被更换)。之所以每次构建都问:让 payload 失效的事件(xlings 换
glibc、payload 从别的 home 继承而来)发生在 mcpp 视野之外,trust-but-verify
是唯一可靠语义。

**分 kind 的动作:**

| kind | 动作 |
|---|---|
| `gcc`(glibc)| 对 gcc payload **及共享的 binutils payload** 做 patchelf 遍历(PT_INTERP → 沙箱 loader,RUNPATH → glibc+gcc lib);specs 重写(烙入的 loader/rpath → payload glibc,**必须感知 specs 语法**——`%{...}` 条件块绝不能被破坏)|
| `llvm` | 只遍历 `lib/`(运行库 `.so` 的 RUNPATH;`bin/` 不碰,保留 xlings 设置的 RUNPATH);确定性再生 cfg(§5)|
| `musl-gcc` | 无——自包含 sysroot,静态世界 |

**安全不变量**(每条都由真实事故换来):

- **绝不就地写。** patchelf 作用于副本,再原子 `rename()` 换入:payload 里可能
  有**当前进程**(自托管、动态链接的 mcpp)或并发构建正 mmap 着的库,就地重写
  活映射的 backing file 会损坏运行中的进程(实测:退出时 `_dl_fini` 段错误)。
  `rename` 让新内容拿新 inode,活进程保有旧 inode。
- **所有权护栏。** 解析到本 home registry 之外的 payload(从别的 `MCPP_HOME`
  symlink 继承)一律不碰——属主已收敛过,隔着 symlink 改写会毁掉属主的工具链。
- specs 重写是内容感知的(已对齐即跳过)。把同样的检查扩展到 patchelf 遍历
  (写前比对 `--print-interpreter`/`--print-rpath`,已对齐的 payload **零写入**
  收敛)是已知的后续项。
- 长期方向:全部写入由**安装器**(xlings)持有——装机时、以及 payload 进入
  新 home 时——mcpp 退为只读 + 校验。本管线在那之前是兼容层,也是双向漂移的
  自愈机制。

## 5. clang cfg:仅服务直接调用场景

`bin/clang++.cfg` 的职责是:直接调用打包内 `clang++`(不经由 mcpp)时,
获得可用且 hermetic 的编译器配置。mcpp 自己的构建从不读它(永远 `--no-default-config`)。
fixup 管线从链接模型**确定性再生**它——同一 payload ⇒ 任何机器、任何安装路径
产出字节一致的 cfg——而不是对装机产物做行级补丁。Linux 上内容为:CRT 发现
(`-B`)、payload loader + rpath、lld/compiler-rt/libunwind、C++ 驱动附加
bundled libc++;macOS 保持历史形态(`--sysroot=<SDK>` + payload libc++ 头,
C++ 运行时的链接交由主构建的平台专属处理)。

## 6. hermetic 链接校验(`src/build/hermetic.cppm`)

在 Linux 上用沙箱工具链构建前,mcpp 以真实链接 flags 干跑 driver
(`-### -x c++ /dev/null`),断言每个 CRT 对象与**生效的**动态链接器(取最后
一次出现)都解析在允许的沙箱前缀之下。这把两种静默故障变成一个可行动的诊断:
裸 CRT 名传给 lld(干净机器上的 #195 症状)与静默的宿主 CRT 污染(它曾让有
宿主工具链机器上的绿色 CI 成为假信号)。判定按 flag 集缓存
(`.mcpp-hermetic-ok`);逃生阀:`[build] allow_host_libs = true` 或
`MCPP_ALLOW_HOST_LIBS=1`。系统/PATH 编译器豁免——显式选择宿主世界是用户的
权利。

CI 用一个**完全没有宿主工具链**的 job(`debian:stable-slim`,无 gcc、无宿主
`Scrt1.o`)守住这一切——那是唯一能忠实复现干净机器故障模式的环境类;另有 e2e
`86_llvm_hermetic_link.sh` 在任何机器上复核 `-###` 的解析结果。

## 7. 扩充指南

### 7.1 新增一个工具链(新编译器家族或发行版)

1. **索引侧**(xim-pkgindex):包含 payload 资产的包,以及——关键——对所需
   C 库 payload 的 `deps` 声明(`xim:glibc`、`xim:linux-headers`)。遵循
   llvm/gcc 的打包 SOP,包括准入 gate(`verify-toolchain.sh`):缺件检查 +
   hermetic CRT 解析 + 真实编译/链接/运行,通过才可发资产。
2. **注册表**(`src/toolchain/registry.cppm`):让
   `parse_toolchain_spec`/`to_xim_package` 认识 spec 写法、xim 包名与
   `frontendCandidates`(哪个二进制是 C++ 驱动)。
3. **能力**(`src/toolchain/provider.cppm`):stdlib 身份、BMI 特性及
   `flags.cppm` 消费的特性开关。
4. **fixup kind**(`post_install.cppm`):确定该 payload 需要哪种装后对齐——
   gcc 式(patchelf + specs)、llvm 式(lib patchelf + cfg)或无(自包含),
   接入 `ensure_post_install_fixup` 的分发。
5. **e2e**:一个 `86_llvm_hermetic_link.sh` 风格的 hermetic 链接测试,并纳入
   无宿主工具链 CI job。

### 7.2 新增一个 CPU 架构(Linux)

机制已按架构参数化,剩下的是数据:

1. 在 `linkmodel.cppm::loader_filename` 的映射表中加入该架构的 glibc/musl
   loader 名(加之前 glob 兜底也能工作);
2. 为该架构发布 payload 资产(glibc、linux-headers、工具链本体)——
   aarch64-linux-musl 交叉目标是现成先例(`[target.aarch64-linux-musl]`,
   经 spec 的 `targetTriple` 解析交叉前端);
3. 其余什么都不用做:`-B`/`-L`/loader 的发射、fixup 管线、hermetic 校验全部
   与名字无关。

### 7.3 嵌入式 / 裸机工具链(展望)

模型可以自然延伸到 `arm-none-eabi` 一类工具链,因为 hosted 世界的难点在这里
是**消失**而不是加倍:

- **无动态链接器**:`loader` 保持为空——全链路本来就允许(渲染器自动省略
  `--dynamic-linker`;部署故事是烧录,不是 ELF interp);
- **无 glibc payload**:newlib/picolibc 在工具链自己的 sysroot 里 ⇒
  `CLibMode::Sysroot`,与今天自包含 musl 完全同一模式。`is_musl_target` 式的
  自包含判定应泛化为能力标志("自带 C 库");
- **fixup kind = 无或 gcc 式**,取决于 payload 怎么打包(交叉 gcc 的
  **宿主运行**的编译器二进制仍需要 PT_INTERP/RUNPATH 对齐——与今天的 gcc kind
  完全相同;**目标侧**什么都不需要);
- **hermetic 校验**可泛化:断言 crt0/semihosting stub 解析在工具链 payload 内,
  替代 Scrt1.o/loader;
- 真正需要新设计的:MCU flags 的 per-target 规格(`-mcpu`、
  `--specs=nosys.specs`)、链接脚本处理、运行/烧录故事——那些是本档之上的
  构建图层面。

### 7.4 非 ELF 平台

macOS(Mach-O)与 Windows(PE)有意绕开本文大部分内容:macOS 从 SDK 解析
C 世界(`CLibMode::Sysroot`)并有自己的 libc++ 链接处理;Windows 没有 rpath——
mcpp 把运行时 DLL 部署到产物 exe 旁,这正是该平台对 §3–§4 所做一切的原生
等价物。

## 8. 源码地图

| 关注点 | 文件 |
|---|---|
| spec → xim 包、前端 | `src/toolchain/registry.cppm` |
| detect/probe(triple、sysroot、payload)| `src/toolchain/detect.cppm`、`probe.cppm` |
| 链接模型 + loader 解析 | `src/toolchain/linkmodel.cppm` |
| 统一 fixup 管线(patchelf/specs/cfg、marker)| `src/toolchain/post_install.cppm` |
| install/lifecycle 入口 | `src/toolchain/lifecycle.cppm`;auto-install 入口在 `src/build/prepare.cppm` |
| flag 组装(主构建)| `src/build/flags.cppm` |
| `import std;` 预编译 | `src/toolchain/stdmod.cppm` |
| build.mcpp 宿主 flags | `src/build/build_program.cppm` |
| hermetic 链接校验 | `src/build/hermetic.cppm` |
| 回归fence | `tests/e2e/86_llvm_hermetic_link.sh`、单测 `test_linkmodel.cpp`、`test_post_install.cpp`;`ci-linux-e2e.yml` 的无宿主工具链 CI job |

设计沿革:`.agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md`。
