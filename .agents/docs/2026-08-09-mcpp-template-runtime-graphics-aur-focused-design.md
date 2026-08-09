# mcpp 模板、运行时、图形栈与 AUR 聚焦设计

> 状态：Review Draft
>
> 日期：2026-08-09
>
> 基线：mcpp <code>main@80291ca01a98</code>
>
> 上位分析：<code>.agents/docs/2026-08-09-xlings-mcpp-ecosystem-convergence-design.md</code>
>
> 本文只冻结四项产品设计，不代表实现、发布或 AUR 状态已经改变。

## 1. 已确认的四项产品决策

本文将以下内容视为已确认方向，不再把旧方案并列为推荐项：

1. **模板 selector 与 <code>mcpp add</code> 保持同一风格**
   - 稳定形式：<code>ns.name@version:tname</code>。
   - <code>ns.</code> 可省略；省略时使用 mcpp/mcpp.toml 的默认 namespace <code>mcpplibs</code>。
   - <code>@version</code> 可省略。
   - <code>:tname</code> 可省略。
   - 不引入 <code>--variant</code>。
2. **xlings 拥有图形栈与运行时，mcpp 构建在该生态之上**
   - xlings 负责图形 runtime/provider 的解析、安装、激活和生命周期。
   - xim-pkgindex 提供 Mesa、NVIDIA、WSL、Vulkan ICD/driver 等 package/provider recipe。
   - mcpp-index 声明 C++ 图形包到 xlings/xim 图形能力的依赖关系。
   - mcpp 只通过 mcpp-index 的包描述、模板和通用构建契约完成构建。
   - mcpp 不探测 GPU，不按厂商写构建分支。
3. **运行时分为 mcpp 默认与项目显式 SubOS**
   - 不配置时使用 mcpp 管理、发布验证过的默认运行时。
   - 项目可在 <code>mcpp.toml</code> 的 <code>[xlings]</code> 中选择命名 SubOS。
   - SubOS 是根项目本地 build/run 环境，类似选择“在哪个 OS 开发”；它不是库的传递依赖，也不要求消费者使用同一 SubOS。
   - 多个项目可以并存于不同 SubOS/不同 glibc，缓存和产物按实际 RuntimeBinding 隔离。
4. **AUR 自动同步聚焦 <code>mcpp-bin</code>**
   - <code>mcpp-bin</code> 是唯一自动对账并纳入漂移告警的 AUR 包。
   - AUR 是 GitHub Release 的最终一致投影，AUR 故障不阻塞 GitHub Release 本身。
   - <code>mcpp-m</code> 本阶段完全不动：不修改 package 文件、不推送、不退役、不改变其现有状态。
   - <code>mcpp-git</code> 不进入主发布关键路径。

## 2. 目标、边界与成功定义

### 2.1 目标

- 用户只学习一种包 selector 风格；namespace 可省略，省略时默认 <code>mcpplibs</code>。
- 默认用户无需理解 xlings/SubOS 即可获得稳定运行时；需要固定 ABI/环境的项目能声明 SubOS。
- 图形包的新平台、驱动和 backend 由 xlings/xim 生态演进；mcpp-index 表达 C++ 依赖关系，不要求修改 mcpp 核心。
- SubOS 只影响当前根项目本地构建/运行，不从库依赖向消费者传递。
- AUR 临时故障恢复后，<code>mcpp-bin</code> 能自动收敛到最新稳定 GitHub Release。
- 设计保持 CLI 简洁、内部身份明确、构建热路径无网络和 GPU 探测。

### 2.2 非目标

- 不在本轮增加 <code>--variant</code>、<code>#template</code> 或新的顶层 template 命令。
- 不让项目 <code>mcpp.toml</code> 写本机绝对 xlings binary/home。
- 不在 mcpp 中实现 Mesa/NVIDIA/Vulkan/WSL 专用逻辑。
- 不要求 <code>mcpp-m</code> 与每次 release 同步。
- 不修改或重新定义 <code>mcpp-m</code> 的维护策略。
- 不在本文中解决 C1–C9 的全部实现；只保留与这四项直接相关的契约。
- 本轮不实现代码、不重跑 workflow、不推送 AUR。

### 2.3 成功定义

~~~text
mcpp new app --template ocornut.imgui@1.92.8:glfw-opengl3
  -> dotted selector 显式解析为 namespace=ocornut, name=imgui
  -> 解析出完整 PackageId 与 exact version
  -> 从 mcpp-index 获取模板/构建描述
  -> mcpp-index 声明 xlings/xim 图形依赖
  -> xlings 解析并物化图形运行时
  -> 使用 mcpp 默认 runtime 或 [xlings].subos
  -> mcpp 只执行通用 build/link/run plan
~~~

发布侧：

~~~text
latest stable GitHub Release
  -> immutable release manifest
  -> mcpp-bin generator and validation
  -> idempotent AUR reconcile
  -> remote git + AUR RPC + clean install verification
~~~

## 3. 方案比较

| 主题 | 方案 A | 方案 B | 方案 C | 选择 |
|---|---|---|---|---|
| 模板名 | <code>--variant</code> 独立参数 | <code>#tname</code> 新分隔符 | <code>[ns.]name[@version][:tname]</code> | **C**：与现有 CLI/TOML 风格一致 |
| runtime 配置 | 跟随全局 active SubOS | 新增 <code>[runtime].provider</code> | 默认 mcpp runtime；已有 <code>[xlings].subos</code> 覆盖 | **C**：字段最少、可复现 |
| graphics | mcpp 内置 GL/Vulkan/provider 逻辑 | mcpp 直接调用 xim 图形接口 | xlings 拥有图形 runtime；mcpp-index 声明 C++ 依赖 | **C**：职责单一 |
| AUR | 双包串行同步 | 三包 matrix 同步 | 只自动对账 <code>mcpp-bin</code> | **C**：主用户路径优先 |

方案 C 的共同原则是：**用户表面复用已有概念，内部不复用含糊字符串；每层只拥有自己能证明的事实。**

## 4. 总体架构

~~~mermaid
flowchart LR
    U[mcpp.toml / CLI] --> M[mcpp generic resolver and builder]
    M --> I[mcpp-index<br/>package, template, build contract]
    I --> X[xlings<br/>graphics stack owner, install, SubOS]
    X -->|reads provider recipes| XI[xim-pkgindex<br/>provider recipes and sentinels]
    X --> RC[resolved graphics and runtime contract]
    RC --> M
    M --> A[artifact]

    R[GitHub Release Manifest] --> AR[AUR mcpp-bin reconciler]
    AR --> AB[AUR mcpp-bin]
~~~

核心依赖方向：

- mcpp 可以理解 PackageSelector、PackageId、LinkIntent、RuntimeBinding 等通用结构。
- mcpp 不理解 <code>mesa</code>、<code>nvidia</code>、<code>wsl</code>、<code>vulkan icd</code> 的选择规则。
- mcpp-index 声明图形包如何编译、链接，以及它依赖哪些 xlings 生态 package/capability。
- xlings 负责解析、安装、激活和导出图形 runtime contract。
- xim-pkgindex 是 xlings 消费的 provider/recipe 数据源，实现 capability 并记录 artifact/provenance。
- AUR reconciler 不读取工作区临时状态，只读取不可变 release manifest。

## 5. 模板 selector 统一设计

### 5.1 用户语法

稳定文法：

~~~text
TemplateSpec     := PackageSelector [ "@" ExactVersion ] [ ":" TemplateName ]
PackageSelector := Name | NamespacePath "." Name
NamespacePath   := Segment { "." Segment }
TemplateName    := NameAtom
~~~

<code>NamespacePath</code> 可省略。省略时：

~~~text
imgui  -> namespace = mcpplibs, name = imgui
~~~

它不是全索引 short-name 搜索，也不是“跟随当前 index 的 namespace”；默认值固定为 <code>mcpplibs</code>，与 mcpp.toml 默认 dependency table 对齐。

分隔符唯一职责：

| 分隔符 | 含义 |
|---|---|
| <code>.</code> | 与 <code>mcpp add</code> 相同的 dotted package selector |
| <code>@</code> | exact package version |
| <code>:</code> | package 内的 template name |

合法示例：

| 输入 | package selector | version | template |
|---|---|---|---|
| <code>imgui</code> | <code>mcpplibs.imgui</code>（ns 省略） | 默认稳定版 | descriptor default |
| <code>ocornut.imgui</code> | <code>ocornut.imgui</code> | 默认稳定版 | descriptor default |
| <code>ocornut.imgui@1.92.8</code> | <code>ocornut.imgui</code> | <code>1.92.8</code> | descriptor default |
| <code>ocornut.imgui:glfw-opengl3</code> | <code>ocornut.imgui</code> | 默认稳定版 | <code>glfw-opengl3</code> |
| <code>ocornut.imgui@1.92.8:vulkan</code> | <code>ocornut.imgui</code> | <code>1.92.8</code> | <code>vulkan</code> |
| <code>mcpplibs.gui.templates@2.0.0:window</code> | <code>mcpplibs.gui.templates</code> | <code>2.0.0</code> | <code>window</code> |

非法示例：

| 输入 | 错误 |
|---|---|
| <code>ocornut.imgui@</code> | empty version |
| <code>ocornut.imgui:</code> | empty template name |
| <code>.imgui@1.0</code> | empty selector segment |
| <code>ocornut..imgui</code> | empty namespace segment |
| <code>ocornut.imgui@1.0:vulkan:extra</code> | more than one template delimiter |

<code>pkg:</code> 不再兼任列表语法。列举模板继续使用现有显式表面：

~~~text
mcpp new --list-templates ocornut.imgui@1.92.8
~~~

这样“省略 tname”永远表示选择 default，不会同时表示 list。

### 5.2 与 mcpp add / mcpp.toml 的一致性

模板不能只复制 <code>mcpp add</code> 的视觉风格，两个命令最终必须复用同一条规范化链：

~~~text
raw selector
  -> shared PackageSelector parser
  -> fill default namespace mcpplibs when omitted
  -> one normalized PackageId
  -> IndexRoute::lookup_descriptor by exact PackageId
  -> resolved PackageId + descriptor provenance
~~~

目标规则：

- <code>imgui</code>：<code>(mcpplibs, imgui)</code>。
- <code>acme.widget</code>：<code>(acme, widget)</code>。
- <code>mcpplibs.capi.lua</code>：<code>(mcpplibs.capi, lua)</code>。
- 多级 selector 总是以最后一个 segment 为 name，其余为 namespace。

对应的 mcpp.toml 语义：

~~~toml
[dependencies]
imgui = "1.92.8"             # default namespace mcpplibs

[dependencies.acme]
widget = "1.0.0"             # explicit namespace acme
~~~

CLI dotted form <code>acme.widget@1.0.0</code> 是第二种 TOML 形式的紧凑输入，不再表示“先猜 mcpplibs.acme，再猜 acme”。

当前 <code>mcpp add</code> 的 ordered dotted candidates 属于兼容实现。为了真正一致，迁移后 add/template 都使用上述单一规范化规则；旧项目清单已解析并锁定的 dependency 不被自动重写。

selector 是用户输入，最终身份仍是结构化：

~~~text
PackageSelector {
  namespace?: NamespacePath,
  name: NameAtom
}

PackageId {
  namespace: NamespacePath,
  name: NameAtom
}

ResolvedTemplatePackage {
  id: PackageId,
  version: ExactVersion,
  indexRoute,
  descriptorDigest,
  payloadDigest,
  root
}

TemplateSelection {
  package: ResolvedTemplatePackage,
  templateName: NameAtom
}
~~~

PackageSelector 规范化时填入默认 namespace，所以进入 resolver 后不再存在“namespace 未知”状态。xlings 安装 wire address 仍由 resolved PackageId 派生为 <code>namespace:name@version</code>。点号只属于 mcpp 用户 selector；冒号 wire address 不反向塞回 <code>--template</code>。

### 5.3 解析顺序

为了避免 namespace、version 与 tname 相互抢分隔符，解析固定为：

1. 验证最多一个 <code>:</code>，分离右侧 TemplateName。
2. 在左侧验证最多一个 <code>@</code>，分离 ExactVersion。
3. 将剩余部分完整交给共享 PackageSelector parser。
4. resolver 返回 PackageId 后再进入版本和模板选择。

禁止当前 scaffold 的“先按短名试空 namespace/compat，命中后再反推 namespace”旁路。

### 5.4 version 省略规则

- 写出 <code>@version</code> 时只接受 exact version；第一阶段不引入 range。
- 省略时选择目标平台可用的最新 stable version。
- prerelease 不自动成为默认；只有用户显式写出 exact prerelease 才选择。
- 解析成功后后续流程只携带 exact version。
- 成功输出、wire result 与生成的自依赖都展示 exact version。

版本选择必须复用包管理 resolver，不能由模板代码排序字符串。

### 5.5 tname 省略规则

省略 <code>:tname</code> 时：

1. 正好一个 template 声明 <code>default = true</code>：选择它。
2. 没有显式 default，但 package 只有一个 template：该单模板自动成为 default。
3. 没有显式 default，且存在多个 templates：失败并列出所有 template，要求用户写 <code>:tname</code> 或 provider 选出 default。
4. 多于一个显式 default：descriptor/index validation 失败。
5. package 没有 <code>templates/</code>：明确报告它不是 template provider。

单模板自动默认是当次已解析 provider/version 的确定事实；同一已锁版本不会因未来 release 新增模板而改变。新版本若增加第二个模板却未声明 default，省略 tname 会明确失败，不进行目录排序猜测。

### 5.6 生成与依赖注入

模板解析后必须保留完整 PackageId，解决当前 namespace 在 fetch 后丢失的问题。

RenderVars：

~~~text
project.name
project.namespace
project.qualifiedName
template.package.namespace
template.package.name
template.package.selector
template.package.version
template.name
~~~

自依赖注入复用 <code>mcpp add</code> 的 manifest editor：

- 不做字符串搜索。
- 使用用户风格的 dotted selector key。
- 写 exact resolved version。
- 保留 namespace 与 index provenance 到 lock/build resolution。
- 模板已经声明相同 PackageId 时不重复写入。
- 同 short name、不同 namespace 不视为同一个 dependency。

### 5.7 输出与诊断

成功的人类输出：

~~~text
Created app
Template ocornut.imgui@1.92.8:glfw-opengl3
Runtime mcpp-default
~~~

机器输出至少包含：

~~~json
{
  "packageSelector": "ocornut.imgui",
  "resolvedPackage": {
    "namespace": "ocornut",
    "name": "imgui",
    "version": "1.92.8"
  },
  "template": "glfw-opengl3",
  "runtimeSelection": "mcpp-default"
}
~~~

诊断规则：

- package 不存在：展示规范化后的 exact PackageId；namespace 省略时明确注明使用了默认 <code>mcpplibs</code>。
- version 不存在：列出该平台可用 stable versions。
- template 不存在：列出 provider 内 template names 和 default。
- 用户写 <code>ocornut:imgui</code> 时，按文法它表示 package <code>ocornut</code> 的 template <code>imgui</code>；若解析失败，诊断额外建议 namespace 风格 <code>ocornut.imgui</code>。
- 所有失败发生在创建目标目录前，或由临时目录事务回滚。

### 5.8 兼容迁移

旧的无 namespace 形式天然兼容：

~~~text
pkg
pkg@version
pkg:tname
pkg@version:tname
~~~

变化只有：

- 新增 dotted namespace selector。
- bare selector 明确填入默认 namespace <code>mcpplibs</code>。
- <code>mcpp add</code> 与 template 最终收敛为“dot 表示显式 namespace、无 dot 表示默认 mcpplibs”，不再各自猜候选。
- <code>pkg:</code> 的 legacy list 含义先 warning 一个 release train，之后错误；用户改用 <code>--list-templates pkg</code>。
- 不再新增 <code>--variant</code>，上一份综合设计中的该建议由本文覆盖。
- builtin <code>bin</code>/<code>gui</code> 可暂时保留 alias，但 package template 输出统一用 TemplateSpec。

当前 add 的 dotted selector 具有默认 namespace 前缀候选，例如 <code>capi.lua</code> 会先尝试 <code>mcpplibs.capi:lua</code>。新规则下：

~~~text
capi.lua            -> capi:lua
mcpplibs.capi.lua   -> mcpplibs.capi:lua
lua                 -> mcpplibs:lua
~~~

迁移要求：

1. 已有 mcpp.toml 与 lockfile 不自动改写，仍按其已记录身份工作。
2. 一个 release train 检测“旧候选结果与新 exact 结果不同”，warning 同时给出两种完整 selector。
3. 新 <code>mcpp add</code> 和 <code>mcpp new --template</code> 在迁移窗口后统一采用 exact dotted 规则。
4. mcpp-index 中所有 nested <code>mcpplibs.*</code> 文档示例改成完整 namespace，不能依赖隐式前缀。

## 6. mcpp 默认 runtime 与 mcpp.toml SubOS

### 6.1 两种模式

只定义两种项目运行时选择：

~~~text
McppDefault
NamedSubos(name)
~~~

不增加第三种“跟随当前全局 active subos”模式。

| mcpp.toml | 选择 |
|---|---|
| 没有 <code>[xlings].subos</code> | <code>McppDefault</code> |
| <code>[xlings] subos = "dev"</code> | <code>NamedSubos("dev")</code> |
| <code>[xlings] subos = "default"</code> | 显式选择 xlings home 中的 default SubOS |

### 6.2 默认模式

<code>McppDefault</code> 的定义：

- 使用 mcpp 所选择的 xlings provider。
- 使用 mcpp home 内已初始化、release 验证过的 default SubOS/RuntimeBinding。
- 不受用户另一个 shell 中执行 <code>xlings subos use</code> 的 active 状态影响。
- 缺失时走 mcpp bootstrap，不从任意目录挑第一个 glibc。
- xlings/runtime contract schema 不兼容时明确失败或升级，不静默回退宿主。

默认路径的用户体验是“安装 mcpp 后直接 build/run”，不要求用户了解 SubOS。

### 6.3 项目显式 SubOS

复用当前已支持的配置：

~~~toml
[xlings]
subos = "dev"
deps = ["cmake@3.28", "python@3.13"]

[xlings.workspace]
clang = "20.1.7"

[xlings.envs]
OPENBLAS_NUM_THREADS = "1"
~~~

语义：

- <code>subos</code> 只选择当前根项目 build/run/test 共用的本地命名环境。
- <code>deps</code>、workspace pins 与 envs 在该环境中物化。
- Linux 上 RuntimeBinding 同时决定 loader/libc。
- macOS/Windows 上仍选择一致的工具与环境契约，但不伪造 Linux glibc 语义。
- 指定 SubOS 不存在或无法回答 runtime contract 时 hard error，不退回 default。
- 同一台机器可同时保留 <code>el8</code>、<code>trixie</code>、<code>default</code> 等多个环境；每个项目按自己的 mcpp.toml 选择，互不切换全局 active 状态。

### 6.4 xlings binary/home 属于机器配置

项目清单必须可移植，所以不在 <code>mcpp.toml</code> 接受绝对 binary/home：

~~~toml
# ~/.mcpp/config.toml
[xlings]
binary = "bundled"  # or "system" / absolute administrator path
home = ""
~~~

职责分离：

- 全局 config 决定“使用哪一个 xlings 实例和 home”。
- 项目 <code>mcpp.toml</code> 决定“在该实例中使用 default 还是哪个 named SubOS”。

这样团队可以提交 <code>subos = "el8"</code>，而不提交某位开发者的 <code>/home/user/.xlings</code>。

### 6.5 选择优先级

稳定顺序：

~~~text
project [xlings].subos exists
  -> NamedSubos
otherwise
  -> McppDefault
~~~

本设计不增加临时 <code>--subos</code> CLI override。runtime 是 build identity，命令行临时覆盖会让同一份 <code>mcpp.toml</code> 产生不同 ABI，并污染缓存解释。

global xlings binary/home 只决定 provider，不插入第三个 runtime selection rung。

### 6.6 一个 snapshot 贯穿生命周期

~~~text
RuntimeSelection
  -> resolve exact SubOS
  -> read RuntimeBinding contract
  -> include contract hash in toolchain/build fingerprint
  -> configure/link
  -> post-link validation
  -> run/test environment
~~~

build、run、test、post-install fixup 不得分别重新猜 runtime。

建议内部模型：

~~~text
RuntimeSelection {
  mode: McppDefault | NamedSubos,
  subosName,
  source: DefaultPolicy | Manifest
}

RuntimeBinding {
  schema,
  providerId,
  platform,
  arch,
  contractHash,
  loader?,
  libc?,
  libraryDirs[],
  environment[],
  capabilities[],
  provenance
}
~~~

### 6.7 [runtime] 与 [xlings] 不混用

现有 <code>[runtime]</code> 表示程序/包启动时需要的 library dirs、dlopen libs 和 capabilities；它不是“选择哪一个 SubOS”的配置。

~~~toml
[xlings]
subos = "dev"                  # 选择 build/run 环境

[runtime]
capabilities = ["opengl"]      # 程序需要的通用 capability
~~~

选择环境属于 <code>[xlings]</code>，声明程序需求属于 <code>[runtime]</code>。禁止再增加 <code>[runtime] provider = "subos"</code> 形成第二入口。

### 6.8 SubOS 是本地、根项目级、非传递环境

SubOS 的心智模型是：

> mcpp 在一台机器上选择一个本地开发 OS 环境来 configure/build/run 当前项目。

它不是库依赖约束。规则如下：

1. 只有本次构建的 root manifest/workspace root 能选择 SubOS。
2. dependency manifest 中的 <code>[xlings].subos</code> 不合并、不继承、也不要求消费者创建同名 SubOS。
3. 一个库作为独立根项目开发时，它自己的 <code>[xlings].subos</code> 生效；同一源码作为另一个项目的 dependency 时，使用消费者 root 选择的环境构建。
4. workspace 整体构建时由 workspace root 选择一个环境；member 的 SubOS 不覆盖 root。member 独立构建时才成为自己的 root。
5. SubOS name 不写入 dependency requirement，不从 lockfile 向下游传播，也不成为 mcpp-index package identity 的一部分。

典型源码分发：

~~~text
application root selects subos=el8
  -> source dependency A builds inside el8
  -> source dependency B builds inside el8
  -> application runs inside el8

another application selects subos=trixie
  -> the same A/B sources rebuild under trixie
~~~

因此可以在同一机器上用不同 glibc 环境开发同一组源码依赖，而不是要求所有库声明或传递 <code>glibc=2.x</code>。

间接关系只来自实际产物：

- RuntimeBinding 必须进入当前项目 build fingerprint，防止跨 SubOS 复用 object/BMI。
- 如果发布的是预构建 binary，发布流程应从最终 artifact 派生 ABI、loader、GLIBC symbol floor 等兼容元数据。
- 这些是 artifact 的客观兼容属性，不是把开发时的 SubOS name 传播给消费者。
- package 确实需要某项运行时能力时，通过 mcpp-index/xim package requirement 表达，不通过 <code>[xlings].subos</code> 表达。

### 6.9 当前 active SubOS 的迁移

当前未显式配置时可能读取 active SubOS。切换到稳定 default 的迁移：

1. 一个 release train 输出 warning，显示当前 active 与未来 default 是否不同。
2. 提供可复制配置：<code>[xlings] subos = "current-name"</code>。
3. 下一 release 将 absence 固定为 <code>McppDefault</code>。
4. 不自动修改用户 <code>mcpp.toml</code>。

冷 HOME、新安装可以直接采用新规则，不需要 legacy 过渡。

## 7. OpenGL/Vulkan 图形栈职责

### 7.1 强制边界

| 层 | 拥有 | 不拥有 |
|---|---|---|
| mcpp | 通用 PackageId、source/build graph、LinkIntent、RuntimeBinding 消费、产物验证 | OpenGL/Vulkan 包名、GPU vendor、ICD 选择、WSL 探测 |
| mcpp-index | ImGui/GLFW/OpenGL/Vulkan C++ 包、features、templates、平台 build/link 声明，以及这些包对 xlings 图形能力的依赖关系 | 安装宿主驱动、判断 NVIDIA/WSL 当前状态 |
| xlings | 图形栈 orchestration、依赖解析/安装/激活、SubOS、runtime contract、provider/sentinel 生命周期 | C++ GUI template 与项目源码 |
| xim-pkgindex | xlings 消费的 Mesa、Vulkan loader/ICD、NVIDIA/WSL sentinel recipe 与 provenance schema | 运行时自行做全局选择、mcpp 工程生成和 source graph |

mcpp 源码中不新增以 <code>opengl</code>、<code>vulkan</code>、<code>mesa</code>、<code>nvidia</code>、<code>wsl</code> 为条件的 planner 分支。

### 7.2 数据流

~~~text
mcpp.toml dependency
  -> mcpp resolves descriptor through mcpp-index
  -> mcpp-index descriptor contributes sources, features, LinkIntent
     and xlings graphics package/capability dependencies
  -> xlings resolves and materializes the dependency closure
  -> xlings activates providers/sentinels described by xim-pkgindex
  -> xlings exports RuntimeArtifacts and provenance
  -> mcpp consumes only generic resolved contract
  -> link, validate and run
~~~

mcpp 不直接选择 <code>xim:graphics</code>。mcpp-index 的平台 package contract 声明 xlings 生态依赖；xlings 才是图形栈运行时 owner，负责从 xim-pkgindex recipe 中选择和物化具体 provider。

### 7.3 通用构建接口

mcpp-index 输出：

~~~text
LinkIntent {
  libraries[],
  linkLibraryDirs[],
  transitiveNeededDirs[],
  runtimeSearchDirs[],
  frameworks[],
  deployFiles[]
}

RuntimeRequirement {
  kind: soname | capability | icd_manifest | display | host_service,
  value,
  phase: link | run,
  requester: PackageId,
  required
}
~~~

xlings 解析 xim-pkgindex provider recipe 后提供：

~~~text
RuntimeArtifact {
  role: loader | library | driver | manifest | host_bridge,
  provider: PackageId,
  path,
  provenance: payload | subos_view | host_link | system_sdk,
  abi,
  digest?,
  hostFingerprint?
}
~~~

mcpp 只检查这些通用结构是否完整、目标平台是否匹配、最终 artifact 是否满足 loader/ABI 物理约束。

### 7.4 OpenGL 收口

mcpp-index：

- <code>ocornut.imgui</code> 声明 core 与 backend features。
- <code>glfw-opengl3</code> template/feature 引入 GLFW、OpenGL headers 与通用 runtime requirement。
- Linux package contract 依赖由 xim 提供的 graphics capability。
- macOS contract 使用 native frameworks。
- Windows contract 使用 Win32/system SDK 与声明的 runtime DLL。

xlings（基于 xim-pkgindex recipes）：

- Mesa/GLVND 与软件/硬件 driver closure。
- NVIDIA/WSL host-link sentinel。
- runtime dirs、实际 libraries 和 provenance 的解析/导出。
- sentinel 的 applicable/not-applicable/inconclusive 生命周期。

目标状态是移除 mcpp-index 中复制 SubOS view library 的长期 symlink bridge；runtime artifact 通过 contract 传递。

### 7.5 Vulkan 收口

mcpp-index：

- Vulkan headers、loader-facing API、backend sources 和 templates。
- <code>vulkan</code> template 声明 loader、ICD manifest、display/surface requirements。
- 不从 <code>/usr/lib*</code> 自行收集宿主 ICD/DSOs。

xlings（基于 xim-pkgindex recipes）：

- Vulkan loader、ICD manifests 与对应 driver。
- Mesa RADV/Intel 等 payload provider。
- NVIDIA/WSL host-link provider。
- macOS MoltenVK、Windows system/provider 策略由平台包契约表达。

验证不能止于 loader symbol：

1. create instance。
2. enumerate physical devices。
3. 记录实际 ICD manifest 与 driver provider。
4. 有窗口 lane 时创建 surface。
5. 无硬件环境明确报告 NOT_EXERCISED，不能汇总为 GPU pass。

### 7.6 ImGui feature/template 形态

建议 mcpp-index 将图形入口拆成：

~~~text
core
headless
backend-glfw-opengl3
backend-vulkan
app
docking
viewports
~~~

示例：

~~~text
mcpp new app --template ocornut.imgui:glfw-opengl3
mcpp new app --template ocornut.imgui@1.92.8:vulkan
mcpp new app --template ocornut.imgui@1.92.8:docking
~~~

- core/headless 不应无条件拉入 GLFW/OpenGL。
- app 可以组合默认 backend，但 resolved dependency/features 必须写入生成清单。
- docking/viewports 与 renderer backend 正交。

### 7.7 可观察性

<code>mcpp why runtime</code> 可以展示 xlings 已解析完成的结果：

~~~text
requirement
  -> selected canonical provider
  -> runtime artifact
  -> provenance
  -> ABI/loader verdict
~~~

它是通用 contract 的解释器，不进行 GPU 探测。GPU/driver 诊断与重探测由 xlings 统一入口及其 xim provider/sentinel 暴露，mcpp 只给出跳转提示。

图形依赖和 SubOS 选择是正交的：

- mcpp-index/xim 依赖描述“当前项目需要哪些图形能力”。
- root <code>[xlings].subos</code> 描述“当前项目在哪个本地环境 build/run”。
- library dependency 不通过自己的 SubOS 要求消费者环境；源码会在消费者已选环境中构建。
- 某个图形 provider 对实际 ABI/glibc 的要求，由 xlings 在当前环境解析并验证，而不是把 provider 的 SubOS name 传播出去。

## 8. AUR 只自动维护 mcpp-bin

### 8.1 包策略

| AUR 包 | 策略 |
|---|---|
| <code>mcpp-bin</code> | 自动生成、验证、对账和漂移告警；不阻塞 GitHub Release |
| <code>mcpp-m</code> | 本阶段不更新；不修改 package 文件或 AUR remote，只从 mcpp-bin 自动对账路径隔离 |
| <code>mcpp-git</code> | 不进入 release workflow；如要发布，单独认领与设计 |

用户主路径是安装 release 预构建产物，因此自动化可靠性集中投入 <code>mcpp-bin</code>。

### 8.2 当前事实

2026-08-09 的审计快照：

- GitHub latest stable：<code>v2026.8.8.4</code>。
- AUR <code>mcpp-bin</code>：<code>2026.8.1.1-1</code>。
- 连续 push 失败的直接响应是 AUR maintenance。
- workflow 缺少 retry、schedule 与状态对账，所以临时故障变成长时间漂移。
- <code>mcpp-m</code> 存在已知独立问题，但本文不处理、不推送，也不让它成为 mcpp-bin reconciler 的前置条件或后置条件。

旧失败 run 不应直接 rerun，因为它从旧 release commit 执行旧生成逻辑。

### 8.3 单一 desired state

release 产出不可变 manifest：

~~~json
{
  "schema": 1,
  "version": "2026.8.8.4",
  "tag": "v2026.8.8.4",
  "commit": "<release-commit>",
  "assets": [
    {
      "platform": "linux",
      "arch": "x86_64",
      "name": "mcpp-2026.8.8.4-linux-x86_64.tar.gz",
      "sha256": "<x86-64-sha256>"
    },
    {
      "platform": "linux",
      "arch": "aarch64",
      "name": "mcpp-2026.8.8.4-linux-aarch64.tar.gz",
      "sha256": "<aarch64-sha256>"
    }
  ]
}
~~~

reconciler 只消费 latest complete、非 draft、非 prerelease manifest。

### 8.4 mcpp-bin reconciler

触发：

- successful release workflow_run：低延迟。
- schedule：建议每 6 小时。
- workflow_dispatch：人工恢复，默认仍指向 latest stable。

流程：

~~~text
read latest stable release manifest
  -> query AUR mcpp-bin version and remote git head
  -> compare with Arch vercmp
  -> download both Linux assets
  -> recompute sha256 and compare manifest/sidecar
  -> generate PKGBUILD
  -> generate .SRCINFO from PKGBUILD in Arch container
  -> makepkg --verifysource
  -> dry-run diff
  -> fast-forward push with bounded retry
  -> verify AUR git head
  -> bounded poll AUR RPC
  -> clean Arch install and mcpp --version smoke
~~~

### 8.5 幂等和失败分类

| 状态 | 行为 |
|---|---|
| desired == current 且内容一致 | success no-op |
| desired > current | 生成、验证、push |
| desired < current | 默认拒绝降级 |
| asset/hash 缺失 | push 前 permanent failure |
| AUR maintenance/timeout | transient；指数退避，后续 schedule 补偿 |
| SSH auth/metadata invalid | permanent；立即告警 |
| git 已更新但 RPC 延迟 | poll，不重复 commit |

已知 package clone 失败不能自动当作首次发布。禁止 force-push AUR 历史。

### 8.6 mcpp-m 完全不动的边界

本文对 <code>mcpp-m</code> 的要求只有隔离，不包含任何维护动作：

- 不修改 <code>scripts/aur/mcpp-m/**</code>。
- 不生成或推送 mcpp-m AUR commit。
- 不改变其版本、checksum、maintainer、deprecated 状态或现有远端历史。
- 不宣称它与 GitHub Release 同步。
- mcpp-bin reconciler 的成功/失败只由 mcpp-bin desired/observed state 决定。

实现 mcpp-bin reconciler 时应建立独立 job/workflow；现有 combined workflow 唯一允许涉及 mcpp-m 的变化是停止自动调用其 publish leg，以保证“不推送 mcpp-m”。不得读取、生成或改写 mcpp-m 内容。

### 8.7 AUR SLO

建议初始目标：

- AUR 可用时，release 后 30 分钟内 mcpp-bin 收敛。
- event path 失败后，6 小时 schedule 再尝试。
- 超过 24 小时仍漂移，自动更新固定告警 issue 或发高优先级通知。

## 9. 四项设计如何协同

### 9.1 默认用户

~~~text
install mcpp-bin
  -> bundled xlings/bootstrap default runtime
  -> mcpp new app --template ocornut.imgui:glfw-opengl3
  -> mcpp-index supplies template/build contract
  -> xlings/xim supplies graphics runtime
  -> mcpp build
  -> mcpp run using the same default RuntimeBinding
~~~

用户不需要手工选择 GPU provider，也不需要配置 SubOS。

### 9.2 固定运行时用户

~~~toml
[xlings]
subos = "el8"
~~~

~~~text
mcpp new/build/run
  -> same template/package selector
  -> same mcpp-index graph
  -> named SubOS RuntimeBinding
  -> cache fingerprint changes
  -> artifact targets that runtime
~~~

### 9.3 平台差异

| 平台 | mcpp default runtime | graphics provider |
|---|---|---|
| Linux x86_64 | mcpp-managed default SubOS/runtime binding | xim Mesa or explicit host sentinel |
| Linux aarch64 | mcpp-managed native runtime | capability must fail clearly until graphics recipe supports it |
| macOS arm64/x86_64 | mcpp-managed tool/SDK environment | native frameworks/MoltenVK package contract |
| Windows x64 | mcpp-managed tool/runtime environment | Win32/system DLL or declared Vulkan provider |

跨平台只共享抽象契约，不假装共享底层文件布局。

## 10. 迁移阶段

### Stage 0 — 文法与边界冻结

- 冻结 TemplateSpec grammar：namespace/version/tname 均可省略，namespace 默认 <code>mcpplibs</code>。
- 冻结“单模板在没有显式 default 时自动成为 default”。
- 冻结 absence → McppDefault、<code>[xlings].subos</code> → NamedSubos。
- 冻结 SubOS 是 root-local、非传递 build/run 环境。
- 冻结 xlings 是 graphics runtime owner，mcpp 只有 generic contract knowledge。
- 冻结 AUR managed set = <code>{mcpp-bin}</code>。
- 冻结 mcpp-m package/AUR remote 不动。

### Stage 1 — 模板统一

- 提取共享 PackageSelector parser/resolver。
- 将 add/template 统一为 bare → mcpplibs、dotted → exact namespace。
- scaffold 保存完整 PackageId/provenance。
- manifest 注入复用 add editor。
- dotted namespace/template E2E。
- <code>pkg:</code> list deprecation。

### Stage 2 — runtime 选择统一

- 引入 RuntimeSelection。
- default 与 named SubOS 生成同一 RuntimeBinding snapshot。
- build/run/test 共用 snapshot 和 contract hash。
- 只从 root/workspace root 读取 SubOS；dependency/member 不向消费者传播。
- active SubOS legacy warning。

### Stage 3 — graphics 收口

- mcpp-index 调整 ImGui/OpenGL/Vulkan features/templates 及 xlings 生态依赖。
- xlings 基于 xim-pkgindex recipes 收口 GL/Vulkan runtime artifacts 和 sentinels。
- 移除 host ICD farm 与 SubOS view symlink bridge。
- mcpp 保持 generic planner/validator。

### Stage 4 — mcpp-bin 对账

- release manifest。
- 单包 generator、dry-run 与 Arch validation。
- event + schedule reconciler。
- 从旧状态恢复到 latest stable，并验证真实安装。

这些 stage 是设计级依赖顺序，不是 implementation plan；每个 stage 仍需在设计获批后拆成独立可评审 PR。

## 11. 验收矩阵

### 11.1 模板

| 场景 | 期望 |
|---|---|
| <code>pkg</code> | <code>mcpplibs:pkg</code> + latest stable + default/single template |
| <code>pkg@1.2.0</code> | default namespace + exact version + default/single template |
| <code>ns.pkg:t</code> | exact namespace <code>ns</code> + named template |
| <code>ns.pkg@1.2.0:t</code> | 完整解析，输出 exact PackageId/version/template |
| <code>capi.lua</code> / <code>mcpplibs.capi.lua</code> | 分别解析为 <code>capi:lua</code> / <code>mcpplibs.capi:lua</code> |
| add/template 相同 selector | 规范化为同一个 PackageId |
| 同 short name 不同 namespace | 不按 short name 注入/缓存碰撞 |
| 单 template、无显式 default | 自动选择该 template |
| 多 templates、无 default | 失败并列出 templates |
| 多 default | provider validation 失败 |
| prerelease only 且省略版本 | 失败并要求显式 version |
| 失败渲染/I/O | 目标目录不存在或事务回滚 |

### 11.2 runtime

| 场景 | 期望 |
|---|---|
| fresh HOME，无 <code>[xlings]</code> | bootstrap mcpp default，build/run 同 binding |
| named SubOS 存在 | build/run/test 使用该 contract |
| named SubOS 不存在 | hard error，不退回 default |
| active SubOS 与 default 不同 | 迁移期 warning；最终仍选择 default |
| 切换 <code>subos</code> | build fingerprint 改变，不复用旧 ABI objects |
| dependency 声明自己的 SubOS | 作为 dependency 时不传递、不覆盖 root 环境 |
| 同一源码在 el8/trixie 两个 root 中构建 | 各自在自己的 RuntimeBinding 下生成独立缓存/产物 |
| 预构建库 | 传递实际 artifact ABI requirement，不传递 SubOS name |
| Linux loader/libc 混源 | post-link 失败或严格迁移阶段明确 warning |
| macOS/Windows | 不执行 Linux glibc 规则 |

### 11.3 graphics

| Gate | 必须证明 |
|---|---|
| mcpp source ownership | 无 GPU vendor/ICD selection branch，只消费 xlings generic contract |
| xlings ownership | 根据 mcpp-index 依赖解析 xim recipes、provider/sentinel 与 provenance |
| mcpp-index static | OpenGL/Vulkan features、templates、platform dependencies 可解析 |
| Linux software GL | Xvfb/Wayland headless + llvmpipe 创建窗口/帧 |
| Linux native GL | AMD/Intel/NVIDIA/WSL 分别记录实际 provider |
| Vulkan | instance、physical device、ICD manifest/driver provenance |
| macOS | native framework/MoltenVK 路径 |
| Windows | Win32 backend 与 Vulkan provider 路径 |
| unsupported arch | 明确 capability error，不以 SKIP 计 pass |

### 11.4 AUR

| 场景 | 期望 |
|---|---|
| 新 stable release | mcpp-bin 在 SLO 内收敛 |
| 重复事件 | no-op，无新 commit |
| 迟到旧事件 | 不降级 |
| 两架构任一资产/hash 错 | push 前失败 |
| AUR maintenance | transient retry + schedule 补偿 |
| mcpp-m | package 文件和 AUR remote 均不变，不参与 mcpp-bin verdict |
| push 后 | git head、RPC version、clean install 全部验证 |

## 12. 性能与简洁性约束

- TemplateSpec parse 为 O(length)，不访问网络。
- resolver 与 <code>mcpp add</code> 共用缓存与 IndexRoute，不增加第二轮全索引扫描。
- RuntimeBinding 每次 configure 解析一次，contract hash 进入 snapshot；hot no-op 不启动 xlings。
- mcpp build 热路径不探测 GPU、不执行 Vulkan/OpenGL 工具。
- graphics provider 探测发生在 xim install/doctor 生命周期，并缓存 host fingerprint。
- AUR schedule 在 desired == current 时只做轻量查询与一致性检查。
- 不因这四项新增顶层 CLI 命令。

## 13. 错误与安全边界

- selector、version、template name 任一非法，在网络/install/目录创建前失败。
- package descriptor 命中后必须校验声明的完整身份。
- 模板为纯数据，不执行 provider hooks/scripts。
- 生成在 sibling temp dir 完成，验证后原子 rename。
- manifest 不接收项目级 absolute xlings binary/home。
- named SubOS 不存在时不 fallback。
- mcpp 不读取 host GPU library 目录来补齐索引缺口。
- AUR 先验证全部 mcpp-bin assets，再加载 SSH secret/push。
- AUR 只 fast-forward，不改写历史。

## 14. 第二轮 review 后已冻结的细节

1. namespace 可以省略；省略固定为 <code>mcpplibs</code>，写出 dotted namespace 时按 exact namespace 解释。
2. 只有一个 template 且没有 <code>default = true</code> 时，该单模板自动成为 default。
3. SubOS 暂时只允许通过 root/workspace-root <code>mcpp.toml</code> 选择，不增加 CLI override。
4. SubOS 是本地 build/run 开发环境，不作为库的传递要求；不同项目可选择不同 SubOS/glibc。
5. xlings 是 graphics stack/runtime owner；mcpp-index 声明 C++ 图形包到 xlings 生态的依赖，mcpp 只做 generic build。
6. AUR 自动对账只面向 mcpp-bin；mcpp-m package 与 AUR remote 本阶段不动。

## 15. 与上一份综合设计的关系

本文仅在以下四处覆盖上位文档：

| 上位文档建议 | 本文最终方向 |
|---|---|
| <code>--template namespace:name@version --variant name</code> | <code>--template [ns.]name[@version][:tname]</code>；默认 ns=mcpplibs |
| mcpp 参与更宽的 graphics runtime 规划 | xlings 负责 graphics runtime；mcpp-index 声明依赖；mcpp 只消费通用契约 |
| runtime contract 为大范围跨仓主线 | McppDefault + existing <code>[xlings].subos</code>，且 SubOS root-local/non-transitive |
| mcpp-bin/mcpp-m 都进入自动 reconciler | 自动 reconciler 只管理 mcpp-bin；mcpp-m 不动 |

未被本文覆盖的 C1–C9、artifact physics、identity 类型化与机器输出分析继续保留在上位文档中。

## 16. 当前实现证据锚点

| 主题 | 证据 |
|---|---|
| mcpp add dotted/colon selector | <code>tests/e2e/12_add_command.sh:82-106</code>；<code>src/pm/commands.cppm:75-143</code> |
| shared dotted candidate rules | <code>src/pm/dependency_selector.cppm:78-120</code> |
| package identity spec | <code>docs/spec/package-identity.md:150-169</code> |
| current TemplateSpec | <code>src/scaffold/template.cppm:22-47</code> |
| scaffold short-name loss | <code>src/scaffold/create.cppm:30-129</code> |
| template E2E | <code>tests/e2e/69_package_templates.sh:94-194</code> |
| existing [xlings].subos | <code>src/manifest/types.cppm:469-485</code>；<code>src/manifest/toml.cppm:988-996</code> |
| root project environment materialization | <code>src/build/prepare.cppm:1997-2009</code> |
| current runtime resolution | <code>src/build/prepare.cppm:928-974</code> |
| user documentation | <code>docs/05-mcpp-toml.md:946-978</code> |
| AUR workflow | <code>.github/workflows/aur-publish.yml:14-102</code> |

本文是待 review 设计，不应据此宣称上述行为已落地或关闭相关 issue。
