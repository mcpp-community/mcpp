# 三项未完成事项的详细方案

2026-08-21。承接 `2026-08-21-freestanding-outstanding-four.md`:那份文档里的四项已完
成两项(openarch 的接口与目录树、`mcpp test` 的 feature 源判断),x86_64 裸机目标行也
已随 mcpp 2026.8.21.1 发布。本文只覆盖仍未完成的三项,并且以本次实施**已经量到的数字**
为前提,而不是重新论证。

---

## 0. 这份文档站在哪些已测量的事实上

| 事实 | 数值 | 何时量的 |
|---|---|---|
| `qemu-x86` 五宿主矩阵 | 五条腿全绿;`linux-x64` 真正引导了 multiboot 探针 | 本次 |
| 显式组装后的载荷 | linux-x64 **54 MB**、linux-arm64 53 MB、darwin 两条 53 MB、win32 **58 MB** | 本次 |
| `meson install` 的载荷 | 392 MB / 389 MB / 341 MB / 342 MB(strip 后 343 MB) | 本次 |
| Windows 的 DLL 闭包 | **13 个** MinGW 运行时库,必须与 exe 同目录 | 本次,`ldd` 实测 |
| CN 镜像上传速率 | **0.012 MB/s**,单连接,无分片、无断点续传 | 早前探针 PR #301 |
| `libcxx-headers` 打包体积 | **943,180 字节**(`include/c++/v1` + `share/libc++`) | 本次 |
| libstdc++ freestanding | 宿主目标 **40/40**;裸机目标 **0/40** | 本次 |

⚠️ **两处刻意留白,后文不得当作已知使用。**

* **MSVC STL 对裸机目标是否可用:未测量。** 本机没有 MSVC 载荷。
* **Windows 的 LLVM 载荷里究竟缺什么:只确立了一条。** `#include <algorithm>` 报
  `'algorithm' file not found`。此前一次试图打印载荷路径的 CI 步骤把通配符原样输出,
  说明当时猜的 home 路径是错的,那次观察作废。

---

## 1. `xim:qemu-x86` 收录

### 1.1 门槛已过,剩下的是发布动作而不是可行性问题

索引收录模拟器的门槛写在 `qemu-riscv` 的描述符里:为索引服务的五个宿主目标从**同一个
版本化发布**提供预编译件,每个资产带校验侧文件。`mcpplibs/qemu-x86` 的矩阵五条腿全绿,
这条门槛已经达到。

⭐ **而「先跑通再发布」这一步的价值已经兑现:八条只有真跑才会出现的发现,每一条都是
「已发布却装不上」的成因。** 它们在临时仓库里只是红叉。清单见
`2026-08-21-freestanding-outstanding-four.md` §4.2 与 §4.3.0。

### 1.2 载荷装什么:依据已经有了

| | `meson install` | 显式组装 |
|---|---|---|
| linux-x64 | 392 MB → 343 MB(strip) | 103 MB → **54 MB** |
| win32-x64 | 装不出来 | 110 MB → **58 MB** |

⭐ **七倍,而这不是收尾优化。** 索引的资产预算是实测 0.012 MB/s 的单连接上传:343 MB
的载荷在那条链路上不可发布,54 MB 的可以。**载荷选什么是收录能否成立的前提。**

载荷内容因此固定为三部分,而不是「install 目标恰好拷了什么」:

1. `bin/qemu-system-x86_64`(Windows 上带 `.exe`);
2. `share/qemu/` —— `pc-bios` 整棵树;
3. Windows 独有:与 exe 同目录的 **13 个 MinGW 运行时 DLL**。

⚠️ **`pc-bios` 一个文件都不删。** 哪些 blob 会在运行时被加载是描述符要回答的问题,靠
「删到出问题为止」来回答,就是一个载荷在别人机器上少一个 blob 的由来 —— 本次实施在这
里犯过一次(顺手删了 `keymaps` 与固件描述符),当场撤回。

### 1.3 三步

**步骤 1:把五个资产发布成一个版本化 release。**

`mcpplibs/qemu-x86` 打 tag `9.2.4`,工作流在 tag 上跑一遍并把五个归档上传为 release
资产,每个带 `.sha256` 侧文件。命名沿用索引已有的形状:

```
qemu-x86-9.2.4-linux-x64.tar.gz          + .sha256
qemu-x86-9.2.4-linux-arm64.tar.gz        + .sha256
qemu-x86-9.2.4-darwin-x64.tar.gz         + .sha256
qemu-x86-9.2.4-darwin-arm64.tar.gz       + .sha256
qemu-x86-9.2.4-win32-x64.zip             + .sha256
```

⚠️ **`GLOBAL` 指向本仓库的 release,而不是镜像。** 索引的版本更新器与镜像物化器都把
`GLOBAL` 读作真源;指向镜像会让镜像镜像它自己。`qemu-arm`/`qemu-riscv` 的 `GLOBAL` 是
xPack,因为它们有上游;这个包没有上游,本仓库就是上游。

**步骤 2:镜像到 `xlings-res/qemu-x86` 双端。**

⚠️ **判定上传成功只能靠回探下载 URL。** `gtc` 的退出码两个方向都会撒谎:PUT 头里的
`x-obs-callback` 让 OBS 存完对象再回调 GitCode API,回调失败返回 `code:400 ... EOF`
而**对象其实已落盘**。逐字节比对两端的 sha256,与本次 mcpp 与 openarch 的发布同法。

⚠️ 单文件拆不开:预签名是 OBS 单次 PUT,无 multipart、无断点续传。54 MB 在 0.012 MB/s
下约需 75 分钟单连接;镜像脚本的并发(`MIRROR_MAX_PARALLEL`,默认 8)可以叠加,而预算
是**整条 leg 的 deadline** 而非 per-asset cap。这也是步骤 1 必须先把载荷压到 54 MB 的
理由。

**步骤 3:写 `xim:qemu-x86` 描述符,PR 到 `openxlings/xim-pkgindex`。**

形状与 `qemu-riscv` 同构。三处必须自己回答而不能抄:

* **`deps` 是否为空** —— 按 `qemu-arm` 的做法**实测** DT_NEEDED / otool 闭包后决定。
  ⚠️ 已知的一半答案:Windows 侧需要 13 个 DLL 与 exe 同目录,因为 PE 没有 rpath。
  Linux 侧本次实测二进制的 NEEDED 只跨出核心 glibc,与 `qemu-riscv` 的结论同形 ——
  但那是 `meson install` 布局下的观察,显式组装后需要**重测**。
* **`programs`** —— 只登记 `qemu-system-x86_64`。
* **`archs`** —— 五个宿主里 win32 只有 x64,`archs` 是跨平台的并集,而 arch 解析是
  fail-closed 的,所以 arm64 的 Windows 宿主会被告知该 arch 不可用,而不是拿到 x64 包。

### 1.4 判据

必须同时满足,**任缺其一即视为未完成**:

1. 五个资产在两端逐字节一致(sha256 比对,不是 HTTP 200);
2. `xlings install qemu-x86 -y` 在 Linux 上装完之后,`qemu-system-x86_64 --version` 可
   执行;
3. ⭐ **`openarch` 的 `examples/switch` 在 `--target x86_64-none-elf` 上经 `mcpp run`
   引导并打印 `switch ok`** —— 这是本包存在的唯一理由,而它当前只在本机的 qemu 上验证
   过;
4. 描述符的 `deps` 是实测 DT_NEEDED / DLL 闭包后写下的,而不是从兄弟描述符抄的。

⚠️ 第 3 条是重点。`--version` 能跑只证明二进制不是坏的;**引导一个真实镜像才证明这个
载荷可用**,而这两件事在本生态里分开过一次(见 `xcb-gen-and-ok-marker-content-gap`)。

### 1.4.1 已完成(2026-08-21),以及计划没有预料到的两条

四条判据全部满足。⚠️ **实施过程中出现两件本节没有预料到的事,两件都是「什么都没发生」
被读成「一切正常」的形状。**

**一、`deps` 为空是实测出来的,但第一次测量什么也没测到。**

写下的检查是 `ldd "$B" 2>/dev/null | grep -v "$PAYLOAD/lib/"`,**一行不输出**,读起来
和「没有越界」一模一样。真相:subos PATH 上的 `ldd` 是个在该 shell 下**语法就崩**的
脚本,而它崩在 **stderr** —— 正好被 `2>/dev/null` 吃掉。改为直接对 loader 调
`LD_TRACE_LOADED_OBJECTS=1` 后得到:

    15 个对象;12 个出自载荷自己的 lib/(RUNPATH=$ORIGIN/../lib)
    2 个跨出边界:libc.so.6  libm.so.6   —— 都是核心 glibc
    1 个是 loader 本身

⇒ `deps` 为空,与 `qemu-arm` 同形,理由同样是 elfpatch 整条替换运行期搜索标签。

**二、⚠️ 扁平归档不能整目录搬,而这一条 §1.3 没有提。**

`xim` **就地解压到存放归档的那个目录**,而那个目录是共享的。xPack 的归档带
`xpack-qemu-arm-<ver>/` 根,所以 `qemu-arm` 搬那一个目录即可;本包的归档是**扁平的**
(`bin/`、`share/` 在顶层),整目录搬会搬走 xim 的共享目录。症状是
`linux-install-test` 报 `no directory contains bin/qemu-system-x86_64`。

改法沿用 `cc-connect` 对其扁平载荷的做法:`os.mkdir(install_dir())` 再按名搬。
⭐ **断言放在搬之前** —— 源目录是共享的,里面的 `bin/` 未必是我们的,放在之后的检查
已经把别人的搬走了。

**三、镜像的判据救了一次。** `gtc` 对 `linux-arm64` 报告 `uploaded`,而回探得到

    {"error_code":404,"error_code_name":"NOT_PATH",...}   # 127 字节

日志里那次上传其实抛了 urllib traceback,循环继续走了下一个。CN 端首轮 **4 OK /
1 mismatch**,重传后 5/5。这与本次会话早先的一次**截断**是同一判据抓住的两种不同故障。

### 1.5 收录之后要回改的三处

* `openarch` 的 `build.mcpp`:x86_64 分支目前直接 `return 0` 且注明「索引里没有
  `xim:qemu-x86`」。收录后改为与另两台机器同形,并把那段注释改写成历史记录。
* `openarch` 的 CI:x86_64 那一行的模拟器从 apt 改回 `xim:qemu-x86`,与另两行一致。
* `openarch` 的模板 README:`mcpp build --target x86_64-none-elf` 改为 `mcpp run`。

⇒ 三处均已改(`mcpplibs/openarch` 0.5.0)。实测:`mcpp run --target x86_64-none-elf`
经索引装的 `xim:qemu-x86` 引导并打印 `switch ok`,即判据 3。

---

## 2. Windows 的 C++ 标准库头

### 2.1 现状,以及唯一确立的事实

`std-freestanding` 的 `build.mcpp` 从 `mcpp::toolchain_dir()` 借三个目录:

```
<tc>/include/c++/v1      libc++ 的头
<tc>/share/libc++/v1     每个头一份 std/<h>.inc 导出表
<out>/libcxx-config/…    本包合成的 __config_site
```

前两个在 Windows 的 LLVM 载荷里不存在,于是 `#include <algorithm>` 报
`'algorithm' file not found`。这是这条路径上**唯一被确立的事实**。

### 2.2 ⚠️ 一处必须纠正的记录:libstdc++ 不是替代方案

上一份文档写着「libstdc++ 一个 `-D_GLIBCXX_HOSTED=0` 就行(实测 29/34)」。重测之后
那个数字站不住,而它站不住的方式说明了真正的问题在哪。

同一批 40 个 C++23/26 freestanding 头、同一份 libstdc++ 16.1.0、同样加
`-D_GLIBCXX_HOSTED=0`,**只换目标**:

| 目标 | 结果 |
|---|---|
| `x86_64-linux-gnu`(宿主) | **40 / 40 编过** |
| `riscv64-none-elf`(裸机) | **0 / 40** |

```
bits/c++config.h:733:
bits/os_defines.h:39: fatal error: 'features.h' file not found
```

⇒ 原来那个 29/34 是在**宿主目标**上量的。它证明的是「libstdc++ 支持 freestanding
模式」,而不是「libstdc++ 能服务一个裸机目标」—— 而后者才是这个包的用途。

### 2.3 提法要改:不是「用哪个实现」,而是「有没有为这个目标 configure 过」

| 实现 | 逐目标配置是什么 | 能不能合成 |
|---|---|---|
| **libc++** | `__config_site` —— 一个**扁平的宏列表** | **能**。本包已经在做:`sed` 改十行即得到交叉目标的配置 |
| **libstdc++** | `bits/c++config.h` —— **configure 的产物**,且 `#include <bits/os_defines.h>` → `<features.h>`,把宿主 C 库拖进来 | 不能。要给裸机目标用,需要一份**为该目标 configure 出来的** libstdc++,即一个 riscv64 的 gcc |
| **MSVC STL** | UCRT / vcruntime 头 | ⚠️ **未测量**,见 2.6 |

⭐ 选 libc++ **不是因为它「更 freestanding」**,而是因为它的逐目标配置是唯一一个可以被
合成出来的。这条理由比「配置层很小」更准确,也更有用:**它说明换实现解决不了 Windows
的问题。**

⇒ 真正的缺口在第三层:那份实现的**头文件本身**在 Windows 的载荷里不存在。libc++ 的头
与宿主无关(是文本),只是没有被打进那个载荷。

### 2.4 三步,每步可独立落地

**步骤 1:`std-freestanding` 增加「头从哪来」的解析顺序。**

```
1. 工具链载荷带了就用          ← 今天 Linux 与 macOS 的路径,一字不变
2. 否则用声明的包
```

⚠️ **只有第 1 条落空时才走第 2 条**,所以 Linux 与 macOS 的解析路径完全不变。这是
「无感升级」那一栏的要求,也是这一步可以先于步骤 2 合入的理由:在包存在之前,第 2 条
永远不触发,行为与今天相同。

第 2 条是一条普通依赖边,版本约束由它承载:

```toml
[dependencies]
libcxx-headers = "^22.1.0"
```

⚠️ **`"^22.1.0"` 而不是 `"22.1.x"` 或 `"22.1"`。** 本包已经为此付过学费:`0.1.x` 让
安装器报 `E_NOT_FOUND` 并点名一个存在的包;`0.1` 解析通过然后 `install path missing
after fetch`。判据是**构建**,不是「没报 E_NOT_FOUND」。

**步骤 2:新建 `xim:libcxx-headers`。**

内容是 `include/c++/v1` 与 `share/libc++`,打包后 **943,180 字节**。它与宿主无关,因此
**一个包服务全部五个宿主**,而「加进载荷」要给每个缺失的宿主各加一次。

⚠️ 版本必须与产出导出表的那份 libc++ **同版本**。`std/<h>.inc` 是 libc++ 自己维护的
导出表,与它的头是一套;混版会让导出表点名不存在的声明,而那是模块接口里的错误,不是
消费者的。

**步骤 3:实现层的选择成为可声明的(可选,且不阻塞前两步)。**

接口层不变,配置层按检测到的实现分支:

```
有 include/c++/v1        → libc++    :合成 __config_site
有 bits/c++config.h      → libstdc++ :需要该目标的 c++config.h,见 2.2
两者皆无                  → 具名诊断
```

⚠️ 按 2.2 的测量,libstdc++ 这条分支对裸机目标**当前不成立**,除非索引里出现一个为该
目标 configure 过的 libstdc++。所以步骤 3 的现实价值是**把诊断写准**,而不是提供第二
条可用路径。

### 2.5 判据

1. Linux 与 macOS 的构建**逐字节不变**(比对 `compile_commands.json` 归一化后的 diff);
2. Windows 宿主 × `riscv64-none-elf` 目标上,`std-freestanding` 的 CI 行从跳过变为通过,
   并且编出的头数量与 Linux 一致;
3. 两端镜像逐字节一致;
4. ⭐ **一条 e2e 断言「载荷带 libc++ 时不去碰那个包」** —— 否则步骤 1 的回落可能在
   Linux 上也悄悄生效,而那正是「无感升级」会被破坏的方式。

### 2.6 ⚠️ 未测量的一栏:MSVC STL

「Windows 上不能基于 MSVC STL 吗」这个问题盖住了两件不同的事:

* Windows 宿主 → **Windows 目标**:MSVC STL 当然可以,而且是默认。`std-freestanding`
  根本不介入。
* Windows 宿主 → **裸机目标**:这才是本节的场景。MSVC STL 的逐目标配置是 UCRT /
  vcruntime,那是 **Windows 目标**的组件。

⚠️ **这一条没有被测量过。** 本机没有 MSVC 载荷,不能声称验证过。此前一个看似合理的
反对理由——「裸机 triple 配不上 MSVC ABI」——已被实测否掉:clang 接受
`riscv64-pc-windows-msvc`。**一个被否掉的理由不构成结论,它只是把这一栏退回未知。**

⇒ 若要把这一栏从未知变成已知,需要一次最小测量:在 Windows 宿主上,用 MSVC STL 的头对
`riscv64-none-elf` 编 `#include <array>`,并记录第一条错误。那次测量应当在
`std-freestanding` 的 CI 里作为一个**允许失败**的信息性步骤跑一次,而不是在本地猜。

#### 2.6.1 已测量(2026-08-21,`std-freestanding` PR#1 的 Windows 行)

那一步跑了,结果**比本节的推测早一步**:

```
── MSVC STL, for a bare-metal target ──
t.cpp:1:10: fatal error: 'array' file not found
── MSVC STL, for its own target, as a control ──
(无诊断)
```

⭐ **那些头从来没被读到。** 本节推测的失败点是「UCRT / vcruntime 是 Windows 目标的
组件,因此编不过」;真实情况是 clang **只在目标是 MSVC 目标时**才搜索 MSVC 标准库,
`riscv64-none-elf` 下它根本不去那儿看,于是 `<array>` 直接不存在。对照组用同一个
clang 同一行编 `x86_64-pc-windows-msvc`,一声不吭。

⇒ 「用 MSVC STL 服务裸机目标」不是一件会失败的事,是一件**驱动从不尝试**的事。
**宿主的 clang 是拿什么标准库编出来的,不参与一次交叉编译。**

⚠️ 这是这个问题的第**三**个解释,前两个都错。这一栏之所以最终答对,不是因为第三次
推理更严谨,而是因为它是唯一一次去问机器。

---

## 3. openarch 的时钟

### 3.1 先调研,不先写接口

前四个接口(context、pte、trap、cpu)有一个共同性质:**三台机器都提供同一件事,只是
拼写不同**。时钟不显然具备这个性质,而在它具不具备被回答之前写接口,会得到一个只有
一台机器能实现的接口,或者一个把板级事实推给所有调用方的接口。

⭐ 这与本层已经做过的一次判断同形:`openarch.pte` 之所以接管 `MAIR_EL1`,是因为
「device 在两台机器上意思相同」这条承诺**只能**由本层兑现。时钟要问的是同一个问题,
而答案可能是否定的。

### 3.2 三台机器各自提供什么(待探针确认)

| | 计时源 | 地址/编号从哪来 | 是否架构保证存在 |
|---|---|---|---|
| riscv64 | CLINT 的 `mtime` / `mtimecmp` | **内存映射,基址是板级事实**(qemu `virt` 为 0x2000000) | M 模式下由平台提供而非架构 |
| riscv64(Sstc) | `stimecmp` CSR | 无地址 | 取决于扩展是否实现 |
| aarch64 | Generic Timer:`CNTFRQ_EL0` / `CNTP_CTL_EL0` / `CNTP_TVAL_EL0` | **架构系统寄存器**,无地址;但中断号是板级事实 | 是 |
| x86_64 | PIT / HPET / LAPIC timer / TSC-deadline **四选一** | 分别是 I/O 端口、ACPI 表、MMIO 或 MSR | 都不是必然存在,需运行时探测 |

⇒ 三台机器在**同一个轴上分歧最大**:riscv 的计时器地址是板级的,aarch64 的是架构的,
x86_64 的连「哪一个计时器存在」都要运行时回答。

### 3.3 调研要回答的三个问题

1. **「设置一次性到期」这个动作,三台机器能不能用同一个签名表达?**
   最小接口候选:`set_deadline(u64 ticks_from_now)` + `now() -> u64` + `frequency()`。
2. **本层能不能在不知道板级事实的前提下兑现它?** riscv 需要 CLINT 基址,aarch64 需要
   中断号,x86_64 需要选一个源。若答案是「不能」,时钟就不属于这一层。
3. **若不属于,正确的归属是什么?** 两个候选:板级支持包(与 UART、内存图同列),或者
   一个介于两者之间的「平台」层。

### 3.4 两种归属,以及各自的代价

**候选 A:留在 openarch,由 `install_memory_attributes` 同款的「本层拥有该寄存器」解决。**
代价是本层必须接受板级参数(CLINT 基址、中断号),而那会让 `openarch` 的接口第一次
带上板级形状。⚠️ 这与本层至今为止的性质冲突:四个现有接口没有一个需要知道板子。

**候选 B:时钟归板级包,openarch 只提供三台机器共有的那部分**(例如「读一个单调计数
器」,若确实三台都有架构级来源)。代价是一个裸机内核仍然要从两个包拿两半。

⭐ 调研的产出应当是**一份最小探针**(每台机器一个,能打印 `now()` 的两次读数递增),
外加一段记录「哪一半需要板级事实」。归属的决定由那段记录得出,而不是由偏好。

### 3.5 判据

1. 三台机器各有一个最小探针,在 CI 里跑,打印两次单调递增的读数;
2. 一份记录,逐条写明每台机器上哪些数值来自架构、哪些来自板子;
3. ⭐ **一个明确的归属结论,以及被否掉的那个候选被否掉的理由** —— 而不是只写选中的
   那个。

---

## 4. 顺序与依赖

```
第 1 项(qemu-x86 收录) ── 独立,可立即开始
                          └→ 完成后回改 openarch 的三处(§1.5)

第 2 项(Windows 头)  步骤 1 ── 独立,可先合入(包不存在时不触发)
                       步骤 2 ── 依赖步骤 1 才有意义
                       步骤 3 ── 可选,不阻塞
                       2.6 的测量 ── 独立,应尽早跑一次以消除未知

第 3 项(时钟)  调研 ── 独立;接口设计依赖调研结论
```

三项之间**没有互相阻塞**,可以并行。

## 5. 多角度检视

| 角度 | 第 1 项 | 第 2 项 | 第 3 项 |
|---|---|---|---|
| **架构** | 载荷内容由「模拟器需要什么」决定,而不是由 install 目标决定 | 目标的 C++ 标准库像目标的 C 库一样**可解析**,与 C 库的做法同构 | 先回答归属,再写接口 —— 顺序本身就是架构决定 |
| **稳定性** | 判据是「引导一个真实镜像」,不是 `--version` | 判据是「Linux/macOS 逐字节不变」 | 判据是探针跑出单调递增,不是「编过了」 |
| **优雅简洁** | 载荷三部分,可以一句话说清 | 不新增清单键,只加一条依赖边 | 可能的结论是「不加接口」,而那也是一种简洁 |
| **用户体验** | x86_64 从「只能 build」变成「可以 run」 | Windows 用户从「报头找不到」变成「装上就用」 | 若归板级,用户少一个会用错的接口 |
| **兼容性** | 已发布的两个 qemu 包不受影响 | 老版本 `std-freestanding` 行为不变 | 现有四个接口不动 |
| **跨平台** | 五个宿主是门槛本身 | 一个包服务五个宿主 | 三台机器分歧最大的一处,正因如此才要先调研 |
| **一致性** | 描述符与 `qemu-riscv` 同构 | 与 `[target].sysroot` 解析 C 库同构 | 与 `MAIR_EL1` 的归属判断同法 |
| **无感升级** | 收录后 `openarch` 的 x86_64 行自动获得 runner | ⭐ 由「载荷优先」保证;并由一条 e2e 守住 | 不改现有接口即无升级成本 |

## 6. 本方案刻意不做的事

| | 理由 |
|---|---|
| 不把 `qemu-x86` 的载荷继续裁小 | 54 MB 已在预算内;继续裁需要回答「哪些 blob 运行时会被加载」,而那不是靠删文件回答的 |
| 不为 libstdc++ 造一份 `c++config.h` | 那等于把 configure 的产物手写出来,并且要逐目标维护 |
| 不在本地猜 MSVC STL 的结论 | 见 2.6:一个被否掉的反对理由不构成结论 |
| 不先写时钟接口再找归属 | 见 3.1 |
