# macOS 首跑遗留问题(已记录,待处理)

**日期**: 2026-06-24
**已修复(单独 PR)**:
- `-lSystem` 链接失败 → 显式 `-isysroot` + 加固 `sdk_path()`(PR #162,
  doc `2026-06-24-macos-link-lsystem-sdk.md`)。
- 首跑需回车(stdin hang)→ POSIX 也 seal stdin(PR #163)。

下面两个**待处理**:

## A. ninja bootstrap ~145s(task #31)

`--verbose`(WS5 计时)实测:
```
[VERBOSE 09:00:49.470] init: bootstrap ninja@1.12.1: start
[VERBOSE 09:03:15.090] init: bootstrap ninja@1.12.1: done (Δ=145618ms)
```
单这一步 ~2.4 分钟,是 macOS 首跑「卡住」的主因。

**未知**:是下载慢、连接握手慢、还是解压慢——当前计时只到「整步」粒度。
**下一步**:落实 WS5 §4.5——把 bootstrap 拆成 connect / download / extract 子计时
(`ScopedTimer.mark()`),再判断:
- 若卡在 download:换镜像 / 并行 / 复用已装 ninja(很多 mac 有 brew ninja);
- 若卡在 extract:换解压实现 / 校验包大小。
**待用户补充**:第二次冷跑是否仍 ~145s?到 github/gitcode 的网络如何?

## B. `xlings update` 子索引 artifact 404(task #33,**xlings 侧**)

CN(mirror=CN)下 `xlings update`:主索引 OK,**子索引 scode/d2x/awesome 取 artifact 失败**:
```
candidate failed (https://gitcode.com/xlings-res/xim-index/releases/download/latest/xim-index-scode-0.4.58.tar.gz): HTTP 404
candidate failed (https://github.com/.../xim-index-scode-0.4.58.tar.gz): Connection failed   # 用户网络到 github 差
→ 回退 git clone via ghproxy(慢但成功)
```

**已查明**:
- GitHub `xlings-res/xim-index` `latest` release **有**这些 artifact(scode/d2x/awesome 0.4.58)。
- GitCode `latest` release 的 **API 资产列表里也列出**了 `xim-index-scode-0.4.58.tar.gz` 等,
  但 `https://gitcode.com/.../releases/download/latest/<asset>` **对所有资产都 404**——
  包括主索引 `xim-index-72b00a4.tar.gz`(而后者 xlings 实际能拉到!)。
- 即:**GitCode 不支持 GitHub 式的 `releases/download/<tag>/<asset>` 直链**;xlings 拉主索引
  走的是另一条(指针 + GitCode API 资产 URL)路径,子索引却拼了 `releases/download/latest/<name>`
  → 404。GitCode 上还缺各 `xim-index-<sub>-latest.json` 子指针(GitHub 有)。

**根因**:xlings 子索引 artifact 的 **GitCode 下载 URL 构造错误**(用了 GitHub 直链格式),
且 GitCode 缺子索引 pointer。属 xlings 内部 fetch 逻辑(非 mcpp,非本次索引发布 CI 范畴)。
**下一步(二选一或都做)**:
1. xlings 修子索引 fetch:GitCode 用 API 资产 URL(同主索引路径),别拼 `releases/download/`。
2. 发布侧:把子索引 `*-latest.json` 指针也推到 GitCode `latest`,并确认子索引 artifact 走可下载的 URL。
**影响**:不阻断(回退 git via ghproxy 能成),但 CN 弱网体验差、慢。
