# MSVC × xlings 生态打通 —— 综合报告(2026-08-16)

## 0. 一句话

MSVC 从「mcpp 工具链体系里唯一无法声明版本的家族」变成了与 gcc 同构的一等公民。
**而这次真正的收获在验证环节**:在已经发布、CI 全绿的东西里查出了一连串缺陷 —— 其中「解包」这一处就有六层,
其中一条让包对默认构建完全不可用,一条是我自己写的测试**不可能失败**,
还有一条是我明知道该怎么避免却还是犯了(零先例的 sandbox API)。

它们的共同形状只有一个:**"没发生"和"成功了"输出一样。**

**终点判据**(真机、非 skip):

```
PASS: msvc@14.44.35207 installs, builds, stays distinct from msvc@system, and removes
                                                    ── e2e 239,1 passed / 0 skipped

xrgui:  Resolved msvc@system → msvc 19.52.36629 (C:\VS2026Insider\...\14.52.36629\cl.exe)
        [  PASSED  ] 54 tests.        ── 两条腿全绿,且 vswhere workaround 仍然是删掉的
```

而每修好一个,下一个才够得着 —— 这条链一共走了十一层:

```
installed() 只查 cl.exe        →  只查目录(缺 kernel32.lib 照样说装好了)
  → 缺 kernel32.lib            →  缺 um 头 / shared 头 / rc.exe / mt.exe
  → mcpp 认 SDK 只看头文件      →  构建终于通了,remove 在 Windows 上失败
  → 报错不说是哪个文件          →  vctip.exe 占着 payload,谁都卸不掉
  → 不再安装它 ≠ 修好已经装了的  →  installed() 得能认出「旧 recipe 的产物」
  → 改名目录(前提就错了)        →  空目录骨架也删不掉,判据得是「没有文件残留」
```

**每一层都被上一层挡着看不见,而每一层的"绿"都是真的绿。**
最后一层尤其说明问题:它是在「构建成功」之后才够得着的 ——
在此之前没有任何一次运行走到过 remove 这一步。

---

## 1. 交付物

| 仓库 | PR | 状态 |
|---|---|---|
| mcpp | #434 MSVC 版本轴(主体) | ✅ 合入,**18/18 绿** |
| mcpp | — | ✅ **发布 `2026.8.16.1`**,四平台产物 + 索引 bump 完成 |
| mcpp | #436 装好的 toolset 不出现在 list 里 | ✅ 合入,18/18 绿 |
| mcpp | #435 记录被查出的缺陷(文档) | ✅ 合入,18/18 绿 |
| mcpp | #438 e2e 239 只能 pass/skip,不可能 fail | 已并入 #440(单 PR) |
| mcpp | #440 payload 根目录 + SDK 两半判据 + Windows remove 三层 + review §3b/§3c/§3 | ✅ **合入,19/19 绿,e2e 239 真机 PASS** |
| mcpp | #441 拒绝 `gcc@system` | ❌ **已关闭 —— 我自己造出来的问题** |
| mcpp | #442 host helper 链接策略对齐(**closes #437**)+ review §1 更正 | ✅ 合入,17/17 绿 |
| mcpp | #443 版本号 → `2026.8.16.3` | ✅ 合入 |
| mcpp | #444 自我 review 抓到的两条(清扫波及面 / yes-no 起编译器) | ✅ 合入,19/19 绿 |
| mcpp | — | ✅ **发布 `2026.8.16.3`**,gitcode 镜像 **21/21 字节一致** |
| xim-pkgindex | #641 `xim:mcpp` → 2026.8.16.3 | ✅ 合入,索引已发布 |
| xrgui | #6 `MCPP_VER` → 2026.8.16.3 | ✅ **合入,两条腿全绿,54/54** |
| mcpp | — | ✅ **发布 `2026.8.16.2`**(带 #436),索引已 bump |
| xim-pkgindex | #629 payload 多地址 + 27 个镜像 | ✅ 合入,15/15 绿 |
| xim-pkgindex | #630 补动态 CRT 导入库 | ✅ 合入,15/15 绿 |
| xim-pkgindex | #631 `xim:mcpp` → 2026.8.16.1 | ✅ 合入,15/15 绿 |
| xim-pkgindex | #632 GNU tar 盘符 / `os.curdir` / skill 规则 | ✅ 合入,15/15 绿 |
| xim-pkgindex | #633 zip 要 bsdtar / 引号 / 去掉 cd | ✅ 合入,15/15 绿,**真机装通** |
| xim-pkgindex | #634 `xim:mcpp` → 2026.8.16.2 | ✅ 合入,15/15 绿 |
| xim-pkgindex | #635 cl.exe 没有 clui.dll 跑不起来 | ✅ 合入 |
| xim-pkgindex | #636 SDK 缺核心导入库 + um/shared 头 + rc/mt(17 payload,+120 MB) | ✅ 合入,windows-test 真机装通 |
| xim-pkgindex | #637 msvc 不再安装 vctip.exe(否则卸不掉) | ✅ 合入,15/15 绿,索引已发布 |
| xim-pkgindex | #638 复用 `toolsdir()`,不再手写第二份布局 | ✅ 合入,15/15 绿 |
| xim-pkgindex | #639 已经装了旧布局的机器要被拉回来 | ✅ 合入,15/15 绿 |
| xim-pkgindex | #640 `installed()` 规则进 skill + lint + windows-test **真的运行**程序 | ✅ 合入,15/15 绿 |
| xlings | #549 D4 管道侧实测 | ✅ 合入 |
| xlings | #550 `info` 平台误报 + e2e flake | ✅ 合入,全绿 |
| xlings | #551 卸载扛得住被占用的文件(park-and-sweep 下沉) | ✅ 合入 |
| xrgui | #5 删 workaround | ✅ **合入,两条腿全绿,54/54** |
| xrgui | #3(原 Windows 支持 PR) | 已关闭 —— #5 是它的超集,并且**不带那个 workaround** |

---

## 2. 架构:版本轴决定来源

改前:`is_system_toolchain()` 对**所有** msvc spec 为真 —— manifest 写了版本也被丢掉。
后果不是不优雅,是**同一份源码在两台机器上被不同编译器编译且不报错**
(xrgui#3 实测:同一轮 CI 里 mcpp 用 14.51、xmake 用 14.52,直到 ICE 才暴露)。

| spec | 来源 | 用哪个编译器 |
|---|---|---|
| `msvc@system`(或裸 `msvc`) | 机器自己的 VS | 这台机器装的那个(**语义一字未改**) |
| `msvc@<toolset>` | mcpp 安装的 xlings payload | 声明的那个,每台机器都是 |

关键在于**两条正交的轴**:「获取」与 gcc 共用(xim 安装),「解析」与
`msvc@system` 共用(`installation_from_tools_dir`)。受管 toolset 因此不是
第二条代码路径,长不出自己的 bug。

已在**发布产物**上验证:Linux 上 `mcpp toolchain install msvc 14.44.35207`
走的是 xim 路径而不是 `msvc_wrong_host()` —— 分流生效。

### ⭐ 最强的一条证据:xrgui 删掉 workaround 之后仍然绿

这是整套里**唯一不能靠调测试自我满足**的判据。workaround 还在的时候,
「`VSINSTALLDIR` 被采纳」与「vswhere 找不到东西」现象完全一样。

xrgui#5 把「把 `vswhere.exe` 挪开」那一步**删掉了**,而 vswhere **还在**、
它排第一的 Enterprise **14.51 也还装着** —— 什么都没被拿走,错误答案只是必须输:

```
Resolved msvc@system → msvc 19.52.36629
  (C:\VS2026Insider\VC\Tools\MSVC\14.52.36629\bin\Hostx64\x64\cl.exe)
```

`xrgui_tests` **54/54**,mcpp 与 xmake 两条腿全绿。原来的 PR #3 已关闭 ——
#5 是它的超集,而且**不再需要那个 workaround**。

合入 master 之后 `mcpp (Windows / MSVC)` 在 master 上**又跑了一遍,仍然绿** ——
不是同一次运行的复用,是合并后的树重新构建的结果。

### 顺带修掉的三条

- **`VSINSTALLDIR` 现在优先于 vswhere**。vswhere 几乎总能返回点什么,所以
  `VSINSTALLDIR` 事实上不可达 —— **猜测不该压过答案**。vswhere 加 `-prerelease`。
  `VS*COMNTOOLS` 仍排 vswhere **之后**(机器全局残留 ≠ 有人为这个 shell 设的)。
- **SDK 搜索**:`WindowsSdkDir` → store 里的 `xim:windows-sdk` payload → 写死路径(降为回退)。
  第二条零配置、零版本硬编码:**编译器自己的路径就说明了它来自哪个 store**。
- **`cxx_runtime` 与 `linkage`**:在 MSVC ABI 上它们是同一个物理开关,现在都走
  `msvc_wants_static_crt()`。默认仍是 `/MD`(判据取 manifest 字面值,不是解析后的
  contract —— 否则每个 Windows 构建都会翻成 `/MT`)。

---

## 3. 验证:哪些能自证,哪些不能

**能被"调一下测试"满足的判据不算证据。**

| 判据 | 自证? | 结果 |
|---|---|---|
| 声明的 toolset 优先于"最新" | ❌ | 单测:两个都在,要**老的**那个。「取最新」会失败,真机上可能碰巧对 |
| 两条来源互不污染 | ❌ | e2e 239:换回 `msvc@system` 必须解析到系统 cl |
| 受管 toolset 真的被用了 | ❌ | e2e 239 每条断言都写成「系统编译器应答就失败」 |
| 27 个 payload 镜像正确 | ❌ | **下载回来** sha256 与微软一致 —— 而这抓到了真问题(下面 §4) |
| SDK 里到底有什么 | ❌ | **解 MSI 的 File/Component/Directory 表**算出每个文件的安装路径 —— 名字和文档都在骗人(§4.8) |
| 删掉 vctip.exe 是否安全 | ❌ | 依据是 **cl.exe 自己的字符串** `Not launching VCTIP: Binary not found` —— 它本来就有这条分支 |
| 单测 83/83、静态 1798 项 | ✅ | 证明没回归,不证明做成了 |

**msvc 发现逻辑第一次能在 Windows 之外测试**:`installation_at()` 收目录而不是
探测机器,`find_windows_sdk()` 收 root 列表,两者都不再被平台宏包住 ——
6 个新单测在 Linux CI 上跑真 fixture。改动前这里一行都测不了。

---

## 4. 验证过程抓到的缺陷 —— 这一节是重点

### 4.1 上传工具说失败、列表说在、实际 404

镜像 27 个 payload 时,`gtc` 对 16 个文件报了失败,而 release 列表显示它们**在**。
下载回来一验:**404,根本不在**。

原因是 gitcode 拒绝 `.cab` 扩展名、也拒绝名字里的空格,而失败发生在 `obs_callback`
之后 —— 资产**注册了**、对象**没落盘**。改用 `<下划线名>` / `<name>.cab.bin`
重传,再逐个下载校验:**27/27 与微软一致**。

> 判据不是「上传成功」,是「字节回来了且摘要一致」。这两者差了 16 个文件。

### 4.2 已发布、CI 全绿的 toolset 链接不了默认构建 ⚠️

查「打包的 14.52 能不能构建 xrgui」时发现:**不能,而且对任何项目都不能。**

payload 集里只有**静态** CRT。判据是头文件自己写的(`use_ansi.h`):

```c
#if defined(_DLL) && !defined(_STATIC_CPPLIB)
#define _LIB_STEM "msvcprt"      // /MD ← payload 里没有
#else
#define _LIB_STEM "libcpmt"      // /MT ← 有
```

`msvcprt.lib` 在 `Microsoft.VC.<ver>.CRT.x64.Store.base` 里 —— **名字是它被跳过的原因**,
听起来像 UWP,实际把桌面用的动态导入库放在 `lib/x64` 顶层。

**真正的缺陷不是少一个 payload,是没有任何东西会发现它。** `installed()` 只查
`cl.exe` 和 `std.ixx`,两个都在 → 安装报成功 → 真 Windows runner 上
`windows-test` 全绿 → 工具链在默认配置下不可用,失败会出现在用户的链接步骤里。

现在 `installed()` 对每种 CRT 模型各查一个导入库,并且失败时**说出缺哪个文件**。
补丁后的 `windows-test` 在真机上仍然绿 —— 这次那个绿是有意义的。

> **一个比"能用"弱的"装好了"判据,报的不是安装成功,是解压成功。**

### 4.3 装好的 toolset 在 `toolchain list` 里根本不出现

拿**已发布的** 2026.8.16.1 二进制对着一个 payload 形状的 fixture 跑,发现
装好的 msvc toolset 一行都不显示。

原因:枚举问的是 `toolchain_frontend(root / "bin", pkg)` —— 而 cl.exe 在
`VC/Tools/MSVC/<ver>/bin/Host<h>/<arch>/`,深四层。拿不到 → `continue` → 消失。

**这个布局有三个地方需要知道:安装知道、构建知道、列表不知道。**
第三份内联副本就是这么来的。现在三者共用 `payload_frontend()`。

值得记的是它**怎么被发现的**:不是测试。我为此写的单测钉的是
`identify_xim_payload("msvc")` —— 那一条本来就是对的。
**「身份映射」和「枚举」是两个问题,而我只问了其中一个。**
(e2e 239 的 1b 步会抓到它,但那要等包发布后 Windows e2e 再跑一轮。)

---

### 4.4 我自己写的 e2e 只能 pass 或 skip,不可能 fail ⚠️

包发布之后,Windows e2e 第一次真的跑到了 `239_msvc_managed_toolset.sh`。
它打印了 `PASS`。**而它其实失败了。**

skip 的判据是**事后**匹配失败文本,而其中一个模式是 `*"index"*` ——
几乎每条 mcpp 命令都会打印 "package index"。于是**任何**真实的安装失败
都会走进 skip 分支。那次安装跑了 **135 秒**、失败了、脚本报 PASS。

> **用失败的样子来决定 skip,不是 skip,是一种不去看。**

现在 skip 在动手**之前**由一条正向检查决定(`toolchain list` 里有没有 msvc 行),
之后的任何失败都是失败,并且**把输出打出来**而不是折进一行消息里。

### 4.5 它藏住的那条:GNU tar 把 `C:` 当成主机名

```
tar -xf "C:\Users\...\.payloads\Microsoft.VC...vsix"
tar: Cannot connect to C: resolve failed
```

GNU tar 先按 `host:path` 解析,再看盘符;Windows 自带的 bsdtar 不会。
**同一个 runner 镜像、同一份 recipe、不同的 tar** —— 这就是它在 index 的
`windows-test`(System32 的 bsdtar)通过、在 mcpp 的 e2e(Git for Windows
把 GNU tar 排在前面)失败的全部原因。

`--force-local` 只有 GNU tar 认、bsdtar 直接拒绝,等于换一个坏掉的环境。
**当时**改成 cd 进 payload 目录用相对文件名 —— 而那一版后来自己变成了
第 4 层的故障,见 §4.5b。

**同一份日志还确认了一件好事**:五个 payload(含 #630 新加的 Store)
**全部从 gitcode.com 取到** —— 镜像在真实 Windows runner 上验证通过了,
坏的只是解包。

### 4.5b 解包这一个代码块,拆出五层原因

修好 skip 判据之后,同一个 `tar` 调用连着炸了五次,**每一次都是真原因,
而且每一层只有在上一层被拿掉之后才看得见**:

| # | 报错 | 真正的原因 |
|---|---|---|
| 0 | *(无 —— 报 PASS)* | e2e 的 skip 匹配到了 `*"index"*` |
| 1 | `Cannot connect to C:` | GNU tar 把 `C:` 当主机名 |
| 2 | `does not look like a tar archive` | **GNU tar 根本不读 zip** |
| 3 | `"C:\Windows/System32/tar.exe"` | `path.join` 混分隔符,exe 路径也要 `winpath()` |
| 4 | 命令完全正确却找不到文件 | `system.exec` **不继承** `os.cd` |
| 5 | `The filename... syntax is incorrect` | `cmd /c` 遇到「带引号的 exe + 带引号的参数」会弄坏整行 |

最后的形态**比我动手之前更简单**:不改 cwd、不用相对路径、不依赖 PATH。
中间几版是在加 workaround,最后一版是在**删** —— 第 1 层的相对路径本来
就只是为了绕开第 2 层里那个错误的 tar,钉死 bsdtar 之后它没了理由,
却留下来变成了第 4 层的故障。

### 4.5c 第六层在 mcpp 自己这边:payload 根目录被猜了

装成功之后暴露的:

```
error: msvc payload installed at '...\xim-x-msvc\14.44.35207\VC'
```

注意结尾的 `\VC`。`XpkgPayload::root` 只在版本目录**直接**含
`bin/` `include/` `lib/` 时才认它是根,否则**钻进唯一的子目录** ——
而装好的 msvc payload 只有一个条目 `VC/`。

修法不是去迁就那个启发式,是**不再问它**:位置是
(store, name, version),三者在两个调用点都是已知的。
`resolve_xpkg_path` 继续负责安装,只是不再被问「在哪」。

### 4.6 还有一条,而且我明知道该怎么避免:`os.curdir` 不在 sandbox 里

修 tar 时我写了 `local prevdir = os.curdir()`。Windows CI:

```
msvc.lua:360: attempt to call a nil value (field 'curdir')
```

**这条 recipe 已经在同一堵墙上撞过三次**(`os.execv`、`path.absolute`、
`os.files`),每一次的信号都一样:**整个 index 里零处使用**。

```bash
grep -rn "os\.curdir" pkgs/ | wc -l    # 0 → 别用
```

零处使用的 sandbox API 基本可以认定不在 runtime 里 —— 不是概率判断,
是「能用的东西早就有人用了」。而代价不对称:猜对省几秒,猜错要等一轮
Windows CI,失败信息还出现在离你写的那行几层远的地方。

已经把这条规则和四个条目一起写进了 `xpkg-creater` 技能的 sandbox API 表。
当时的改法是 `os.cd(pkginfo.install_dir())` —— 而最终版**把 `os.cd` 整个删了**,
因为 `system.exec` 根本不继承它(§4.5b 第 4 层)。也就是说:这条 API 缺失
是在一段**本来就不该存在**的代码里踩到的。

---

### 4.8 Windows SDK:名字写着 "Desktop" 的,一样都不给 ⚠️⚠️

§4.2 之后 `installed()` 改成查文件名而不是查目录。它立刻报了两个不在:
`Include/<v>/um/winnt.h` 和 `bin/<v>/x64/rc.exe`。

它们**从来就没在过**。recipe 里那份注释写着
「`Windows SDK Desktop Headers x64` → um / shared 头文件」—— 那是我写的,而且是错的。

把每个 MSI 的 `File` / `Component` / `Directory` 三张表解出来
(7z 取 OLE 流,`!_Columns` 取 schema —— SDK 的 MSI 列宽**并不统一**,
按固定宽度猜会让后面每一列静默错位),算出每个文件的真实安装路径:

| 以为在 | 实际在 | 实际内容 |
|---|---|---|
| Desktop Headers x64 | — | **只有 4 个文件**:三个 Hyper-V 头 + 一个 catalog |
| Desktop Tools x64 | — | 99 个 bin 工具,**没有 rc,也没有 mt** |
| — | **Store Apps Headers** | 528 个 um 头:`windows.h` `winnt.h` |
| — | **Store Apps Headers OnecoreUap** | `shared/`:`windef.h` `sal.h` `winerror.h` |
| — | **Store Apps Tools** | `rc.exe` `rcdll.dll` `mt.exe` |

**一个 Win32 程序需要的每一样东西,都在名字写着 "Store Apps" 的 MSI 里;
四个写着 "Desktop" 的,给的是长尾、几个 bin 工具、三个 Hyper-V 头。**
和 §4.2 的 `CRT.x64.Store.base` 是同一个陷阱,这是第三层和第四层。

补齐 3 个 MSI + 14 个 cab(8 MSI + 35 cab,170 MB → 291 MB)。
其中 Tools 一个就 99 MB,换一个 200 KB 的资源编译器 —— 付了,
因为 `programs = {"rc", "mt"}` 写在包里,而那两个文件当时**不在磁盘上**。

`required_files()` 现在是**覆盖**而不是抽样:每条对应一个「只有它才提供」的
payload(`gdi32.lib` 只在 Desktop Libs —— 它的 365 个库和 Store Apps Libs 的
116 个**完全不相交**),所以哪个 payload 没下来/没解开,报错就点名哪一个。


补完之后**离线核过一遍全集**(把 8 个 MSI 的表并起来,算每个文件的安装路径),
一个 x64 桌面程序链接需要的东西全部到位,包括下一层本来会炸的 ucrt 库:

```
Lib/<v>/ucrt/x64/{libucrt.lib, ucrt.lib}      Include/<v>/ucrt/corecrt.h
Lib/<v>/um/x64/{kernel32,user32,gdi32,uuid}   Include/<v>/um/{windows.h,winnt.h}
bin/<v>/x64/{rc.exe, mt.exe}                  Include/<v>/shared/{windef.h,sal.h,winerror.h}
```

—— 这一步是**在 CI 发现之前**做的:这一轮已经反复出现「修好一层才够得着下一层」,
所以这次没有等它自己撞出来。

**无感升级这一条是顺带成立的,而且成立的理由值得记一笔。** `windows-sdk` 的版本号
没变(还是 `10.0.26100`),按「目录在就算装了」的判据,已经装了旧的那一份的人
**永远等不到这个修复**。

但 xim 决定「是否已安装」时会调 recipe 自己的 `installed()` 钩子
(`installer.cpp:2451` `executor.check_installed(ctx)`),而 `installed()` 现在
是**覆盖式**的七条文件断言 —— 旧的那棵树少 `winnt.h`、少 `windef.h`、少 `rc.exe`,
判据直接为假,于是重装。

也就是说:**把"装好了"的判据从"目录在"改成"能用",顺手把升级路径也修好了。**
这不是额外做的设计,是同一个决定的另一面。

### 4.9 mcpp 这边同一个病:SDK 只查了头文件 ⚠️

补完 SDK 之后,e2e 239 仍然挂在同一句:

```
LINK : fatal error LNK1104: cannot open file 'kernel32.lib'
```

`find_windows_sdk()` 认一个根的条件是 `Include/<v>/ucrt/corecrt.h` 存在 ——
**只看头文件**。而 SDK 是头文件**和**导入库两半。

于是半装的 payload「找到了」,而且因为版本号更高,**排在机器自己那套完整
SDK 前面**;每个 TU 都编过,一直到最后一步才炸,**日志里没有一行提到 SDK**。

这条和 §4.2、§4.8 是同一句话的第三种说法,只是这次在 mcpp 自己的代码里:
`has_usable_msvc()` 的注释早就写了「用比构建真正需要的更弱的信号去选工具链,
正是这个判据要防的 bug」—— 它对编译器坚持了两半,对 SDK 没有。

现在两半都要。半装的根被跳过,搜索落到下一个,**本来就能用的构建就能用了**。
三个测试各自对着旧判据跑过:headers-only 被拒、完整的仍然收
(否则「全拒」也能让测试变绿)、高版本的半装根输给低版本的完整根。
第一和第三条在没有修复时失败。

### 4.10 修好之后才够得着的下一个:remove 在 Windows 上失败

SDK 修好之后 e2e 239 的前三步全过了 —— 装上、用 payload 里的 cl 构建、
跑出 Hello、切回 `msvc@system` 仍然解析到系统 cl。第四步 remove 挂了:

```
error: remove failed: Access is denied.
```

`remove_all` 直接上,不清只读位、不重试、报错不说是哪个文件。payload 从
.vsix/.msi 解出来,归档条目带只读属性;而 POSIX 只要**目录**可写就能 unlink
子项 —— 所以这个问题在 Linux/macOS 上不可能出现,**而单元测试都跑在那边**。
另外 `/Zi` 构建之后 mspdbsrv.exe 还在 payload 里活几秒。

两个原因表现一模一样,所以两个都处理:清可写位重试 + 给活着的进程一个
**有上限**的窗口。并且报错点名卡在哪个文件。

> 没有配单元测试:唯一会跑它的平台上,删只读文件本来就成功 ——
> **那个测试不可能失败**。这一轮已经修过一个这样的测试(§4.5),不再造第二个。
> 判据是 Windows runner 上的 e2e 239。

---

#### 而它点名的那个文件,是第八层

加上「说出卡在哪个文件」之后,CI 立刻给出了答案:

```
error: remove failed: Access is denied.
         stuck at: ...\bin\Hostx64\x64\Microsoft.VisualStudio.Telemetry.dll
```

**两个猜测都不对** —— 不是只读位,也不是 mspdbsrv。占着它的是 `vctip.exe`:
cl.exe 拉起来的后台**遥测上传进程**,它比编译器活得久,而且就住在
payload 目录里面。于是这个 toolset **装得上、编得过、跑得起来,就是卸不掉**。

关键在于:**占用者不是 mcpp 能删掉的东西**。任何一边的 `remove` 都删不掉
另一个进程正打开的文件 —— `xlings remove msvc` 一样卸不掉。所以修复必须在
payload 那一层:recipe 解包后直接不装 `vctip.exe`。

删它安全,依据是 cl.exe **自己的字符串**,不是我猜的:

```
Not launching VCTIP: Binary not found @ '%S'
Not launching VCTIP: CLR not present
```

—— 二进制不在,是它本来就处理的一条分支。而且没有环境变量可以关掉它:
cl.exe 里没有任何遥测开关(vctip 自己的 `-upload:optout` 写的是**机器级**状态,
装个包不该动那个)。

顺带一提,一个构建工具链本来也不该因为你编译了一下就联网上报。

**另外修掉了我自己在这一层引入的一处:** 那个「找出卡住的文件」的探测,
第一版是**靠试删**来找的 —— 诊断变成第二次破坏。`remove_all` 已经把能删的
都删了,所以**活下来的第一个文件就是卡住的那个**,根本不需要再删。
同时报错现在明说:**失败的 remove 不是空操作**,剩下的是一个有洞的工具链。

#### 第九层:修好「不再安装」,不等于修好「已经装了的」

#637 合入并发布之后,mcpp #440 的 e2e 239 **还是挂在同一个文件上**。

原因很干脆:#637 让 recipe 不再**安装** vctip.exe,对**已经装了它的机器**
一点作用都没有 —— 而那正是卸不掉的那批机器。包的版本号不会因为 recipe 改了
就变,于是 xim 问 `installed()`,旧 payload 回答「装好了」,安装钩子直接跳过,
vctip.exe 原地不动。**修复被自己的判据挡在门外。**

所以 `installed()` 现在也检查 vctip.exe **不在**:

> `installed()` 的含义是「payload 处于**这份 recipe 产出的状态**」,
> 不是「这儿有个 payload」。两者只在 recipe 变更时不一样,而那正是它要紧的时候。

这和 §4.8 那条「无感升级顺带成立」是同一个机制,只是这次针对的是新 recipe
**删掉**的东西,而不是新增的东西 —— 加文件靠断言文件在就能发现,
删文件必须显式说「它不该在」。

**顺带解释了一件事:为什么索引侧的 CI 从来没发现它。** index 的 windows-test
做的是「装 → 检查 → 卸」,全程**不编译任何东西**。而 vctip.exe 是 cl.exe
**运行时**才被拉起来的 —— 不跑编译器,就没有进程,卸载自然成功。
所以 msvc.lua 前后几个 PR 的 windows-test 一路全绿,包括「post-uninstall checks」。

只有真正构建过东西的 e2e 才够得着这一条。**一个不运行被测物的验收步骤,
验的是解压。**

### 4.11 一条不是缺陷、但今天撞了五次:「合入索引」不等于「装得到」

| # | 任务 | 起跑 | 索引就绪 | 差 |
|---|---|---|---|---|
| 1 | xrgui CI | 07:16:55 | 07:17:35(产物发布) | 抢跑 40s |
| 2 | xrgui CI 重跑 | 07:20:28 | 07:17:29(指针已更新) | 指针 CDN 缓存 |
| 3 | mcpp #438 e2e | 07:36:38 | 07:41:20(tar 修复入索引) | 抢跑 4.5 min |
| 4 | mcpp #440 e2e | 10:31 起跑,239 跑在 10:52 | 10:26:49(SDK 补全发布) | **赶上了** —— 构建第一次链通 |
| 5 | mcpp #440 e2e 重跑 | 11:31 起跑,239 跑在 11:39 | ~11:40(vctip 修复发布) | 抢跑 ~1 min |

**这不是这次改动引入的**,但它意味着「合入 → 索引 → 装得到」之间有一个
肉眼不可见的窗口,而**窗口期内的失败信息是 `not found`** —— 与「这个包根本
不存在」一模一样。第 3 次尤其值得看:它让一个**已经修好**的缺陷看起来像
没修好,只有对着时间戳才分得清。

`xlings` 已经会说 `run \`xlings update\` if the package was just published` ——
那句提示正是为这个窗口写的,它是对的,只是在 CI 里没有人能照做。

第 5 次是我自己踩的:合入 #637 之后**没等发布完就重跑**了 mcpp 的 CI。
之后改成「先等 raw 索引里真的能 grep 到那行代码,再 push」——
也就是把判据从"合入了"换成"取得到",和这一整轮的主题是同一句话。

小插曲,同一个毛病的第三种形态:第一版等待脚本 grep 的模式**少写了一个右括号**
(`vctip.exe") then` vs 实际的 `vctip.exe")) then`),于是它永远等不到 ——
而"等不到"和"还没发布"输出一模一样。**连我用来检查判据的东西,判据本身也得能失败。**

**而且第 5 次差点把结论带偏**:它失败在同一个文件上,看起来像"修复没用",
实际有两个原因叠着(抢跑 + 老 payload 不会自动重装)。分清它们靠的是时间戳
和"runner 上那份 payload 是 #637 之前装的"这个事实,不是重跑一次看看。

---

## 4b. 沙箱真实验证(`xlings subos ... --sandbox --cmd`)

在**干净 subos** 里,对**已发布的 2026.8.16.2 二进制**做的验证:

| 验的是什么 | 命令 | 结果 |
|---|---|---|
| 索引装得到 mcpp | `xlings update && xlings install xim:mcpp` | ✅ `xim:mcpp@2026.8.16.2` 装上 |
| xlings 生态本身可用 | `xlings install gcc` → 编译 → 运行 | ✅ `eco ok` |
| **两条来源在发布产物上真的分开了** | `mcpp toolchain install msvc system` | ✅ `the msvc toolchain is only available on Windows hosts`(探测机器那条路) |
| 同上,受管那条 | `mcpp toolchain install msvc 14.44.35207` | ✅ `Installing msvc@14.44.35207 via mcpp's xlings`(走 xim,**不是**宿主守卫) |

最后两行是这轮架构改动**在发出去的二进制上**的判据:同一个命令、只差一个版本号,
走的是两条完全不同的路径。

### 两处已知、且与本轮无关的现象(如实记)

1. **`xlings info msvc` 仍然说 `not found`。** 平台感知的新消息在 xlings#550
   里(2026-08-16 15:16 合入),而最新 release tag 是 `v2026.8.14.1`,
   本机 0.4.51 **早于**那个修复。CI 上 E2E-76 带新断言通过,证明消息确实产生了。
2. **干净沙箱里 mcpp 自举 gcc 失败(exit 127)。** 装出来的 gcc@16.1.0 其 ELF
   解释器指向一个本机早已不存在的目录;xlings 自己 store 里的同一个 payload
   解释器是对的 —— 说明发布产物没问题,是 mcpp 自举那步没打上 patchelf。
   mcpp 自己的日志点名了原因:SubOS 缺 `subos_info`(**openxlings/xlings#547**)。
   **已知在跟踪,与 MSVC 这轮无关,没有动它。**

---

## 4c. 第十、十一层:改名改错了东西,以及"卸载"的判据

vctip 修好之后,占用者换成 `mspdbcore.dll` —— `/Zi` 构建拉起的 **mspdbsrv.exe**,
它比 cl.exe 多活几十秒,而且就住在正要被删的 payload 里。等它不现实(等多久都是猜)。

### 我先前的前提是错的,CI 当场否掉

我写的是:「Windows 拒绝删除含打开文件的目录,但允许**重命名**」,于是去改名
payload 目录。**错在哪:** Windows 允许重命名一个**打开着的文件**(更新器就是这么
替换运行中的 .exe),但**不允许**重命名一个**含有**打开文件的目录。改名失败,
remove 照样报错。

正确的形状是反过来的:**把文件逐个挪出去**,再删掉此时只剩目录的那棵树。

### 然后是第十一层:空目录也删不掉

文件都挪走了,`remove_all` 仍然失败 —— 这次是**目录**上的 sharing violation
(某个进程把它当工作目录;mspdbsrv 正是在 payload 里被拉起来的)。

判据因此定为 **「没有文件残留 = 已卸载」**:
**一个没有文件的工具链就是没装**,这正是 `remove` 承诺的事;
报失败等于报了相反的事实。骨架在下一条生命周期命令里清扫。

而这立刻照出对称的另半句谎:`toolchain default` 原来只查目录**存在**,
会把骨架当成装好的工具链交给构建。现在它问 `payload_frontend` 要一个
**能解析出来的编译器**。

> 又是同一句话:**装好了 = 能用,不是"在那儿"。**

### 测试:第三次得出"这个测试不可能失败"

POSIX 上唯一能让文件删不掉的办法是去掉父目录的写权限 —— 而这个函数的**第二遍**
正是要把写权限加回来(那是它要修的只读 payload 那条)。**fixture 在被测代码碰到
它的那一刻就变成可删的了。** 那是函数在正常工作,不是漏洞。

所以两个依赖该 fixture 的测试删掉了,留下的是在任何平台都成立的那两条
(普通 payload 走普通路径、sweep 只吃 `.trash-*` 不吃装好的 toolset)。
判据是 Windows runner 上的 e2e 239。

---

## 4d. review 落地了什么

| 项 | 落在哪 |
|---|---|
| §3b `doctor` 还留着 #436 修掉的第四份布局拷贝 | mcpp#440 |
| §3c `has_usable_msvc` 把"机器有 VS"当成"这里能用 MSVC" | mcpp#440 |
| §3 `/MD` 产物在干净 Windows 上跑不起来(`vc_redist_dir` → PATH) | mcpp#440 |
| §3e 主构建与 host helper 链接策略不一致(**#437**) | mcpp#442 |
| §4 `installed()` 语义写进 skill §2.2.1 + lint | index#640 |
| §5 windows-test **真的运行**声明的程序 | index#640 |
| §6 park-and-sweep 下沉到 xlings 卸载路径 | xlings#551 |

**没做的四项**(§1 收拢 Origin、§2 SDK 绑定 + 版本轴、§3d PE 版 pack、§7 索引修订号)
都要动 spec/schema 层或跨仓库,不适合塞进发版窗口 —— 在 review 里写到了可以直接开工的程度。

### 一处我自己造出来的问题,已作废

review 初稿把 `is_system_toolchain()` 里的 `Family::Msvc` 读成"不对称,应该推广",
提议支持 `gcc@system`。**方向反了**:xlings 是用户态 OS,host 依赖是要**降到最低**的。
被纠正后我又去给自己发明的拼写写了个"显式拒绝"(#441),那是从自己的错误里长出来的
新范围,已关闭。而且那条消息还说错了 —— `prepare.cppm:1376` 有一条**故意保留**的
裸 `system` 逃生口,真正提供 host 依赖的那一处**不带族名**。

---

## 5. 发布与 gitcode 资源

- **mcpp 2026.8.16.1**:四平台产物 + 别名 + `SHA256SUMS` 共 20 个资产,
  release CI 全绿。**发布产物本身验过**:下载 linux-x86_64 跑
  `mcpp toolchain install msvc 14.44.35207`,走的是 xim 路径而不是
  `msvc_wrong_host()` —— 版本轴分流在**发出去的二进制**上生效。

  ⚠️ 这个 tag 打在 #436 / #438 **之前**,所以它带着 §4.3 那条
  「装好的 toolset 不出现在 `toolchain list` 里」。两个修复合入后应当补一个
  `2026.8.16.2`,让发布版本与 main 一致 —— 已列入待办,`.1` 的其余部分
  (安装、构建、default、remove)不受影响。
- **gitcode 镜像**:四个平台产物都在
  `gitcode.com/xlings-res/mcpp/releases/download/2026.8.16.1/`(206 实测)。
  注意 tag **不带 `v` 前缀** —— 第一次我按 `v2026.8.16.1` 探测得到 404,
  差点误报成「镜像没传上去」。
- **MSVC 生态资源**:29 个 payload(27 + #630 新增的 2 个 Store)手动
  `gtc release upload` 到 `xlings-res/{msvc,windows-sdk}`,逐个下载校验
  sha256 与微软一致。

---

## 5b. 发布 2026.8.16.3:`publish-ecosystem` 连续两版卡在同一步

| | 结果 |
|---|---|
| 四平台产物 + manifest 封存 | ✅ 全部成功 |
| GitHub release 21 个资产 | ✅ 齐 |
| `publish-ecosystem` 的 gitcode 镜像步 | ❌ **10 分钟超时,重跑仍然超时** |

`.2` 那次也是同一步超时(当时重跑成功了)。**连续两个版本卡在同一位置,
已经不是 flake。** 值得当成一条待办给 pipeline。

按授权用本地 `gtc` 补齐 14 个缺失资产,再**下载回来逐个哈希**:**21/21 与本地一致**。

### 又一次:HEAD 不能当判据

第一次扫描镜像用的是 HTTP HEAD,结论是"21 个全缺"。改用真实 GET 才发现
**实际是 19 缺 2 在** —— gitcode 对确实存在的文件也会给 HEAD 返回非 200。

要是照着 HEAD 的结论去传,就会对着已经在的两个重复上传;
要是反过来(HEAD 说在、实际不在),就会漏。**判据必须是"字节回来了且摘要一致"**
—— 和 §4.1 那 16 个文件是同一课,只是这次我差点自己又栽一遍。

索引 bump 用的是 release pipeline 自己的 `version-check.py --apply --only mcpp`,
所以 sha256 是**从已发布产物算出来的**,不是手抄。

---

## 6. 镜像:地址会失效,字节不会

27 个 payload(325 MB)镜像到 `gitcode.com/xlings-res/{msvc,windows-sdk}`,
官方 CDN 作为**最后**一个回退。**sha256 规则一行没动** —— 从哪个地址取到都按同一个
摘要校验,所以镜像不是第二个可信来源,是同一份字节的第二个地址。

回退的两个前提都单独验过:`curl -f` 遇 404 退 22 且不留文件(所以 `pcall` 接得住)、
官方地址仍服务同样的字节。走到第一个之后的地址会 `log.warn` ——
**看不见的回退等于没有回退**。

---

## 7. 明确没有覆盖的部分

1. **「用受管 toolset 真的链接一次」这条,在写这份报告时正在 CI 上跑第一次**。
   mcpp#438 修好 e2e 239 的 skip 判据之后,加上索引里的 tar 修复(`8b291db`),
   239 会第一次真的装 `xim:msvc@14.44.35207` 并用它构建一个 hello。
   §4.2 那条缺陷能走到发布,正是因为在此之前**没有任何 CI 用这条 toolset
   链接过东西**;补强 `installed()` 只是让「文件在不在」变严,
   **不等于「链接得通」**。

   xrgui 目前用的仍是 `msvc@system`(见 §2 —— 那一条腿存在的意义是
   「只有构建系统不同」,pin toolset 会让两条腿同时差两件事)。
   用受管 toolset 构建 xrgui 是下一步,不是这一轮。
2. **`msvc@14.52.36629` 的安装路径没在 CI 上跑过**(windows-test 装的是 14.44)。
   本机按 recipe 的方式解包了 14.52 的五个 payload,`installed()` 要的四个文件
   加上 `/MD` 的其余部分(`msvcrt.lib` / `vcruntime.lib` / `oldnames.lib`)
   **全部就位** —— 但那验的是解包,不是 Windows 上的安装。
3. **镜像回退没被真正触发过** —— 两个前提验过了,完整路径没执行过。
4. **D4 的控制台那一半仍然开着**。管道那条路量到了(零撕裂,且样本是**修复前**的
   2026.8.10.1,所以结论是「管道从来没坏过」,这收窄了 D4 而不是回答它);
   CI 结不了控制台的案 —— runner 上 job 没有附着的控制台。
5. **gitcode 的 probe 资产删不掉**(API 两个路径都 404),已在镜像 README 里点名。
6. **index CI 用 4 个版本以前的 xlings 验证 recipe**(`ci-test.yml` 钉
   `v2026.8.10.1`)。没在这里改 —— 换版本可能影响别的 recipe,应当是它自己的 PR。
7. **Linux 侧沙箱里 `mcpp` 自举 gcc 失败,不是这轮引入的。**
   在干净 subos + 干净 `MCPP_HOME` 里,mcpp 装的 gcc@16.1.0 其 ELF 解释器指向
   一个**本机早已不存在的目录**,`g++ --version` 直接 127。
   xlings 自己 store 里的同一个 payload 解释器是对的 —— 也就是说发布的产物没问题,
   是 mcpp 自举那一步没打上 patchelf。mcpp 自己的日志点了名:
   SubOS 缺 `subos_info`(**openxlings/xlings#547**),所以它「拒绝猜一个版本」。
   属于已知在跟踪的问题,与 MSVC 这轮无关,**没有在这轮动它**。
   xlings 生态本身在同一个沙箱里验过是通的:`xlings install gcc` → 编译 → 运行 → `eco ok`。
8. **`info` 的修复已合入但尚未发布**。本机 `xlings` 是 0.4.51,还是旧的,
   所以在这台机器上跑 `xlings info msvc` 依然是 `not found`。
   证据来自 CI:E2E-76 带着新断言(`no build for` + 必须点名平台)通过,
   97/97 全绿 —— 也就是说新消息**确实产生了**,只是还没到本机。

---

## 7b. 自我 review:在自己刚打完 tag 的代码里又找到两条

`2026.8.16.3` tag 打完之后,我对 `v2026.8.16.2..v2026.8.16.3` 的 diff 做了一次
独立 review,抓到两条真的:

**1. 清扫会误删另一个正在解压的版本(🔴 数据损失)**

`sweep_parked_payloads` 扫整个 family 目录,删掉**任何**没有文件的版本目录。
但安装是**逐步**往目录里写文件的 —— 这正是 `package_fetcher` 用标记文件而不是
"目录在"判断装完没有的原因。于是另一个进程半解压的目录和"残骨架"短暂无法区分,
而共用一个 `MCPP_HOME` 的机器(自托管 runner、共享开发机)上两个 mcpp 同时跑
是常态,**恰恰就是 park/sweep 这套东西存在的理由**。

修法:`.trash-*` 照旧全扫(那个名字只有这段代码会写);
"没有文件的骨架"收窄成**只扫这条命令点名的那一个版本**。

**2. 一个 yes/no 问题在起编译器(延迟回归)**

`msvc_available_here()` 只想知道"这儿有没有能用的 toolset",却走完整
`installation_at()` —— 里面跑 cl.exe 拿 banner 定**版本号**,然后把它丢掉。
而这个判据在**每次构建**的 MSVC ABI 门上都被问到,装了多个 toolset 的机器
每次构建付好几次子进程 —— 正好是这个判据当初为之添加的那类机器。

修法:`installation_at(..., identifyVersion=false)`。
**布局仍然只有一份实现** —— 是加参数,不是再抄一遍路径拼接
(那正是 review §1 骂的事)。

新测试对着旧的全目录扫法跑过,**会失败**。

> 这一节存在本身就是判据:**"CI 全绿"和"这段代码是对的"是两件事。**
> 两条都是 19/19 绿之后才被读出来的。

---

## 8. 按你给的八个角度逐条对照

| 角度 | 这轮做了什么 | 依据 |
|---|---|---|
| **架构** | MSVC 拆成**两条正交的轴**:「获取」与 gcc 共用(xim),「解析」与 `msvc@system` 共用(`installation_from_tools_dir`)。受管 toolset 因此不是第二条代码路径,长不出自己的 bug | §2 |
| **稳定性** | 九层缺陷全部收口,每一层都补了**对着缺陷跑过**的测试;三处「不可能失败的测试」被识别出来,其中一处直接不写(平台上删只读文件本来就成功) | §4 |
| **优雅简洁** | 判据统一成一句话:**「装好了」= 能用,不是「解压完了」**。`installed()`、`find_windows_sdk()`、`has_usable_msvc()` 现在是同一条规则的三处应用;重复的布局拼写合并回 `toolsdir()`/`payload_frontend()` | §4.8–4.10 |
| **用户体验** | 报错说出**具体是哪个文件**(缺哪个 payload / 卡在哪个 DLL),并说清「失败的 remove 不是空操作」;99 MB 换 rc+mt 是明账,写在 recipe 头部 | §4.8、§4.10 |
| **兼容性** | `msvc@system` 语义**一字未改** —— e2e 239 专门有一条反向断言:切回 `msvc@system` 必须解析到系统 cl,否则失败 | §2、§3 |
| **跨平台** | msvc 发现逻辑第一次能在 **Windows 之外**测试(收目录/收 root 列表,不再被平台宏包住),新单测跑在 Linux CI 上;SDK 库路径按**宿主架构**拼,不写死 x64 | §3 |
| **一致性** | msvc 与 gcc 现在同构:同样的 `xim:` 获取、同样的 `payload_frontend`、同样的多地址 + sha256 镜像规则 | §2、§6 |
| **无感升级** | 版本号没变也能把老机器拉回来 —— 靠的是 `installed()` 忠实描述「**这份 recipe 产出的状态**」:新增的文件靠断言它在,**删掉**的文件靠断言它不在 | §4.8、§4.9(第九层) |

**如果只留一句**:这九层缺陷没有一层是"写错了",全都是**验收判据比"能用"弱**。
把判据改成"能用"之后,它们一层层自己冒出来了。

---

## 9. 复现用的工具

`msimap.py`(离线解 MSI 的 File/Component/Directory 表,算每个文件的安装路径)
留在 scratchpad 里。它是这轮唯一能回答"这个 MSI 里到底有什么"的东西 ——
**payload 的名字、微软的文档、我自己写的注释,三个都错了,只有表是对的。**
