---
subject: release
status: active
---

# 发布的发布面与 macOS 链接的 SDK 契约（2026-11-14 补充）

**日期：** 2026-11-14
**状态：** 第 1 节（B1–B4）与第 2 节的 M1 在 mcpp 2026.11.14.1 实施；M2、M3 在 openkal-macos 的补丁版实施，待负责人审阅。

本文补充 `2026-11-14-ci-and-release-time-cost-design.md`，处理 v2026.11.13.1 发布中测到的两件事：一是 `publish-ecosystem`
的耗时与它对 xim-pkgindex 自动更新器的依赖；二是 canary `cxx aarch64-macos test` 揭示的 mcpp 与 openkal-macos 之间关于
macOS SDK 的契约矛盾。两件事互不依赖，各自一个 PR（mcpp 一个，openkal-macos 一个）。

## 1. 发布面：GitCode 同步默认关闭，索引 PR 直接创建

### 1.1 测量（v2026.11.13.1，运行 43594022005）

| 环节 | 用时 |
|---|---|
| 四个构建并行，至 `create-release` | 40 分钟 |
| 封存（`release-manifest`） | 约 1 分钟 |
| GitHub 镜像（`xlings-res/mcpp`，8 个资源） | 64 s，每个资源 1 s |
| GitCode 镜像（同 8 个资源） | 713 s；四个归档各约 604 s（10–17 MB），`.sha256` 各 4–7 s |
| 索引 PR（`bump_index.sh`） | 向 xim-pkgindex 发 `repository_dispatch`，等待其更新器开出 PR，上限 1200 s |

GitCode 一侧的瓶颈是 GitHub 美国 runner 到 `file.gitcode.com` 的上行（`mirror_res.sh` 的测量：0.012 MB/s；中国大陆主机为
1.84 MB/s）。同一批资源由负责人在本地以 gtc 上传、取回逐字节比对，用时以分钟计。

`bump_index.sh` 不写配方，它请求 xim-pkgindex 的更新器（`xpkgindex-update.yml`）提出 PR；该更新器要求 GLOBAL 与 CN 两处
资源都已存在才提出。因此索引 PR 被串在 GitCode 镜像之后，GitCode 慢或失败时索引 PR 也不出现。

### 1.2 方案

**B1. GitCode 镜像是一项可选择的步骤，默认不运行。**

- `release.yml` 的 `workflow_dispatch` 增加布尔输入 `gitcode`（缺省 `false`）。`publish-ecosystem` 的镜像分为两步：
  GitHub 一步以 `MIRROR_HOSTS=gh` 调用 `mirror_res.sh`，总是运行；GitCode 一步以 `MIRROR_HOSTS=gtc` 调用，只在
  `WITH_GITCODE`（`github.event_name == 'workflow_dispatch' && inputs.gitcode`）为真时运行。tag 推送没有输入，
  只镜像 GitHub。`mirror_res.sh` 已支持 `MIRROR_HOSTS=both|gh|gtc`，脚本本身不改。
- 步骤的 `timeout-minutes`：GitHub 一步 10 分钟，GitCode 一步 45 分钟（脚本自身的 GitCode 期限为 2400 s）。
- 本地补齐 GitCode 的做法写入 `docs/92-release.md` §2：`MIRROR_HOSTS=gtc bash .github/tools/mirror_res.sh mcpp <version>`
  （需要 `GITCODE_TOKEN` 与 PATH 上的 gtc），随后逐个资源从 GitCode 取回并与 GitHub 的字节比对。检查清单中"两处镜像都提供
  四个平台"一项保留，执行者由 CI 变为负责人。

**B2. 索引 PR 由发布直接创建，不经 xim-pkgindex 的更新器。**

- 新脚本 `.github/tools/index_pr.py --manifest <mcpp-release.json> --gitcode pending|absent`（替换 `bump_index.sh`，
  后者删除）：
  1. 读本次运行封存的 `mcpp-release.json`（`release-manifest` 作业上传的 artifact `mcpp-release-manifest`），取版本与四个
     平台归档的 sha256；不重新下载、不重新计算，索引写入的摘要即封存的摘要。tag 重建时版本亦取自清单而非 `mcpp.toml`。
  2. 在 xim-pkgindex `main` 的 `pkgs/m/mcpp.lua` 中，向 `linux`、`macosx`、`windows` 三张表各追加该版本的条目（GLOBAL 为
     `github.com/xlings-res/mcpp/releases/download/<v>/…`，CN 为 `gitcode.com/xlings-res/mcpp/releases/download/<v>/…`），
     并把三处 `latest` 指向它。条目的形状与更新器写出的相同（见 xim-pkgindex#1149）。
  3. 以 `XIM_PKGINDEX_TOKEN`（Contents 与 Pull requests 权限；`gh auth setup-git` 供推送认证）推送分支
     `auto/bump-xim-mcpp`，开 PR。PR 正文以更新器的标记
     `<!-- xpkgindex-update: {"id":"xim:mcpp","candidate":"<v>"} -->` 开头：更新器按该标记识别已有提案，不再重复提出。
     同一分支上若有较早版本的打开 PR，改写其标题与正文，不另开。
  4. PR 正文写明 GitCode 一侧的状态：`pending`（本次运行随后镜像 GitCode，作业绿后合入）或 `absent`（未由本次运行镜像，
     以 `MIRROR_HOSTS=gtc` 补齐后合入）。
- 已存在同一标记的 PR（打开或已合入）时脚本不做任何事；配方已含该版本时同样不做任何事。重复运行无副作用。
- 改写的是配方的版本表，不改配方逻辑。由此配方多了一个写入者（mcpp 的发布），这是有意的取舍：发布不再等待另一仓库的
  调度，代价是条目形状须与更新器一致，由 xim-pkgindex 自身的 CI（配方解析、L3/L4 安装检查）把关。更新器继续服务其余包。

**B3. 次序与时长。** `publish-ecosystem` 变为：GitHub 镜像（约 1 分钟）→ 索引 PR（秒级）→ 可选的 GitCode 镜像（放在最后，
失败不影响前两步的结果，但仍使作业为红）。发布从分派到索引 PR 预计约 42 分钟。负责人在本地补齐 GitCode 之后合入索引 PR，
随后 `ci-fresh-install` 与真实安装验证照旧。

**B4. 验证。** `tests/scripts/test_index_pr.py` 离线运行：以固定的 `mcpp.lua` 片段与封存清单为输入，断言追加的三处条目、
`latest` 的移动、标记行与幂等；并读 `release.yml`，断言 `gitcode` 输入缺省为假、三步的次序、只有 GitCode 一步调用 gtc
且受 `WITH_GITCODE` 约束、索引一步读封存清单。另以 v2026.11.13.1 的真实清单对 #1155 之前的 `mcpp.lua` 执行
`--edit`，结果与手工合入的 xim-pkgindex#1155 逐字节相同。

## 2. macOS 链接的 SDK：mcpp 与 openkal-macos 的契约

### 2.1 现象

canary `cxx aarch64-macos test`（macOS runner，`mcpp test --target aarch64-macos`）自加入起 9 次运行全部链接失败：
`ld64.lld: error: undefined symbol: strlen`（以及 `memcpy`、`pthread_create` 等）。测试程序经 mtest → mcpplibs.os 依赖
openkal-macos；同一行的 build、run 两格不依赖 openkal-macos，通过。

### 2.2 原因：两处陈述自 2026-08-25 起矛盾

| 时间 | 变化 |
|---|---|
| 2026-06-24（mcpp#162） | macOS 的每个链接带 `-isysroot <SDK>`，不依赖主机开发工具的状态 |
| 2026-08-24（mcpp#486） | `build_program.cppm` 对构建程序陈述：Mac 上 `--target aarch64-macos` 的目标侧由图供给，mcpp 不把系统 SDK 放上链接。openkal-macos 据此在 `MCPP_TARGET_REQUESTED` 非空时把 `port/`（只列三个名字的 `libSystem.tbd`）加入链接搜索路径 |
| 2026-08-25（d35c0436） | 图供给 C 库时 `-isysroot` 同样保留（`flags.cppm` "AND KEPT"）。上一行的陈述自此不成立 |

`-L port` 先于 SDK 被搜索，三个名字的存根遮蔽了 SDK 的完整 `libSystem`。缺陷不由 2026.11.13.1 引入，自 2026.8.25.2 起
潜伏：mcpp 自己的 macOS 作业是原生构建（不写 `--target`，openkal-macos 不加存根），canary 是第一个在 Mac 上以 `--target`
构建依赖 openkal-macos 的程序的检查。

影响面：在 Mac 上以 `--target aarch64-macos`、`--target x86_64-macos`，或以含 macOS 成员的目标集合，构建任何依赖
openkal-macos 的项目（mcpplibs.os、mtest 的使用者）。

### 2.3 方案

openkal-macos 要回答的问题是"链接中有没有系统自己的 `libSystem`"，而不是"是否指定了目标"。这个事实由 mcpp 决定，
因此由 mcpp 陈述。

**M1（mcpp）：构建程序得到链接所用的 SDK。**
- 新变量 `MCPP_TARGET_SDK`：链接以 `-isysroot` 指名的 SDK 目录；链接不指名 SDK 时为空。与其余变量一样总是给出，空即不适用。
- 取值与 `flags.cppm` 的链接参数出自同一个函数：把 `flags.cppm` 中选择 `macos_sdk` 的三支（Apple 交叉行的 SDK、以
  `AppleIsysroot` 陈述的基座、`apple::sdk_path()`）提为 `mcpp::toolchain::apple::link_sdk(...)`，链接参数与构建程序的环境
  各读一次，二者不会再分歧。
- `hostprogram.cppm` 增加访问器 `target_sdk()`；`build_program.cppm` 中关于"`--target` 时不放 SDK"的陈述改为现行行为。
- 变量进入构建程序的重跑键（`contract_hash`），SDK 改变时构建程序重新运行。
- 文档：`docs/30-build-mcpp.md` 的变量表（中英）。测试：单元测试断言 macOS 链接参数中的 `-isysroot` 与 `MCPP_TARGET_SDK`
  相同、非 Apple 目标为空。

**M2（openkal-macos）：存根只在链接中没有系统的存根时提供。**
- `build.mcpp` 的条件改为：指定了目标（`MCPP_TARGET_REQUESTED` 非空），且 `MCPP_TARGET_SDK` 为空。典型情形是从 Linux 交叉
  到 macOS。原生构建、Mac 上的 `--target` 由 SDK 回答，不再加存根。
- 兼容：不认识 `MCPP_TARGET_SDK` 的旧 mcpp 不给出它（读为空），条件退化为现行条件，行为不变。新 mcpp 配旧 openkal-macos
  的组合仍是现行行为。
- 文件头的说明随之改写：判断依据是链接中是否已有系统的存根，由 mcpp 陈述。
- 以补丁版发布（0.23.x 的下一个）。mcpplibs.os 的依赖范围为 `>=0.22.0, <0.24.0`，使用者解析时自动取到，不需要其余包改动。

**M3（验证）。**
- openkal-macos 的 CI：macOS 上以 `--target aarch64-macos` 构建并运行其一致性测试（此前只有原生构建）；Linux 上交叉到
  `aarch64-macos` 的链接仍取存根并成功。
- mcpp 的 canary：`cxx aarch64-macos test` 在新的 openkal-macos 发布后通过，canary 报告该格与矩阵不符，矩阵改回 `ok`。

### 2.4 不采纳

- **mcpp 把 SDK 的库目录排在包的 `link_search` 之前。** 这改变 `-L` 的一般语义（包陈述的搜索路径先于系统），为一个包的
  问题改所有包的规则。
- **openkal-macos 以主机系统判断。** 其文件头记录了以主机判断的失败（2026-08-23）：判断应依据链接的事实，而该事实只有
  mcpp 知道。
- **openkal-macos 自行探测 SDK（`xcrun --show-sdk-path`）。** 它重复 mcpp 的判断，且与 mcpp 实际所用的 SDK（`SDKROOT`、
  基座陈述）可能不同。
