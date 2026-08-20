# 08 — 工具链机制内幕

> 本文详细描述 mcpp 工具链机制的内部工作原理,以及如何扩充新工具链、新架构乃至
> 嵌入式目标的支持。与面向用户的 [03 — 工具链管理](03-toolchains.md)(CLI 用法)
> 互补,本文面向贡献者与维护者。

## 1. 一张图看全模型

```
mcpp.toml [xlings].subos / mcpp 管理的默认运行时
        ▼
解析 runtime binding             ← 产物将加载哪个 libc(§2.1)——是答案,不是搜索
        ▼
解析工具链 payload               ← 项目/default/install 入口共享同一管线
        ▼
ensure_post_install_fixup()      ← 精确 glibc@version、marker 闸门,不取 readdir 首项
        ▼
detect / probe                   ← triple、sysroot、payload 路径(glibc、linux-headers)
        ▼
ToolchainLinkModel(C 库轴的唯一解析器)
        ├──► flags.cppm        (主构建编译/链接 flags)
        ├──► stdmod.cppm       (`import std;` BMI 预编译)
        ├──► build_program     (build.mcpp 宿主编译)
        └──► cfg 再生          (供人类直接使用的 clang++.cfg)
        ▼
hermetic 链接校验(`-###` 干跑)  ← 校验沙箱 CRT/loader 的解析结果
        ▼
链接 → 内部 ELF 物理校验        ← 校验真实产物及解析闭包(§6.1)
```

贯穿一切的两条原则:

1. **沙箱工具链默认接受 hermetic 校验。** 在正常的 payload-first 或 sysroot 路径中,
   产物的 CRT 启动对象、libc、动态链接器必须解析在允许的沙箱前缀内。这不是无条件的
   隔离保证:`CLibMode::None` 会落到宿主默认,系统/PATH 编译器是显式选择宿主世界,
   `[build] allow_host_libs = true` 或 `MCPP_ALLOW_HOST_LIBS=1` 会退出宿主库校验。
   在没有编译器、没有 `/usr/lib/**/Scrt1.o` 的机器(全新 WSL2、精简容器)上,正常的
   沙箱路径仍可工作。
2. **每层路径知识只有一个属主。** 过去"如何对 payload glibc 链接"有四份漂移副本,
   现在收敛为一个解析器(`linkmodel`);过去 fixup 行为按入口路径各自为政,现在
   是一条管线。副本间漂移正是一整类 bug 的来源(issue #195)。

## 2. 工具链解析

自 0.0.93 起,身份由两条正交轴构成:`ToolchainSpec` 是
`(family ∈ gcc|llvm|msvc,version,target Triple)`。`triple.cppm` 负责唯一的
triple 解析器与封闭的已知 target 词汇表;`compat.cppm` 只处理旧拼写
(`gcc@15.1.0-musl`、`musl-gcc`、`mingw`、`mingw-cross`、`clang`、
`<triple>-gcc`),解析时归一化且永久兼容。`to_xim_package` 把
`(family,target,host)` 映射为含 xim 包名、版本、前端候选的
`XimToolchainPackage`;这也是 Linux 宿主的 `mingw-cross-gcc` 与 Windows
宿主的 `mingw-gcc` 等分发层名称存在的位置,它们不是面向用户的写法。payload 经
xlings 后端解析/自动安装到沙箱
(`$MCPP_HOME/registry/data/xpkgs/xim-x-<name>/<version>/`)。

`detect`/`probe`(`src/toolchain/detect.cppm`、`probe.cppm`)随后推导:

| 字段 | 方式 |
|---|---|
| `targetTriple` | `<compiler> -dumpmachine` |
| `sysroot` | `-print-sysroot`(校验必须真带 libc 头);xlings 构建的 GCC 烙的是构建机路径,有 remap 回退 |
| `payloadPaths` | 由解析出的 runtime binding(§2.1)**精确指名** glibc payload;linux-headers 仍按兄弟 xpkg 发现。没有 binding 就不走 payload-first——这是设计,不是缺陷 |
| 运行库目录 | 工具链私有 lib 目录,用于产物的 `-L`/`-rpath` |

### 2.1 runtime binding:绑哪个 libc,只决定一次

payload-first 的构建会链接到某个具体的 glibc,而**是哪一个**是根项目本地开发 OS 的事实,
不该从编译器路径或 shell 状态推断。mcpp 只有两种选择:

1. 未声明 `[xlings].subos`:使用全局 mcpp 配置所选 xlings home 中已初始化的
   `McppDefault` (`subos/default`)。
2. `[xlings] subos = "<name>"`:使用 `NamedSubos(name)`;显式 `"default"`
   仍保留命名选择身份,其他名称解析到根项目的本地 xlings scope。

workspace 构建由 workspace root 选择;member 与 dependency 声明不合并、不传递。
`XLINGS_ACTIVE_SUBOS`、current、编译器 owner home 以及 CLI/环境 override 都不是第三层。
**矛盾报错,缺席降级。** 点名的环境不存在是 hard error —— 该请求无法被满足,
换一个环境会让同一份 `mcpp.toml` 在不同机器上意味着不同 ABI,绝不回退
default/active/编译器烙入状态。

而「环境存在但没有自我描述」是另一回事,它**降级**:记 `declared = false` 与一条
调用方必须打印的 note;runtime 规则报 `inconclusive` 而不是给出判决;没有可用的
payload-first binding(mcpp 拒绝猜一个 libc 版本,因而 hermeticity 检查会如实报告
回退到宿主)。**构建不被中止。** 同理,`subos_info` 的 schema **高于**本 mcpp 所理解的
版本时,读懂的字段照用并附 note —— 这正是读取器自己写下的规矩:**发布数据不得使
读它的程序失效**。直接拒绝就是 2026.8.10.2 在 Windows 上停掉每一次 `mcpp build` /
`mcpp test` 的原因(那里 xlings 根本不写这个 block,它承载的事实也不存在,
openxlings/xlings#543)。

该 contract 只读取一次形成 `RuntimeBinding` snapshot,
由 configure/link/run/test 与 fast-path cache 共同使用。
Linux snapshot 还记录所选 loader/libc 目录的规范路径及可选的创建宿主 glibc floor;
它们是链接后校验的证据,不是新的选择入口。

当解析后的 SubOS view 可规范化到一个受管 glibc payload 时，该真实 view 是权威事实。
旧 xlings 状态可能在 view 已原子切到受管 2.44 后仍保留 `runtime = "glibc@2.39"`；此时
mcpp 在同一 binding 中记录真实的 2.44 身份与路径。若旧 view 是断链，mcpp 只允许解析
`runtime` 精确指名的 payload；不会枚举已安装版本，也不会挑“最近/最新”版本。两条路径
都只消费 xlings 给出的事实，不引入 mcpp 自己的运行时选择策略。

没有 binding 是**拒绝**而不是取默认值:`CLibMode::PayloadFirst` 会被放弃,而不是
去挑一个 libc。

被它取代的旧规则是:向某个目录问"那个 glibc",然后拿 `readdir` 吐出的第一项。
只装了一个 glibc 时它永远正确,所以从来没有东西逼它正确——而一条带
`xim:glibc@>=2.38` 的依赖就足以装进第二个。这在 mcpp-index 上真实发生过:编译侧
取了 2.44,而产物的 interpreter(装机时冻结在 gcc specs 里)仍指向 2.39;二进制
引用了 `GLIBC_2.42` 符号,却跑在没有这些符号的运行时上,且报错落在与"拉进第二个
glibc 的那条依赖"毫无关系的包上。目录顺序不是决策依据。

因为 binding 决定产物加载什么,完整规范化 contract hash 是工具链指纹第 11 个字段——
即使两个命名 SubOS 都使用同一 glibc,只要 provider 或环境声明不同也绝不能共用缓存。

注意:probe 已**不再**从 clang cfg 挖 `--sysroot`——cfg 是这套机制的输出,
不是输入(见 §5)。

### 2.2 通用运行时 provider 与 artifact

`RuntimeBinding` 还携带 xlings 为所选开发 OS 解析好的 provider-neutral fact。
`subos_info.runtime_contract` 是 schema 1 上可选、可加的 block:

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

binding parser 只解析一次 `${subosdir}` 和相对 artifact 路径,排序 fact,并把它们纳入
contract hash 与 cache snapshot。进入 BuildPlan 后,所选 provider fact 排在描述符
fallback 前。描述符 requirement/artifact 则由 resolver 分别用 requester/provider 的
canonical PackageId 盖章,所以不同 namespace 下相同短名不会在任何环节碰撞。

所有权边界是刻意的:mcpp-index 表达通用 runtime requirement;xlings/xim 选择并诊断
宿主图形/运行时栈;mcpp 只消费已选 provider/artifact fact 与通用 LinkIntent。
mcpp 不含硬件、driver vendor、WSL 或 ICD 选择路径;源码 gate 会拒绝引入这类
provider-specific 分支,也拒绝把相关词汇与外部 probe 启动耦合。

`LinkIntent` 分开 `linkLibraryDirs`、`transitiveNeededDirs` 和
`runtimeSearchDirs`;最后一类绝不渲染为 `-L`。ELF 只为 runtime 目录发 rpath,
且仅为 transitive 类发 `-rpath-link`;Mach-O 发 rpath/framework;PE 发链接库路径并
用显式 deploy-file copy edge。精确 RuntimeBinding、canonical identity、LinkIntent、
搜索机制与链接后 verdict 写入 `resolution.json` schema 2。
`mcpp why runtime` 只解释该存储文件;重新诊断由 `xlings doctor` 负责。

### 2.3 运行期搜索闭包(`src/platform/runtime_search.cppm`)

mcpp 在**编译与链接**两条线上都发 `--sysroot=<subos>`,所以 subos 提供的库
(`-lGL`、`-lX11`、`-lwayland-client`)零 flag 就能解析。运行期的搜索路径必须由
**同一个决策**推导,否则就是「链得上、跑不起来」。

闭包是一张有序表,每条带来源:

| origin | 例子 | 可变? | 可随产物分发? |
|---|---|---|---|
| `payload` | `<store>/xim-x-glibc/2.39/lib64` | 否 —— 装一次不再动 | 否 |
| `package` | 依赖描述符的 `[runtime]` 目录 | 否 | 否 |
| `subos_farm` | `<subos>/lib` | **是** —— 每次 `xlings install` 重写 | 否 |
| `host_default` | `/usr/lib/x86_64-linux-gnu` | 目标机自己的 | 不适用 |

**次序 = 不可变性递减,farm 在最后。** 这是本模块唯一的不变式:载荷在前,
`libc` / `libm` / `libstdc++` 永远从被 pin 的载荷解析,farm 只补没人提供的那些;
farm 在前的话,一次 `xlings install` 就能在事后悄悄换掉一个**已经链接完成**的产物
所加载的 libc。

两条护栏决定 farm 是否适用:格式必须有搜索路径(ELF;Mach-O 与 PE 什么都不发,
与加载器标签契约同一形状),且目标必须是本宿主的运行时 —— 交叉目标不发 farm,
因为 farm 属于宿主 subos。

`host_default` 只在产物**确实**跑在宿主加载器下时才进模型。hermetic 产物的
`PT_INTERP` 指向私有加载器,它的内建默认路径不同;把 `/usr/lib` 算进去就是在模拟
另一个加载器 —— 而开发机通常自带 `libGL.so.1`,于是「解析到了」会被报给一个
退出 127 的产物。

闭包记录在 `resolution.json` 的 `runtime.search.closure`,并由 `mcpp why runtime`
打印。hermetic 产物上一个谁都提供不了的 `DT_NEEDED` 是**可证的**失败
(`unresolvable`),不是「没查过」,它会让构建变红。`LD_DEBUG=libs` 实测:私有加载器的
内建默认路径是 glibc 载荷**自己的构建期前缀**,该目录在本机并不存在 —— `/usr/lib`
从不被查。

有三件事收窄这个「可证」,每一件都是 mcpp 知道的比措辞暗示的少:

- **产物的格式由产物决定,不由 binding 决定。** 交叉构建用的是本宿主的 binding,
  产物却是 PE / Mach-O;无论 binding 怎么说,ELF 规则都不适用于它。
- **只有「找不到的 SONAME」能证明什么。**「这个对象读不了」「我在 512 个之后停了」
  都是关于**检查**的陈述;没能看的检查什么都没证明,故仍报 inconclusive。
- **`[build] allow_host_libs` 同时退出两个阶段。** 它本就关掉链接期 hermeticity 检查;
  既然解析责任已归用户,mcpp 就只报告不阻断 —— 他们可能用 `LD_LIBRARY_PATH` 跑,
  或装在私有加载器确实会看的地方。

mcpp 还会给它启动的每个进程声明 `XLINGS_SUBOS_LD_PATHS=0` —— 这是 xlings 链接器
包装器路径注入的退出声明(openxlings/xlings#540):mcpp 要它的
`--disable-new-dtags`,但必须拒绝它的 `-rpath "$XLINGS_SUBOS_LIB"`,因为那个变量
指的是**当前 shell 的** subos,而 mcpp 有自己的 xlings home
(`<mcpp home>/registry`),两者通常指向由**不同物理 glibc 载荷**支撑的不同 farm。
mcpp 发的是自己从已选 binding 推导出来的那一条。

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
CLibMode::None           无可用来源——落宿主默认;除非显式允许宿主库,否则由
                         hermetic 校验(§6)拒绝该泄漏
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

沙箱 payload 是预编译 ELF 树,其烙入的 `PT_INTERP`/`RUNPATH` 在打包期不可能预知,
必须对齐到**本机沙箱**。
`ensure_post_install_fixup(cfg, payloadRoot, pkg)` 是这次对齐的**唯一入口**,
由三条入口路径(显式 install、默认 auto-install、manifest auto-install)共同
调用。

> 本管线曾同时重写 GCC 的 `specs` 文件,以便产物拿到 loader 和 rpath。现已不再
> 这样做,原因见 §5。

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
| `gcc`(glibc)| 对 gcc payload **及共享的 binutils payload** 做 patchelf 遍历(PT_INTERP → 沙箱 loader,RUNPATH → glibc+gcc lib)——目的是让 *gcc 自己* 能跑起来。不往 `specs` 里写任何东西 |
| `llvm` | 只遍历 `lib/`(运行库 `.so` 的 RUNPATH;`bin/` 不碰,保留 xlings 设置的 RUNPATH);确定性再生 cfg(§5)|
| `musl-gcc` | 无——自包含 sysroot,静态世界 |

**安全不变量**(每条都由真实事故换来):

- **绝不就地写。** patchelf 作用于副本,再原子 `rename()` 换入:payload 里可能
  有**当前进程**(自托管、动态链接的 mcpp)或并发构建正 mmap 着的库,就地重写
  活映射的 backing file 会损坏运行中的进程(实测:退出时 `_dl_fini` 段错误)。
  `rename` 让新内容拿新 inode,活进程保有旧 inode。
- **所有权护栏。** 解析到本 home registry 之外的 payload(从别的 `MCPP_HOME`
  symlink 继承)一律不碰——属主已收敛过,隔着 symlink 改写会毁掉属主的工具链。
- 把内容感知扩展到 patchelf 遍历
  (写前比对 `--print-interpreter`/`--print-rpath`,已对齐的 payload **零写入**
  收敛)是已知的后续项。
- 长期方向:全部写入由**安装器**(xlings)持有——装机时、以及 payload 进入
  新 home 时——mcpp 退为只读 + 校验。本管线在那之前是兼容层,也是双向漂移的
  自愈机制。

## 5. 编译器是能力,不是配置

一个 payload 交付两样可分离的东西:编译的**能力**,以及关于如何链接的**主张**。
mcpp 要前者,后者自己给——链接行才是构建决策该待的地方,因为它是逐次构建而变的
那一个。现在两个编译器都遵循这条规则,只是机制不同。

**clang** —— mcpp 每次调用都带 `--no-default-config`。

**gcc** —— 用生成的文件走 `-specs=`。`<compiler> -dumpspecs` 打印的是**内建**
specs(不受磁盘上任何文件影响);mcpp 从中取出 `*link:` 的正文,删掉其中的 loader
与 rpath 行,把结果写进构建目录。不带前导 `+` 的 `-specs=` 文件是**替换**它所命名的
规则,于是 payload 自己的主张被覆盖,而 payload 本身分毫未动。两条推论值得写明:
它是逐构建的,所以同机器上的另一个项目不受影响;它不需要对工具链的写权限,所以
继承来的、或只读的 payload 都能用。

删掉 gcc 烙入的 `*link:`,也就删掉了它提供的东西。因此 mcpp 在链接行上显式补齐
`--dynamic-linker` 与每一条 rpath——loader、glibc lib 目录、以及 gcc 自己的
`lib64`(libgcc_s)。这三条都是把 specs 拿掉后、看什么先坏掉而逐一找出来的。

为什么不继续重写 `specs`?因为那个文件是共享的,而值是逐构建的。旧的重写用的是
单路径的 needle 配双路径的 replacement,于是每个跑过它的 home 都会漏下一条:一台
开发机产出的**每一个** gcc 产物里都带着 **68** 条陈旧 `RUNPATH`,全部指向已被删除的
`mktemp` 目录。没有任何东西发现它们,因为一条死 RUNPATH 只多花一点搜索时间。e2e
`201_gcc_no_specs_pollution.sh` 断言的是**产物**而不是 specs 文件——用户最终交付的
是产物。

### 5.1 clang cfg

`bin/clang++.cfg` 的职责是:直接调用打包内 `clang++`(不经由 mcpp)时,
获得可用且 hermetic 的编译器配置。
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

### 6.1 链接后的 Linux 运行时物理校验(`elf_runtime.cppm`)

hermetic 校验回答的是链接前问题:driver 看起来会解析到什么。运行时物理校验回答更强的
链接后问题:新 ELF 真正记录了什么,其闭包实际会加载什么。
`[build] allow_host_libs = true` 会有意放宽前者,但不会屏蔽后者已经可证明的物理矛盾。

对每个新链接的 Linux executable/shared object,mcpp 在进程内解析 ELF64 little-endian
program/dynamic/GNU version 表;构建路径不启动 `readelf`、`patchelf` 或 `ldd`。读取内容包括
`PT_INTERP`、`DT_RPATH`/`DT_RUNPATH`、`DT_NEEDED` 以及所需/导出的 `GLIBC_*` 版本。
随后按产物搜索路径、所选 runtime/toolchain 目录和已知宿主库目录解析闭包,并执行:

- **规则 B(同源):** `PT_INTERP` 和闭包中每个 `libc.so.6` 必须都来自
  `RuntimeBinding` 选定的规范 payload。宿主 loader + 私有 libc,或两个私有 libc payload,
  都是可证明的 pre-main 故障。
- **规则 A(版本 floor):** 闭包请求的每个 `GLIBC_x.y` 都不得高于所选 libc 的 GNU
  version definitions。满足条件时链接宿主 DSO 完全允许;mcpp 校验的是物理事实,不是
  强加“禁止宿主库”策略。

判定是类型化的:`Pass`、`ProvenMismatch`、`Inconclusive`。可证明的 A/B 冲突会 hard fail,
诊断给出规范化 requester/provider/artifact 路径与可复制的 SubOS 修复步骤。无法取得
loader cache/硬件闭包时明确报告 inconclusive,绝不伪装成绿色。macOS/Windows 复用同一
类型接口但为 no-op,不会套用 ELF/glibc 规则。

verdict 以 `.mcpp-runtime-verdicts.json` 存在 `build.ninja` 旁,键包含产物 stat 指纹与
完整 runtime contract hash。热 no-op 必须已有当前 `pass` 记录,比较 Ninja 前后产物 stat,
并执行零次 ELF 解析;若 Ninja 意外重链则先退回完整路径重新校验,之后才允许成功/运行。
`mcpp self doctor` 复用同一存档 verdict,不会拿已经变化的当前宿主重新猜一次。

装后 fixup 同样按精确身份收敛:`glibc@2.44` 只解析
`<xpkgs>/xim-x-glibc/2.44/{lib64,lib}`。精确 payload 缺失/陈旧就是错误,其他已安装版本
永远不是回退项。

### 6.2 一条运行时搜索路径可以住在哪里(`runtime_env_contract.cppm`)

告诉 loader「去哪找」有两条通道,差别不在便利性,而在**波及范围**:

| | 波及到 | |
| --- | --- | --- |
| `DT_RUNPATH` | 携带它的那**一个**对象,及其 `dlopen()` | 逐二进制 |
| `LD_LIBRARY_PATH` | 本进程**以及它派生的每一个进程** | 继承,且一直传下去 |

第二行就是私有 libc 目录必须是 **binary 作用域**的原因。glibc 的 `libc.so.6` 与它的
`ld.so` 通过 `GLIBC_PRIVATE` 版本锁死:2.44 的 libc 里 `__pointer_chk_guard` 是未定义
引用,只有 2.44 自己的 loader 导出它。mcpp 构建出来的程序没事——`PT_INTERP` 指向私有
loader;`/bin/sh` 有事:它的 `PT_INTERP` 指向**宿主** loader,而且任何环境变量都改不了,
于是 `popen()`/`system()` 的子进程在重定位阶段就死掉,连 `main` 都进不去,也没有任何
输出(#401;#291 是同一形状往内一跳,杀掉的是 mcpp 自己的嵌套宿主工具)。

所以 mcpp 不会把私有 libc 目录发布到环境里。也不需要:只要存在 payload,link model
就已经在 `--dynamic-linker` 旁发了 `-Wl,-rpath,<glibc>`,恰好覆盖这个目录存在的唯一
理由——某个 `dlopen()` 的 `DT_NEEDED` 闭包看不到可执行文件的 RUNPATH。

这是**作用域**,不是条件判断。「只在依赖可能 dlopen 时才导出」仍然是导出,而死掉的
子进程不关心原因。普通依赖运行时目录保留环境作用域:它们没有 loader 耦合,宿主二进制
撞上去最多是困惑,不会死。

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

### 7.3 嵌入式与裸机工具链

`riscv64-none-elf` 与 `riscv32-none-elf` 已实现,面向用户的说明见
[13 — 裸机与 freestanding 目标](13-baremetal.md)。本节记录由此得到的形态与
上文 hosted 模型之间的关系。

本节早先的三条预测成立:

- **无动态链接器。** `loader` 保持为空,这一点每个渲染器本来就允许;部署路径是
  烧录而不是 ELF interp。
- **目标侧不需要 fixup。** 宿主运行的编译器二进制仍需要 PT_INTERP/RUNPATH 对齐,
  与今天的 gcc kind 完全相同。
- **MCU flags、链接脚本处理与运行路径确实需要新设计。** 三者都已落地,且如预测
  那样位于本文档之上的层次:ISA 参数来自 `src/freestanding/target.cppm` 中每个
  目标一行的表,链接脚本经 `link-script` 构建指令到达,执行方式经 `runner` 到达。

有一条预测是错的,而这条更正正是整个设计的承重部分。C 库**不在**工具链载荷内,
因此这并不是换了一个 sysroot 的 `CLibMode::Sysroot`。picolibc 是一份独立载荷,
由目标自己的表行指名(`src/toolchain/triple.cppm` 中的
`sysroot = xim:picolibc-riscv@1.8.12`),与该行的编译器 `pin` 处于同一地位。
从目标而非从工具链解析它,正是裸机**包**不必指名一份 libc 的原因,如同 hosted
包从不指名 glibc。

freestanding 链接行同样是被**替换**而不是被追加 —— 见
`src/freestanding/linkline.cppm` —— 因为在那里每一条 hosted 链接 flag 都是错的,
而不只是多余的。管线更早处追加到普通 ldflags 上的内容会被丢弃,这也是目标 sysroot
的 `-L` 发在该行上而不是与通用 flag 一起发出的原因。

### 7.4 非 ELF 平台

macOS(Mach-O)与 Windows(PE)有意绕开本文大部分内容:macOS 从 SDK 解析
C 世界(`CLibMode::Sysroot`)并有自己的 libc++ 链接处理;Windows 没有 rpath——
mcpp 把运行时 DLL 部署到产物 exe 旁,这正是该平台对 §3–§4 所做一切的原生
等价物。

### 7.5 一个 flag 由哪根轴决定

2026.8.18 那一轮改了四个 flag,每一个此前都挂在错误的轴上。而这类错误的表现
永远相同:**在恰好一个平台上莫名其妙地失败**,报错既不点名那个 flag,
也不点名它背后的决定。

一共三根轴,而在它们之间做选择的问题是:**这个 flag 最终被谁读到。**

| 轴 | 问题 | 例子 | 怎么问 |
|---|---|---|---|
| **目标格式** | 产出的是哪种映像 | `-fPIC`(PE 代码本就位置无关;clang 直接拒绝这个 flag) | `triple::parse(...)->is_pe()`,宿主兜底 |
| **目标 ABI** | 哪个链接器会消费它 | `--out-implib` vs `/IMPLIB:`、`/DEF:`、SONAME / install-name 的形式 | `is_msvc_target(tc)`、`triple->is_msvc_env()` |
| **方言** | mcpp 直接调用的是哪个程序 | `-L` vs `/LIBPATH:`、`-I` vs `/I`、归档命令 | `dialect_for(tc)`、`LinkStyle::SeparateLinker` |

**面向 MSVC ABI 的 clang 是同时区分这三根轴的那个反例。** 它说 GNU 方言、
产出 MSVC ABI 的对象、生成 PE 映像。问错了轴就会:

- 按**方言**判 ⇒ 拿到 `-Wl,--out-implib`,lld-link 回
  `ignoring unknown argument`,随后是「文件不存在」;
- 按**ABI** 判 ⇒ 拿到 `/LIBPATH:`,而编译器驱动不认;
- 按**编译器二进制**判 ⇒ 拿到 `-fPIC`,直接拒绝运行。

失败形状总是同一个:flag 按邻近平台的拼法发出去,而报错来自离那个决定三步远的
另一个程序。面向链接器的答案集中在 `ninja_backend` 的 `pe_link_flag`;
方言表在原来那个条目的位置写明了为什么它答不了这些问题。

## 8. 源码地图

| 关注点 | 文件 |
|---|---|
| spec → xim 包、前端 | `src/toolchain/registry.cppm` |
| detect/probe(triple、sysroot、payload)| `src/toolchain/detect.cppm`、`probe.cppm` |
| 链接模型 + loader 解析 | `src/toolchain/linkmodel.cppm` |
| 统一 fixup 管线(patchelf/specs/cfg、marker)| `src/toolchain/post_install.cppm` |
| install/lifecycle 入口 | `src/toolchain/lifecycle.cppm`;auto-install 入口在 `src/build/prepare.cppm` |
| root runtime 选择/binding | `src/platform/xlings/runtime_selection.cppm`、`src/platform/runtime_binding.cppm`、`src/platform/xlings/subos_info.cppm` |
| 通用 runtime contract + LinkIntent | `src/manifest/types.cppm`、`src/build/plan.cppm`、`src/build/flags.cppm` |
| 存储 resolution 解释 | `src/build/prepare.cppm`、`src/build/runtime_validation.cppm`、`src/doctor.cppm` |
| flag 组装(主构建)| `src/build/flags.cppm` |
| `import std;` 预编译 | `src/toolchain/stdmod.cppm` |
| build.mcpp 宿主 flags | `src/build/build_program.cppm` |
| hermetic 链接校验 | `src/build/hermetic.cppm` |
| 回归fence | `tests/e2e/86_llvm_hermetic_link.sh`、单测 `test_linkmodel.cpp`、`test_post_install.cpp`;`ci-linux-e2e.yml` 的无宿主工具链 CI job |

设计沿革:`.agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md`。
