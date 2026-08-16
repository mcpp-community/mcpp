# MSVC 在 xlings 生态里打通 —— 跨仓库计划、依赖与验收(2026-08-16)

> 配套 `2026-08-16-msvc-as-a-managed-toolchain.md`(mcpp 侧设计)。
> 那份讲**为什么这么改**;这份讲**改动落在哪几个仓库、谁挡着谁、以及每一步凭什么算通过**。

---

## 0. 一句话

四个仓库、五条改动,只有**一条硬依赖**:包必须先发布,mcpp 才装得到,xrgui 才用得上。
其余都可以并行,而把它们排成一条线是这类工作最常见的浪费。

---

## 1. 从八个角度看这次改的到底是什么

用户点名的八个角度不是修辞,它们各自对应一条具体改动。逐条落到实处:

| 角度 | 改前的具体事实 | 改后 |
|---|---|---|
| **架构** | MSVC 是工具链体系里唯一「无法声明版本」的家族;`is_system_toolchain()` 对**所有** msvc spec 为真 | 版本轴决定来源。**获取**与 gcc 同路(xim 安装),**解析**与 `msvc@system` 同路(`installation_from_tools_dir`)——两条轴正交,受管 toolset 不是第二条代码路径 |
| **稳定性** | 同一份源码在两台机器上被不同编译器编译**且不报错**;xrgui#3 实测 mcpp 用 14.51、xmake 用 14.52,直到 ICE 才暴露 | 声明了版本就必须拿到那个版本,拿不到是 nullopt 而不是替代品 |
| **优雅简洁** | 两个旋钮(`linkage` / `cxx_runtime`)指向同一个物理开关,只有一个管用,注释互相矛盾 | 一个 `msvc_wants_static_crt()`,项目 TU 与 std 模块问同一个函数 |
| **用户体验** | 没装 VS 时告诉你「mcpp 不安装 MSVC,自己去装」——而现在这句话是假的 | 两条路都给出:装一个 pin 住的 toolset,或用机器自己的 VS。`install_guidance()` 里两条命令都能直接抄 |
| **兼容性** | —— | `msvc@system` 语义一字未改;唯一的破坏性变更(`msvc@19.44`)有精确的替代指引,而它原本**只在一个命令里生效、构建路径完全忽略** |
| **跨平台** | msvc 发现逻辑一行都无法在 Windows 之外测试(入口全在 `#if defined(_WIN32)` 里) | `installation_at()` 收目录、`find_windows_sdk()` 收 root 列表,6 个单测在 Linux CI 上跑真 fixture |
| **一致性** | `search` 说 `xim:msvc` 在,`info` 说 `not found` —— 同一台机器、同一个索引 | xlings#550:区分「不存在」与「这个平台没有构建」,并说出它在哪些平台有 |
| **无感升级** | —— | 现有工程零影响:`msvc@system` 不变、默认 CRT 仍是 `/MD`(判据取 manifest 字面值而非解析后的 contract,否则每个 Windows 构建都会翻成 `/MT`) |

---

## 2. 五条改动与它们的仓库

| # | 仓库 | 改动 | PR |
|---|---|---|---|
| **X** | xim-pkgindex | payload 多来源 `urls` + 27 个 payload 镜像;windows-sdk 导出 `WindowsSdkDir`/`WindowsSdkVersion` | #629 ✅ 已合 |
| **M** | mcpp | A 受管 toolset / B SDK 搜索顺序 / C CRT 双入口 / D 发现顺序 | #434 |
| **L1** | xlings | D4 的管道那一半量到了(文档) | #549 |
| **L2** | xlings | `info` 对别的平台的包说 "not found" | #550 |
| **V** | xrgui | 删掉 vswhere workaround,验证整条链 | 待 mcpp 发布 |

---

## 3. 依赖图 —— 边是真实依赖,不是先后偏好

```
X (xim-pkgindex #629)
│   包必须先发布:`mcpp toolchain install msvc 14.44.35207` 装的就是它
│   在此之前 mcpp 的 e2e 239 走 SKIP 分支,而不是红
▼
M (mcpp #434) ──→ 发布 2026.8.16.1 ──→ V (xrgui)
                                        │
    L1、L2 与上面这条链 无 依赖 ────────┘(可随时合)
```

**为什么 C、D 没有拆成独立 PR**:C 与 A/B 确实无依赖,但它回答的是同一个问题
——「MSVC 在 mcpp 里到底怎么被描述」;拆开会让 CHANGELOG 的读者以为是两件事。
D 与 A 改同一个函数,拆开的第二个 PR 必然要重写第一个 PR 刚写的注释。

**为什么 L2 不在 M 里**:它是 xlings 的缺陷,是**在验证 M 的过程中**被发现的
(`xlings info msvc` 在 Linux 上说 not found),但它与 MSVC 无关 —— 每一个
windows-only 包在 Linux 上都会这样,反之亦然。

---

## 4. 验收标准 —— 哪些能自证,哪些不能

这一节是这份计划的重点。**能被"调一下测试"满足的判据,不算证据。**

| 判据 | 能否自证 | 说明 |
|---|---|---|
| 27 个 payload 镜像正确 | **不能** | 判据不是「上传成功」,是**下载回来 sha256 与微软一致**。而这恰好抓到了真问题:上传工具对 16 个文件报了失败、release 列表却显示它们在,实际 404。**谁都不能信,只能信字节。** |
| 受管 toolset 真的被用了 | **不能** | e2e 239 的每一条断言都写成「系统编译器来应答就会失败」:cl.exe 必须在 mcpp 的 store 里、toolset 目录必须是 spec 声明的那个 |
| 两条来源互不污染 | **不能** | 同一台机器、同一个项目,spec 换回 `msvc@system` 必须解析到系统 cl。少了这条,「受管能用」与「受管把一切都换掉了」长得一样 |
| 声明的 toolset 优先于"最新" | **不能** | 单测:两个 toolset 都在,要**老的**那个。「取最新」的实现会在这里失败,而真机上它可能碰巧对 |
| **xrgui 删掉 workaround 后仍然绿** | **不能** | 全套里最强的一条。workaround 还在时,「`VSINSTALLDIR` 被采纳」与「vswhere 找不到东西」现象完全一样 —— **无法区分缺陷 A 是否真修好**。而且删掉之后 vswhere 与 14.51 **都还在**:错误答案没有被拿走,它只是必须输 |
| 单测 83/83、静态检查 1788 项 | **能** | 有用,但它们证明的是「没有回归」,不是「这件事做成了」 |

---

## 5. 验证过程本身抓到的:包好的 toolset 根本链接不了默认构建

这条不在计划里,是**做第 4 节那份「不被覆盖」清单时查出来的** —— 我去检查
「打包的 14.52 能不能构建 xrgui」,答案是不能,而且对**任何**项目都不能。

payload 集里只有**静态** CRT。`.CRT.x64.Desktop.base` 带的是
`libcmt` / `libcpmt` / `libvcruntime`,而默认的 `/MD` 需要 `msvcprt.lib`。
判据是头文件自己写的(`use_ansi.h`):

```c
#if defined(_DLL) && !defined(_STATIC_CPPLIB)
#define _LIB_STEM "msvcprt"      // /MD  ← payload 集里没有
#else
#define _LIB_STEM "libcpmt"      // /MT  ← 有
```

`msvcprt.lib` / `msvcrt.lib` / `vcruntime.lib` / `oldnames.lib` 在
`Microsoft.VC.<ver>.CRT.x64.Store.base` 里。**名字是它被跳过的原因** ——
听起来像 UWP。它确实也带 `uwp/` 和 `store/` 子目录,但**桌面**用的动态导入库
就放在 `lib/x64` 顶层,而且别处都没有。已为两个 toolset 补上(xim-pkgindex#630)。

**真正的缺陷不是少了一个 payload,是没有任何东西会发现它。**
`installed()` 只查 `cl.exe` 和 `std.ixx` —— 两个都在。于是安装报成功,
真 Windows runner 上的 `windows-test` 全绿,而这条工具链**在它的默认配置下不可用**;
失败会出现在用户的链接步骤里,离原因三层远。

现在 `installed()` 对每种 CRT 模型各查一个导入库(`/MT` 的 `libcpmt.lib`、
`/MD` 的 `msvcprt.lib`),两者都不能再用同样的方式静默消失。

> 这也是对 §4 那张表的一个注脚:「1790 项静态检查通过」与
> 「windows-test 在真机上装成功」都是真的,而这条工具链同时是坏的。
> **一个比"能用"弱的"装好了"判据,报的不是安装成功,是解压成功。**

---

### 5.1 第二条:装好的 toolset 在 `toolchain list` 里根本不出现

拿**已发布的** 2026.8.16.1 二进制对着一个 payload 形状的 fixture 跑,发现
装好的 msvc toolset 一行都不显示。

枚举问的是 `toolchain_frontend(root / "bin", pkg)`,而 cl.exe 在
`VC/Tools/MSVC/<ver>/bin/Host<h>/<arch>/`,深四层。拿不到 → `continue` → 消失。

**这个布局有三个地方需要知道:安装知道、构建知道、列表不知道。**
第三份内联副本就是这么来的。现在三者共用 `payload_frontend()`(#436)。

值得记的是它**怎么被发现的**:不是测试。为此写的单测钉的是
`identify_xim_payload("msvc")` —— 那一条本来就是对的。
**「身份映射」与「枚举」是两个问题,而只有一个被问了。**

> e2e 239 的 1b 步会抓到它 —— 但要等包发布之后 Windows e2e 再跑一轮。
> 测试写在了对的粒度上,只是还没轮到它跑。**「有测试」和「测过了」不是一回事。**

---

## 6. 已知不被覆盖的部分(不要当成已完成)

1. **没有任何 CI 用这条 toolset 链接过东西**。这正是上面那条缺陷能一路走到
   发布的原因,而补上 `installed()` 的检查只是让「文件在不在」变严,
   **不等于「链接得通」**。真正能覆盖它的是 xrgui 的 V3(用
   `msvc@<toolset>` 构建一个真实项目),那一步还没跑。
2. **`msvc@14.52.36629` 的安装路径没有在 CI 上跑过**。index 的 `windows-test`
   装的是 `latest`(14.44)。两者只差 payload URL 与目录版本,后者已逐个从真实
   payload 读出核对,但**没有实际装过一次**。
2. **镜像回退没有被真正触发过**。两个前提单独验过了 —— `curl -f` 遇 404 退 22
   且不留文件(所以 `pcall` 会接住、`os.isfile` 为假),官方地址仍然服务同样的
   字节 —— 但「镜像挂掉时自动走官方」这条完整路径没有被执行过。
   现在至少它**不会静默**:走到第一个之后的地址会 `log.warn`。
3. **D4 的控制台那一半仍然开着**。管道那条路量到了(xlings#549),
   而 CI 结不了控制台的案:runner 上 job 没有附着的控制台。
4. **gitcode 的 probe 资产删不掉**。API 没有删除端点(两个路径都 404),
   已在镜像 README 里点名说明,而不是留一堆没人知道是什么的文件。
