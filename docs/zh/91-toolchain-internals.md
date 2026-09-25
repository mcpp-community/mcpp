# 91 —— 工具链机制内幕

**读者：** 贡献者，或者正在排查「工具链为什么解析成这样」的用户。

**本章回答的那一个问题：** mcpp 究竟如何解析、安装并组装一条工具链，以及每个
flag 由哪一层决定。

**不在这里：** 如何**选择**一条工具链，那是
[20 —— 工具链管理](20-toolchains.md)。本章讲的是它之下的机制，这里的任何内容
都不是稳定接口。

> mcpp 工具链机制的内部工作原理，以及如何用新工具链、新架构，及（最终）嵌入式
> 目标扩展它。与面向用户 CLI 的 [20 —— 工具链管理](20-toolchains.md) 互补。
> 本文面向贡献者与维护者。

## 1. 一张图看懂整个模型

```
mcpp.toml [xlings].subos / mcpp-managed default runtime
        ▼
resolve runtime binding          ← which libc the artifact will load (§2.1) — an
                                   answer, not a search
        ▼
resolve toolchain payload        ← project/default/install paths share one pipeline
        ▼
ensure_post_install_fixup()      ← exact glibc@version, marker-gated; never readdir-first
        ▼
detect / probe                   ← triple, sysroot, payload paths (glibc, linux-headers)
        ▼
ToolchainLinkModel (single resolver for the C-library axis)
        ├──► flags.cppm        (main build compile/link flags)
        ├──► stdmod.cppm       (`import std;` BMI precompile)
        ├──► build_program     (build.mcpp host compiles)
        └──► cfg regeneration  (the human-facing clang++.cfg)
        ▼
hermetic link check (`-###` dry-run)  ← checks sandbox CRT/loader resolution
        ▼
link → internal ELF physics check    ← validates the artifact and resolved closure (§6.1)
```

贯穿全篇的两条原则：

1. **沙箱工具链默认接受 hermetic 校验。** 对于常规的 payload-first 或 sysroot
   路径，产物的 CRT 启动对象、libc 与动态链接器必须解析到允许的沙箱前缀之下。
   这不是一项无条件的隔离保证：`CLibMode::None` 会回落到宿主默认值，系统 /
   PATH 编译器是显式选择宿主世界，`[build] allow_host_libs = true` 或
   `MCPP_ALLOW_HOST_LIBS=1` 会退出宿主库检查。在既没有编译器、也没有
   `/usr/lib/**/Scrt1.o` 的机器上（全新的 WSL2、精简容器），常规沙箱路径仍能
   正常工作。
2. **每一层的路径知识只有一个属主。** 过去「如何链接 payload glibc」有四份
   彼此漂移的副本，现在收敛为一个解析器（`linkmodel`）；过去按入口路径各自
   维护的 fixup 行为，现在是一条管线。副本之间的漂移正是一整类 bug 的来源
   （issue #195）。

## 2. 工具链解析

自 0.0.93 起，身份由两条正交的轴构成：`ToolchainSpec` 是
`(family ∈ gcc|llvm|msvc, version, target Triple)`。`triple.cppm` 是唯一的
triple 解析器，加上封闭的已知目标词汇表；`compat.cppm` 是唯一认识旧拼法的
文件（`gcc@15.1.0-musl`、`musl-gcc`、`mingw`、`mingw-cross`、`clang`、
`<triple>-gcc` —— 解析时归一化，永久接受）。`to_xim_package` 是一个
*(family, target, host)* 到 payload 的映射，产出带 xim 包名、版本与前端候选
的 `XimToolchainPackage` —— host 拆分的分发名，如 Linux 宿主上的
`mingw-cross-gcc` 与 Windows 宿主上的 `mingw-gcc`，就存在于这一层；它们是
当前分发层的身份，不是面向用户的拼法。payload 经 xlings 后端解析 / 自动
安装到沙箱中（`$MCPP_HOME/registry/data/xpkgs/xim-x-<name>/<version>/`）。

`detect`/`probe`（`src/toolchain/detect.cppm`、`probe.cppm`）随后推导出：

| Field | Derivation |
|---|---|
| `targetTriple` | `<compiler> -dumpmachine` |
| `sysroot` | `-print-sysroot`（经校验：必须真的带有 libc 头文件），并为烙有构建期路径、在本机不存在的 xlings 构建版 GCC 提供 remap 回退 |
| `payloadPaths` | 由已解析的 runtime binding（§2.1）精确指名 glibc payload；linux-headers 仍作为同级 payload 被发现。没有 binding 就没有 payload-first，这是设计使然 |
| runtime dirs | 工具链私有的 lib 目录，供产物的 `-L`/`-rpath` 使用 |

注意，probe 刻意**不再**从 clang cfg 中挖 `--sysroot`：cfg 是这套机制的输出，
不是输入（见 §5）。

### 2.1 runtime binding —— 一个 libc，只决定一次

payload-first 的构建会链接到某一个具体的 glibc，而**是哪一个**，是根项目
本地开发 OS 的事实，不应从编译器路径或 shell 状态推断。mcpp 恰好有两种
选择方式：

1. 未声明 `[xlings].subos`：使用 `McppDefault`，即全局 mcpp 配置所选 xlings
   home 中已初始化的 `subos/default`。
2. `[xlings] subos = "<name>"`：使用 `NamedSubos(name)`。显式写出的
   `"default"` 仍然是一次具名选择；其他名字在根项目本地的 xlings scope 中
   解析。

workspace 构建期间由 workspace root 拥有这次选择。member 与 dependency 的
声明既不合并也不传播；同一个 member 的声明只在它作为独立 root 时才生效。
`XLINGS_ACTIVE_SUBOS`、`current`、编译器所属的 home，以及命令行 / 环境变量
的 override，都不构成第三种选择层级。

点名的环境不存在时是硬错误，绝不回退到 default / active / 编译器烙入的
状态 —— 该请求无法被满足，而换成另一个环境会让同一份 `mcpp.toml` 在不同
机器上对应不同的 ABI。

而「SubOS 存在，但**没有自我描述**」是另一种情形，它会**降级**：
`declared = false` 被记录下来，并附一条调用方必须打印的说明；runtime 规则
报告 `inconclusive` 而不是给出判决；也没有 payload-first binding 可用
（mcpp 拒绝去猜一个 libc 版本，因此 hermeticity 检查会如实报告回退到
宿主）。构建不会因此中止。同理，当 `subos_info` 的 schema **新于**当前
mcpp 所理解的版本时，mcpp 读取它认识的那些字段，并附一条说明 —— 这正是
这个读取器自己写下的规则：发布数据不得使读取它的程序失效。直接拒绝正是
2026.8.10.2 在 Windows 上让每一次 `mcpp build` 与 `mcpp test` 都停摆的
原因：那里 xlings 根本不写这个 block，它承载的那些事实也就不存在
（openxlings/xlings#543）。

mcpp 只把 SubOS 读取一次，形成一份 `RuntimeBinding` 快照，把其中的 libc
身份喂给 payload probing，并在 configure/link/run/test 与 fast-path cache
中复用同一份快照。在 Linux 上，该快照还记录所选 loader/libc 目录的规范
路径，以及可选的创建宿主 glibc 下限。这些是链接后校验器使用的证据，不是
另一套选择机制。

当解析出的 SubOS view 能够规范地指名一个受管的 glibc payload 时，它就是
权威事实。较旧的 xlings 状态可能在 view 已原子切换到受管的 2.44 payload
之后，仍保留着 `runtime = "glibc@2.39"`；此时 mcpp 记录的是真实的 2.44
身份与路径。若较旧的 view 含有一条断链，mcpp 只会解析 `runtime` 精确指名
的那个 payload；它从不枚举已安装版本，也不会挑选「最近的」或「最新的」
那一个。两条路径都只消费 xlings 给出的事实，并保持单一的 RuntimeBinding，
而不引入 mcpp 自己的运行时选择策略。

没有 binding 是一次**拒绝**，不是取默认值：`CLibMode::PayloadFirst` 会
被放弃，而不是去挑一个 libc。

被它取代的旧规则，是向某个目录询问「那个 glibc」，然后取 `readdir` 吐出
的第一项。只装了一个 glibc 时，这样做永远正确，因此从来没有什么迫使它
正确 —— 而一条带 `xim:glibc@>=2.38` 的依赖就足以装进第二个 glibc。这在
mcpp-index 上真实发生过：编译侧拿到的是 2.44，而产物的 interpreter（在
安装时冻结进 gcc 的 specs 里）仍然指名 2.39；二进制引用了 `GLIBC_2.42`
符号，却跑在没有这些符号的运行时上，故障浮现在与「拉进第二个 glibc 的
那条依赖」毫无关系的包上。目录顺序不是一种决策依据。

由于 binding 决定产物加载什么，完整的规范化 contract hash 是工具链指纹
的第 11 个字段 —— 即使两个具名 SubOS 环境恰好使用同一个 glibc，只要它们
的 provider 或环境声明不同，也绝不能共用同一条缓存记录。

### 2.2 通用的 runtime provider 与 artifact

`RuntimeBinding` 还携带 xlings 为所选开发 OS 解析出的、provider 无关的
事实。`subos_info.runtime_contract` 是 schema 1 上一个可选、可加的
block：

```json
{
  "providers": [
    {"capability": "display.present", "provider": {
      "namespace": "xim", "name": "display-runtime", "version": "1.0.0",
      "source": "xim-pkgindex@<revision>"}}
  ],
  "artifacts": [
    {"role": "driver", "provider": {
      "namespace": "xim", "name": "display-runtime", "version": "1.0.0",
      "source": "xim-pkgindex@<revision>"},
     "path": "${subosdir}/lib/runtime/provider.so",
     "provenance": "subos_view", "abi": "elf-x86_64",
     "digest": "sha256:...", "host_fingerprint": "..."}
  ]
}
```

binding parser 只解析一次 `${subosdir}` 与相对的 artifact 路径，对这些事实
排序，并把它们纳入 contract hash 与 cache snapshot。进入构建计划后，这些
被选中的 provider 事实排在描述符声明的 fallback 之前。描述符的
requirement 与 artifact 分别用各自解析出的 requester / provider 的规范
PackageId 打上印记，因此不同 namespace 下相同的短名在全程都保持互不相同。

这条所有权边界是刻意划定的：mcpp-index 表达的是通用的 runtime 需求；
xlings/xim 负责选择并诊断宿主的图形 / 运行时栈；mcpp 只消费已选定的
provider/artifact 事实，以及通用的 LinkIntent。mcpp 没有硬件、驱动厂商、
WSL 或 ICD 的选择路径。它的源码 gate 拒绝引入这类 provider 专属分支，也
拒绝把这些术语与某个被启动的探针耦合起来。

`LinkIntent` 把 `linkLibraryDirs`、`transitiveNeededDirs` 与
`runtimeSearchDirs` 分开维护。最后一类绝不会被渲染成 `-L`：ELF 只为
transitive 这一类发出 rpath 加 `-rpath-link`，Mach-O 发出
rpath/framework flag，PE 发出链接库路径外加显式的 deploy-file copy
edge。精确的 RuntimeBinding、规范身份、link intent、搜索机制与链接后
判决都持久化在 `resolution.json` schema 2 中。`mcpp why runtime` 只解释
这份存储文件；重新诊断的职责属于 `xlings doctor`。

### 2.3 运行期搜索闭包（`modules/platform/src/runtime_search.cppm`）

mcpp 在编译**与**链接两条命令行上都传 `--sysroot=<subos>`，因此 SubOS
提供的库 —— `-lGL`、`-lX11`、`-lwayland-client` —— 不需要用户加任何 flag
就能解析。运行期搜索路径必须由同一个决策推导出来，否则链接会成功，而
产物却无法启动。

闭包是一张有序列表，每一条都标注了它的来源：

| origin | 例子 | 可变性 | 可随产物再分发 |
|---|---|---|---|
| `payload` | `<store>/xim-x-glibc/2.39/lib64` | 否 —— 安装时写入一次 | 否 |
| `package` | 依赖的 `[runtime]` 目录 | 否 | 否 |
| `subos_farm` | `<subos>/lib` | **是** —— 每次 `xlings install` 都会重写 | 否 |
| `host_default` | `/usr/lib/x86_64-linux-gnu` | 不适用 —— 属于目标机自身 | 不适用 |

**次序按可变性递减排列，farm 排在最后。** 这就是全部的不变式：
payload-first 让 `libc` / `libm` / `libstdc++` 始终从被 pin 住的 payload
解析，只把没人提供的部分留给 farm 补齐。若 farm 排在前面，之后的一次
`xlings install` 就能悄悄换掉一个**已经链接完成**的产物所加载的 libc。

有两道护栏决定 farm 是否适用。格式必须拥有搜索路径（ELF 如此；Mach-O 与
PE 什么都不获得，这与 loader 标签的约定一致），而且目标必须是本宿主的
运行时 —— 交叉目标不会获得 farm，因为 farm 属于宿主 SubOS。

`host_default` 只在产物确实会跑在宿主加载器之下时才进入模型。hermetic
产物的 `PT_INTERP` 指名一个内建默认路径不同的私有加载器，把 `/usr/lib`
算进去，就是在为错误的加载器建模 —— 而开发机通常自带一份 `libGL.so.1`，
于是这样做会把「已解析」报给一个最终以 127 退出的二进制。

闭包被记录在 `resolution.json` 的 `runtime.search.closure` 下，并由
`mcpp why runtime` 打印。一个在 hermetic 产物的路径上找不到任何满足者的
`DT_NEEDED`，是一次**已证明**的失败（`unresolvable`），而不是「没查
清楚」，它会让构建失败。用 `LD_DEBUG=libs` 实测：私有加载器的内建默认
路径是 glibc payload 自己的**构建期**前缀，这个目录在本机并不存在 ——
`/usr/lib` 从不会被查询。

有三点收窄了这一「证明」的范围，而每一点都是 mcpp 实际知道的比措辞暗示
的要少：

- **由产物的格式决定，而不是由 binding 决定。** 一次交叉构建使用的是本
  宿主的 binding，产出的却是 PE 或 Mach-O。无论 binding 怎么说，ELF 规则
  都不适用于这个产物。
- **只有找不到的 SONAME 才能证明什么。**「这个对象我读不了」与「我在
  512 个之后停下了」都是关于**检查本身**的陈述；一次没能看清楚的检查
  什么都没证明，因此这些情形仍报告为 inconclusive。
- **`[build] allow_host_libs` 会同时退出两个阶段。** 它本就关掉了链接期
  的 hermeticity 检查；既然解析的责任已经转交给用户，mcpp 就只报告而不
  阻断 —— 因为他们可能在 `LD_LIBRARY_PATH` 下运行，或者所在的机器上，
  那个库恰好就在私有加载器会去查找的位置。

mcpp 还会为它启动的每一个进程声明 `XLINGS_SUBOS_LD_PATHS=0`。这是对
xlings 链接器包装器路径注入（openxlings/xlings#540）的退出声明：mcpp
想要包装器的 `--disable-new-dtags`，但必须拒绝它的
`-rpath "$XLINGS_SUBOS_LIB"`，因为那个变量指名的是**当前 shell** 的
SubOS —— 而 mcpp 把自己的 xlings home 放在 `<mcpp home>/registry` 之下，
两者通常指向由不同的物理 glibc payload 支撑的不同 farm。mcpp 发出的是它
从自己实际选中的 binding 推导出的那一条 farm 记录。

出于同样的原因，mcpp 自身对 xlings 的调用不携带 `XLINGS_ACTIVE_SUBOS`
（2026.9.14.3 起）。执行过 `xlings subos use <name>` 的 shell 会导出这个
变量，xlings 把它排在 home 自身的 `activeSubos` 之上；一旦被继承，就会让
registry 把 mcpp 的工具与工程的 payload 都装进那个同名的 SubOS，而 mcpp
读取的却是 `subos/default`。

## 3. 链接模型（`modules/toolchain-model/src/linkmodel.cppm`）

`ToolchainLinkModel` 只回答一个问题 —— *mcpp 如何针对这条工具链的 C 库
编译与链接* —— 每一个消费方都从它派生出自己的 flag：

```
CLibMode::PayloadFirst   glibc/linux-headers xpkgs found (the normal bundled-LLVM
                         and no-usable-sysroot GCC case)
                           compile: -isystem (clang) / -idirafter (gcc) payload headers
                           link:    -B <glibcLib>   ← CRT discovery (Scrt1.o/crti.o/crtn.o;
                                                       the driver never consults -L for these)
                                    -L <glibcLib> [+ -rpath + --dynamic-linker for clang]
CLibMode::Sysroot        a usable --sysroot (GCC include-fixed world, self-contained
                         musl sysroots, the macOS SDK)
                           link:    --sysroot, plus --dynamic-linker and -L/-rpath
                                    for the payload when one is known — the
                                    sysroot says where headers live, not which
                                    loader runs the result
CLibMode::None           nothing usable — host defaults apply; the hermetic
                         check (§6) rejects that leakage unless an explicit
                         host-library exception is in effect
```

`ClangDriverModel` 是 bundled LLVM 的配套模型：mcpp 始终传入
`--no-default-config`（绕开装机时生成的 cfg 以保证可复现），并显式重新
提供 libc++ 头文件 / 库，以及
`-fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind`。

**loader 的解析**是数据驱动的，从不硬编码：先查一张按架构分列的 triple
映射表（x86_64 / aarch64 / riscv64 / loongarch64 / i686，glibc 与 musl
两种拼法），再对映射表未收录的架构，用 payload 上的 `ld-*.so*` glob 作为
回退。曾经实现过第三种来源 —— 由安装器持久化的声明式元数据
（`.xpkg-exports.json`）—— 评估之后**被移除**：它唯一的消费者本会是这个
解析器，而上面两级来源已经覆盖了每一个真实 payload（整套 0.0.83 验证
矩阵在这个文件从未存在的情况下全部通过），而一个通用包管理器不应该承载
一套唯一读者只是某个下游工具的机制。如果将来真的出现「已安装状态元数据
库」的需求，它必须以 xlings 自身作为第一消费者来设计，届时 mcpp 可以再
加回一个读取端。

## 4. 统一的 post-install fixup 管线（`src/toolchain/post_install.cppm`）

沙箱 payload 是预先构建好的 ELF 树，其中烙入的 `PT_INTERP`/`RUNPATH` 在
打包期无从得知，必须对齐到**本机**沙箱。
`ensure_post_install_fixup(cfg, payloadRoot, pkg)` 是完成这次对齐的
**唯一入口**，由三条入口路径共同调用：显式 install、默认 auto-install、
manifest auto-install。

> 这条管线过去也会重写 GCC 的 `specs` 文件，以便产物获得 loader 与
> rpath。现在不再这样做，原因见 §5。

> 历史注记：0.0.83 之前，各条路径各自记得 —— 或者忘记 —— 自己那一部分。
> manifest 路径**什么都不跑**，这正是一个刚刚自动安装的 llvm 会保留一份
> 陈旧、随装机环境漂移的 cfg 的原因（issue #195），也是 gcc 曾经产出一个
> 找不到 `stdlib.h` 的沙箱的原因。「用哪条命令装的」绝不能决定「这条工具
> 链好不好用」。

**触发语义 —— 每次构建都询问，只执行一次：**

```
every build → ensure() → read <payload>/.mcpp-fixup.json
                          marker == {schema, kind, rev, glibcLib}?  → return   (ms-level)
                          mismatch → run the fixup for this kind, write marker
```

marker 是一份**内容指纹化的缓存**，不是事件标志：它编码了 fixup 的版本，
以及它对齐时所针对的那个 glibc payload。因此「执行」这一分支在每一个
`(payload × fixup-rev × glibc 指纹)` 组合上恰好触发一次 —— 首次使用，外加
两类确实需要重写的重新收敛事件（经 `kFixupRev` 升级的 fixup 逻辑，或者
底层的 glibc payload 发生变化）。mcpp 之所以每次构建都要问一遍，是因为
使 payload 失效的那些事件（xlings 换掉 glibc、payload 从另一个 home 继承
而来）发生在 mcpp 的视野之外 —— trust-but-verify 是唯一可靠的语义。

**按 kind 划分的动作：**

| kind | actions |
|---|---|
| `gcc`（glibc） | 对 gcc payload **以及共享的 binutils payload** 做一次 patchelf 遍历（PT_INTERP → 沙箱 loader，RUNPATH → glibc + gcc 的 lib 目录）—— 目的是让 *gcc 自己* 能跑起来。不会往 `specs` 里写入任何东西 |
| `llvm` | 只对 `lib/` 做 patchelf 遍历（运行期 `.so` 的 RUNPATH；`bin/` 不动，以保留 xlings 设置的 RUNPATH）；确定性地重新生成 cfg（见 §5） |
| `musl-gcc` | 无操作 —— 自包含 sysroot，静态世界 |

**安全不变量**（每一条都由一次真实事故换来）：

- **绝不就地打补丁。** patchelf 作用于一份副本，随后原子地 `rename()`
  换入：payload 中可能有库被**当前进程**（一个自托管、动态链接的 mcpp）
  或某个并发构建 mmap 着，就地重写一个活映射的 backing file 会损坏正在
  运行的进程（实测现象：退出时 `_dl_fini` 中的 SIGSEGV）。`rename` 让新
  内容获得一个新 inode，活跃进程仍持有旧的那一个。
- **所有权护栏。** 解析到本 home registry 之外的 payload（通过 symlink
  从另一个 `MCPP_HOME` 继承而来）绝不会被打补丁 —— 它们的属主已经完成
  过收敛，隔着 symlink 打补丁会毁掉属主自己的工具链。
- 把内容感知能力扩展到 patchelf 遍历本身（写入前先比对
  `--print-interpreter`/`--print-rpath`，使一个已经对齐的 payload 能以
  **零写入**收敛）是一项已知的后续工作。
- 长期方向是让**安装器**（xlings）拥有全部写入 —— 无论是在安装时，还是
  在 payload 进入一个新 home 时 —— 让 mcpp 退化为只读加校验。在那之前，
  这里的管线是兼容层，也是应对双向漂移的自愈机制。

## 5. 编译器是一种能力，不是一份配置

一个 payload 交付两样可分离的东西：编译的**能力**，以及关于如何链接的
**主张**。mcpp 只要前者，后者自己提供 —— 链接行才是构建决策应该待的
地方，因为它是随每次构建而变化的那部分。两个编译器现在都遵循这条规则，
只是机制不同。

**clang** —— mcpp 的每一次调用都带上 `--no-default-config`。

**gcc** —— 用一份生成出来的文件走 `-specs=`。`<compiler> -dumpspecs`
打印的是**内建**的 specs（不受磁盘上任何文件影响）；mcpp 从中取出
`*link:` 的正文，删掉其中的 loader 与 rpath 行，把结果写入构建目录。一份
不带前导 `+` 的 `-specs=` 文件会**替换**它所命名的那条规则，于是
payload 自身的主张被覆盖，而 payload 本身不受触碰。这带来两条值得说明的
推论：它是逐次构建的，因此同一机器上的另一个工程不受影响；它也不需要对
工具链的写权限，因此继承来的、或只读的 payload 同样可用。

删掉 gcc 烙入的 `*link:`，也就删掉了它原本提供的东西。因此 mcpp 在链接行
上显式补上 `--dynamic-linker` 与每一条 rpath —— loader、glibc 的 lib
目录，以及 gcc 自己的 `lib64`（libgcc_s）。这些都是把 specs 拿掉之后，
逐一观察什么先坏掉而找出来的。

为什么不继续重写 `specs`？因为这个文件是共享的，而它的值却是逐次构建才
确定的。旧的重写方式用一条单路径的 needle 去做双路径的替换，于是每个跑
过它的 home 都会留下一条残留：一台开发机产出的**每一个** gcc 产物里，
都带着 **68** 条陈旧的 `RUNPATH`，全部指向已经被删除的 `mktemp` 目录。
没有任何东西发现它们，因为一条失效的 RUNPATH 只多花一点搜索时间。e2e
`201_gcc_no_specs_pollution.sh` 断言的是**产物**，而不是 specs 文件 ——
用户最终交付的是产物。

### 5.1 clang 的 cfg

`bin/clang++.cfg` 的存在，是为了让直接调用打包内 `clang++`（不经由
mcpp）的场景，也能得到一份可用、hermetic 的编译器配置。fixup 管线从
链接模型**确定性地重新生成**它 —— 同一个 payload 在任何机器、任何安装
路径上都产出字节相同的 cfg —— 而不是对某次安装产出的文件做行级修补。在
Linux 上，它包含 CRT 发现（`-B`）、payload loader 加 rpath、
lld/compiler-rt/libunwind，以及供 C++ 驱动使用的 bundled libc++；在
macOS 上，它保留历史形态（`--sysroot=<SDK>` 加 payload 的 libc++ 头
文件 —— C++ **运行时链接**仍由主构建中平台专属的
`needs_explicit_libcxx` 处理）。

## 6. hermetic 链接检查（`src/build/hermetic.cppm`）

在 Linux 上用沙箱工具链开始构建之前，mcpp 会用真实的链接 flag 对 driver
做一次干跑（`-### -x c++ /dev/null`），并断言每一个 CRT 对象，以及
**生效的**动态链接器（以最后一次出现为准），都解析到允许的沙箱前缀之
下。这把两种静默的故障模式变成了一条可以据以行动的诊断：lld 打不开的裸
CRT 名字（干净机器上的 #195 症状），以及静默的宿主 CRT 污染（它曾让带有
宿主工具链的机器上的绿色 CI 成为假信号）。判定结果按 flag 集合缓存
（`.mcpp-hermetic-ok`）；逃生舱是 `[build] allow_host_libs = true` 或
`MCPP_ALLOW_HOST_LIBS=1`。系统 / PATH 编译器豁免此项检查 —— 显式选择
宿主世界是用户自己的决定。

链接依赖图提供的 C 库时还要满足一条规则（mcpp#696，2026.9.26.1+）。这样的链接
带有 `--sysroot=<构建目录>/graph-sysroot`，一个空目录，驱动器因此不会从宿主推导
任何库目录；检查还会读取试运行中的 `-L` 目录：每一个都必须位于工具链存储、构建目录
或依赖图中的某个包之内。判定结果的缓存键记录链接是否属于这一类，以及它允许的包根。

CI 用一个**完全没有宿主工具链**的 job（`debian:stable-slim`，没有 gcc，
也没有宿主的 `Scrt1.o`）守住这一点 —— 它是唯一能忠实复现干净机器故障
模式的环境类别，另外还有 e2e `86_llvm_hermetic_link.sh`，在任何机器上
重新检查 `-###` 的解析结果。

### 6.1 链接后的 Linux 运行时物理校验（`elf_runtime.cppm`）

hermetic 检查回答的是一个链接前的问题：driver 看起来会解析到什么？运行
时物理校验回答的是更强的链接后问题：新产出的 ELF 实际记录了什么，它的
闭包将会加载什么？`[build] allow_host_libs = true` 有意放宽第一项检查，
但不会压制第二项检查所证明的物理上的不可能。

对每一个新链接的 Linux 可执行文件 / 共享对象，mcpp 在进程内部解析
ELF64 小端序的 program/dynamic/GNU-version 表 —— 构建路径上不启动
`readelf`、`patchelf` 或 `ldd` 子进程 —— 并记录 `PT_INTERP`、
`DT_RPATH`/`DT_RUNPATH`、`DT_NEEDED`，以及所需 / 已定义的 `GLIBC_*`
版本。它用产物的搜索路径、已选定的 runtime/toolchain 目录，以及已知的
宿主库目录来解析声明的闭包，随后施加：

- **规则 B（同源）：** `PT_INTERP` 与每一个被解析到的 `libc.so.6`，都
  必须是 `RuntimeBinding` 选定的那份规范 payload。宿主 loader 配私有
  libc，或者两份私有 libc payload，都是一次已证明的、在进入 `main` 之前
  就会发生的故障。
- **规则 A（版本下限）：** 闭包中对 `GLIBC_x.y` 的每一次请求，都不得新于
  所选 libc 导出的 GNU version definitions。满足这一条时，链接一个宿主
  DSO 是允许的；mcpp 校验的是物理事实，不是在强加一条「禁止宿主库」的
  策略。

判定结果是类型化的：`Pass`、`ProvenMismatch` 或 `Inconclusive`。已证明
的 A/B 不匹配会使构建失败，并给出规范化的 requester/provider/artifact
路径，以及一份可以直接复制执行的 SubOS 修复步骤。缺失 loader-cache 或
硬件闭包数据时，会被报告为 inconclusive，绝不会被重新标记成绿色。macOS
与 Windows 复用同一接口，但作为一个类型化的空操作，永远不会套用
ELF/glibc 规则。

判定结果以 `.mcpp-runtime-verdicts.json` 的形式存放在 `build.ninja`
旁，键由产物的 stat 指纹加上完整的 runtime contract hash 组成。热的空
操作构建要求已经存在一条当前的通过记录，比较 Ninja 前后的产物 stat，并
执行零次 ELF 解析。一次意外的重新链接会先回落到完整路径，才允许成功或
执行。`mcpp self doctor` 报告的是同一份存档判定，而不是针对可能已经
变化的当前宿主重新探测一次。

装后对齐遵循同一条身份规则：`glibc@2.44` 只解析
`<xpkgs>/xim-x-glibc/2.44/{lib64,lib}`。精确 payload 缺失或陈旧是一个
错误；另一个已安装的版本绝不会被当作回退。

精确的 payload 由 mcpp 在第一次需要针对它打补丁的 fixup 之前安装
（`ensure_declared_runtime`，mcpp#660）。这一步只在以下条件同时成立时
执行：binding 找不到 payload、宿主是 Linux、provider 是 `glibc`，且该
工具链的 fixup 需要消费一份 C 运行时（`gcc` 与 `llvm` 的 payload）——
因此使用系统、musl 或 PE 工具链的构建不会下载任何东西。它通过 xlings
安装 `xim:glibc@<声明的版本>`，然后重新解析 binding。在这一步存在之
前，payload 只有在 xlings 把它当作某个工具链的依赖装上时才会出现；从
CI 缓存恢复的工具链不会被重新安装，因此当缓存中是另一个 glibc 修订版
时，声明的 payload 就会缺席。当安装不可能完成时（例如处于离线状态），
fixup 的报错会写出需要预先提供的坐标 `xim:glibc@<版本>`。

### 6.2 运行时搜索路径的允许位置（`runtime_env_contract.cppm`）

告诉加载器该去哪里查找有两种方式，二者的区别不在于是否方便，而在于
**波及范围**：

| | 波及范围 | |
| --- | --- | --- |
| `DT_RUNPATH` | 携带它的那一个对象，以及该对象的 `dlopen()` | 逐二进制 |
| `LD_LIBRARY_PATH` | 该进程，**及其派生的每一个进程** | 被继承，且永久继承 |

第二行正是私有 libc payload 必须是**二进制作用域**的原因。一份 glibc 的
`libc.so.6` 与它的 `ld.so` 通过 `GLIBC_PRIVATE` 版本锁定在一起：2.44 的
libc 携带一个未定义的 `__pointer_chk_guard`，只有 2.44 自己的 loader 才
导出它。mcpp 构建出的程序没有问题 —— 它的 `PT_INTERP` 指名的正是这个
私有 loader。`/bin/sh` 则不然：它的 `PT_INTERP` 指名的是**宿主**的
loader，任何环境变量都无法覆盖它，因此一个 `popen()`/`system()` 的子
进程会在重定位期间、还没进入 `main` 之前就死掉，且没有任何输出
（mcpp#401；mcpp#291 是形状相同的问题往内靠近了一跳，杀掉的是 mcpp
自己嵌套的宿主工具）。

因此 mcpp 从不通过环境变量发布私有 libc 目录。它也不需要这样做：只要
存在一份 payload，链接模型就已经在 `--dynamic-linker` 旁边发出了
`-Wl,-rpath,<glibc>`，这已经覆盖了这个目录存在的唯一理由 —— 某个
`dlopen()` 自身的 `DT_NEEDED` 闭包不会去查阅可执行文件的 RUNPATH。

这是一个**作用域**问题，不是条件判断。「只在某个依赖可能 `dlopen()`
时才导出它」仍然是导出，而那个死掉的子进程并不在乎原因。普通依赖的
运行时目录保持自己的环境作用域：它们与 loader 没有耦合，因此一个偶然
撞上它们的宿主二进制，最坏也只是感到困惑。

## 7. 扩展这套机制

### 7.1 新增一个工具链（新的编译器家族或发行版）

1. **索引侧**（xim-pkgindex）：一个携带 payload 资产的包，并且 —— 这一
   点至关重要 —— 声明对它所需的 C 库 payload 的 `deps`（`xim:glibc`、
   `xim:linux-headers`）。遵循 llvm/gcc 的打包 SOP，包括准入 gate
   （`verify-toolchain.sh`）：完整性 + hermetic CRT 解析 + 一次真实的
   编译/链接/运行，通过之后资产才能发布。
2. **词汇表 + 注册表**：先把目标行加入 `triple.cppm` 的
   `kKnownTargets`（档位/pin/defaultStatic），再让 `to_xim_package`
   （`src/toolchain/registry.cppm`）认识 *(family, target, host)* 到
   xim 包行的映射，以及它的 `frontendCandidates`（哪个二进制是 C++
   驱动）。若有旧拼法，一律只放进 `compat.cppm`。
3. **能力**（`src/toolchain/provider.cppm`）：stdlib 身份、BMI 特性，
   以及 `flags.cppm` 消费的 feature 开关。
4. **fixup kind**（`post_install.cppm`）：确定这个 payload 需要哪种
   装后对齐 —— gcc 式（patchelf + specs）、llvm 式（lib patchelf +
   cfg），或者不需要（自包含）。把它接入 `ensure_post_install_fixup`
   的分发逻辑。
5. **e2e**：按 `86_llvm_hermetic_link.sh` 的思路写一个 hermetic 链接
   测试，并纳入无宿主工具链的 CI job。

### 7.2 新增一个 CPU 架构（Linux）

这套机制已经按架构参数化；剩下的工作只是数据：

1. 把该架构的 glibc/musl loader 名字加进
   `linkmodel.cppm::loader_filename` 的 triple 映射表（在此之前，glob
   回退一直有效）；
2. 为该架构发布 payload 资产（glibc、linux-headers、工具链本体）——
   aarch64-linux-musl 交叉目标是现成的先例（`[target.aarch64-linux-musl]`，
   交叉前端经由 spec 的 `targetTriple` 解析）；
3. 其余什么都不用做：`-B`/`-L`/loader 的发出、fixup 管线，以及
   hermetic 检查全都与具体名字无关。

### 7.3 嵌入式与裸机工具链

`riscv64-none-elf` 与 `riscv32-none-elf` 已经实现，面向用户的说明见
[40 —— 裸机与 freestanding 目标](40-baremetal.md)。本节记录由此得到的
形态，与上文 hosted 模型之间的关系。

本节早先做出的三条预测都成立：

- **没有动态链接器。** `loader` 保持为空，每个渲染器本来就允许这一点；
  部署方式是烧录，而不是 ELF interp。
- **目标侧不需要 fixup。** 宿主上运行的编译器二进制仍然需要
  PT_INTERP/RUNPATH 对齐，与今天的 gcc kind 完全相同。
- **MCU flag、linker script 处理与运行方式确实需要全新设计。** 三者均
  已落地，而且都如预测那样位于本文档所述层次之上：ISA flag 来自
  `src/freestanding/target.cppm` 中每个目标一行的表，linker script 经
  `link-script` 构建指令到达，执行方式经 `runner` 到达。

有一条预测是错的，而这次纠正正是整个设计中承重的那一部分。C 库**不**
存在于工具链 payload 之内，因此这并不是换了一个 sysroot 的
`CLibMode::Sysroot`。picolibc 是一份独立的 payload，由目标自己的表行
指名（`modules/toolchain-model/src/triple.cppm` 中的
`sysroot = xim:picolibc-riscv@1.8.12`），与该行的编译器 `pin` 处于
同一地位。从目标而不是从工具链解析它，正是裸机**包**不必指名一份
libc 的原因，如同 hosted 包从不指名 glibc。

freestanding 的链接行同样是被**替换**而不是被扩展 —— 见
`src/freestanding/linkline.cppm` —— 因为在那里，每一条 hosted 链接
flag 都是错的，而不只是多余的。管线更早处追加到普通 ldflags 上的任何
内容都会被丢弃，这也是目标 sysroot 的 `-L` 在这一行单独发出、而不与
通用 flag 一起发出的原因。

### 7.4 非 ELF 平台

macOS（Mach-O）与 Windows（PE）有意绕开本文档的大部分内容：macOS 从
SDK 解析它的 C 世界（`CLibMode::Sysroot`），并有自己的 libc++ 链接
处理方式；Windows 没有 rpath —— mcpp 把运行时 DLL 部署到产出的 exe
旁边，这正是该平台上，与 §3–§4 对 ELF 所做的一切等价的原生方式。

**工程自己产出的共享库**（`kind = "shared"`）确实按格式而有所不同，
而这个差别不在于 flag 的拼法 —— 而在于产物记录了关于自己的什么：

| 格式 | 生产方的产出 | 消费方链接时的输入 |
|---|---|---|
| ELF | 声明了 soname 时发出 `-Wl,-soname,<n>` | `-L` + `-l`、`-Wl,-rpath,$ORIGIN` |
| Mach-O | **始终**发出 `-Wl,-install_name,@rpath/<file>` | `-L` + `-l`、`-Wl,-rpath,@loader_path` |
| PE / MinGW | `-Wl,--out-implib,<lib>` | **导入库**，且 `-Wl,-Bdynamic` 在前 |
| PE / MSVC | 拒绝（不做自动导出；见 docs/12） | —— |

其中三行是新增的：此前除 ELF 之外全部被拒绝，无论原生还是交叉。要让
它们可用而不只是被允许，有两处细节必须改变。Mach-O 的 install name
默认取该库被**链接**时所在的路径，因此「只在声明了 `soname` 时才发出
它」会让其余每一个 `.dylib` 都记下一个构建目录 —— 在构建它的那台机器
上没问题，换到任何其他机器就是 `image not found`。而这个选择原本是在
**宿主**上用 `#if defined(__APPLE__)` 做出的，这只在原生 macOS 构建上
碰巧是对的，对任何交叉链接都是错的；现在它改由目标决定，如同
`target_output` 早已如此。

**无法服务的目标会被拒绝**，而不是悄悄按宿主构建：在 Linux 上，
`--target x86_64-windows-msvc` 过去会解析到原生的 `g++`，写进
`target/x86_64-linux-gnu/`，并报告成功。词汇表的档位说的是「mcpp 支持
这个目标」；`host_can_serve`（`registry.cppm`）回答的是另一个问题 ——
「这台机器能不能产出它」，而 `prepare.cppm` 现在会去问这个问题 —— 作者
可以用显式的 `[target.X] toolchain = "…"` 作为提供自备交叉工具链时的
出口。

### 7.5 决定一个 flag 的轴

2026.8.18 那一轮改动涉及四个 flag，每一个此前都被挂在了错误的轴上。这些
错误全都以同一种方式暴露：在恰好一个平台上出现无法解释的失败，而报错既
不点名那个 flag，也不点名它背后的决定。

一共存在三根轴，在它们之间做选择的问题是：**这个 flag 最终由谁读取。**

| 轴 | 要问的问题 | 例子 | 查询方式 |
|---|---|---|---|
| **目标格式** | 产出的是哪一种镜像 | `-fPIC`（PE 代码按设计就是位置无关的；clang 直接拒绝这个 flag） | `triple::parse(...)->is_pe()`，宿主兜底 |
| **目标 ABI** | 哪个链接器会消费它 | `--out-implib` 与 `/IMPLIB:`、`/DEF:`，SONAME / install-name 的形式 | `is_msvc_target(tc)`、`triple->is_msvc_env()` |
| **dialect** | mcpp 调用的是哪个程序 | `-L` 与 `/LIBPATH:`、`-I` 与 `/I`，归档命令 | `dialect_for(tc)`、`LinkStyle::SeparateLinker` |

**面向 MSVC ABI 的 clang，是同时区分这三根轴的那个例子。** 它说 GNU
dialect、产出 MSVC-ABI 的对象、生成 PE 镜像。问错轴的后果是：

- 按 dialect 判定，它会拿到 `-Wl,--out-implib`，lld-link 报出
  `ignoring unknown argument`，紧接着是文件缺失；
- 按 ABI 判定，它会拿到 `/LIBPATH:`，而编译器驱动不接受这个写法；
- 按编译器二进制判定，它会拿到 `-fPIC`，直接拒绝运行。

故障的形状总是相同的：flag 按相邻平台的拼法发出，而诊断信息来自一个与
那次决定相隔三步的程序。链接器侧的答案集中在 `ninja_backend` 的
`pe_link_flag` 里；dialect 表在它原来那个条目所在的位置，写明了自己
为什么答不了这些问题。

## 8. 源码地图

| 关注点 | 文件 |
|---|---|
| spec → xim 包、前端 | `src/toolchain/registry.cppm` |
| detect/probe（triple、sysroot、payload） | `src/toolchain/detect.cppm`、`probe.cppm` |
| 链接模型 + loader 解析 | `modules/toolchain-model/src/linkmodel.cppm` |
| 统一 fixup 管线（patchelf/specs/cfg、marker） | `src/toolchain/post_install.cppm` |
| install/lifecycle 入口 | `src/toolchain/lifecycle.cppm`；auto-install 入口在 `src/build/prepare.cppm` |
| root runtime 选择 / binding | `src/xlings/runtime_selection.cppm`、`src/runtime/binding.cppm`、`src/xlings/subos_info.cppm` |
| 通用 runtime contract + LinkIntent | `modules/manifest/src/types.cppm`、`src/build/plan.cppm`、`src/build/flags.cppm` |
| 已存储的 resolution 解释 | `src/build/prepare.cppm`、`src/build/runtime_validation.cppm`、`src/doctor.cppm` |
| flag 组装（主构建） | `src/build/flags.cppm` |
| `import std;` 预编译 | `src/toolchain/stdmod.cppm` |
| build.mcpp 宿主 flag | `src/build/build_program.cppm` |
| hermetic 链接检查 | `src/build/hermetic.cppm` |
| 回归护栏 | `tests/e2e/86_llvm_hermetic_link.sh`、单元测试 `test_linkmodel.cpp`、`test_post_install.cpp`；`ci-linux-e2e.yml` 中的无宿主工具链 CI job |
