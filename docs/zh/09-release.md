# 09 — 发布 mcpp

**mcpp 自身**的发布如何到达用户手上。本章面向维护者;打包**自有项目**见
[02 — 打包发布](02-pack-and-release.md)。

在此之前这套流程只活在 commit message 和 workflow 注释里，其中一条 commit message
里的诊断是错的，已在 §5 更正。

## 1. 三处持久化版本号，加一个运行时推导的 CI 值

| 位置 | 组 | 何时变 |
|---|---|---|
| `mcpp.toml` `[package].version` | **正在构建的** | 开始做新版本时 |
| `modules/versioning/src/version.cppm` `MCPP_VERSION` | **正在构建的** | 与上一行同一个 commit（编译进二进制的副本） |
| `.xlings.json` `[workspace].mcpp` | **自举起点** | 单独地、在某个版本**已可安装之后** |
| `ci-fresh-install.yml` `MCPP_PIN` | **被测版本** | **不变 —— 运行时推导**（§5） |

`.github/tools/check_version_pins.sh` 机器校验剩下的关系：两处"正在构建的"
必须相等，自举 pin 永远不得**新于**正在构建的版本。

```bash
bash .github/tools/check_version_pins.sh
```

必须用 **bash** 跑，不能用 `sh`。脚本用了进程替换（`done < <(...)`），
POSIX `sh`/dash 解析不了 —— `sh check_version_pins.sh` 会在第 95 行附近报
`Syntax error: redirection unexpected`。那是**调用它的 shell** 的问题，
不是脚本的缺陷：它的 shebang 是 `#!/usr/bin/env bash`，CI 也是用 `bash` 调的。

两组刻意允许不同。把它们一起 bump 正是 pin 校验器早期版本要求过的做法，
结果是所有 CI 都去装一个还不存在的版本。

## 2. 发布管线

`release.yml`（推 tag，或 `workflow_dispatch` 不带输入时从 `mcpp.toml` 推导 tag）会做完这些：

```
四平台构建（linux x86_64 / linux aarch64 / macOS ARM64 / Windows x64）
  → GitHub Release v<version>，含 tarball 与 .sha256 边车文件
  → 重新计算每个载荷的哈希并发布不可变 mcpp-release.json
  → 镜像到 xlings-res/mcpp 的 GitHub 与 GitCode 双端
  → 向 openxlings/xim-pkgindex 开版本 bump PR
  → workflow_run 钩子触发 ci-fresh-install
```

### 2.1 不可变 release manifest

`release-manifest` 会等待四个平台上传 job 全部结束，然后下载最终的非 draft、
非 prerelease GitHub Release，重新计算每个带版本平台载荷的 SHA256，校验对应的
`.sha256` 边车文件，并发布 schema 1 的 `mcpp-release.json`：

```json
{
  "schema": 1,
  "version": "<version>",
  "tag": "v<version>",
  "commit": "<tag-所指完整-commit>",
  "assets": [
    {
      "platform": "linux",
      "arch": "x86_64",
      "name": "mcpp-<version>-linux-x86_64.tar.gz",
      "sha256": "<重新计算的-sha256>"
    }
  ]
}
```

数组按平台、架构、名称排序，包含所有带版本的平台载荷；其中 Linux x86_64、
Linux aarch64、macOS ARM64、Windows x86_64 四项是硬性要求。无版本别名和源码包
刻意不进入 desired-state 行。

workflow 随后会再次下载公开 release、重新生成 manifest，并要求逐字节一致。
重跑 workflow 时，已有 manifest 只有在字节完全相同时才会被接受；同一 tag 下
绝不以不同内容覆盖。下游发布消费者（尤其 `mcpp-bin` AUR reconciler）必须消费
该 manifest，不能从会变化的工作区或部分 release 资产猜测发布是否完整。

两步**没有**自动化：

- **合并 xim-pkgindex 的 bump PR** —— 由维护者完成。在它落地之前，发布出来的版本
  可以下载，但无法通过 `xlings install` 安装。
- **bump `.xlings.json`** —— 见 §4。

bump PR 是机器生成的：分支 `bump/mcpp-<version>`，提交者 `xlings-ci
<ci@xlings.dev>`，diff 恒为 +22/-3。承重的是那三行删除 —— 生成器对每个平台表是
**替换** `["latest"]` 那一行，而不是新增一行。手写一个索引 PR 并不是等价的捷径；
在 bot 的 PR 之外另开一个，代价有两份，2026-09-05 的 `xim-pkgindex#764` 两份都付了：

- 手写的 diff 把 `["latest"] = { ref = "<新版>" }` **追加**在原有那行上面而不是替换
  它，于是每个平台表里留下两个 `["latest"]` 键。Lua 的表构造器以最后一次赋值为准，
  `latest` 因此解析回上一个发布。精确版本条目存在且正确，所以每个按精确版本 pin 的
  消费者都是绿的 —— mcpp 自己的 CI 正是精确 pin，完全看不见这件事。只有不带版本的
  `xlings install mcpp` 会拿到过期的二进制。
- 它先于 bot 的 PR 落地，使那个 PR 变成冲突。解法是取 bot 分支那一侧的
  `pkgs/m/mcpp.lua`；它与手写后的 `main` 只差那几行陈旧的 `["latest"]`。

因此，bump PR 合入的判据是 `latest` **解析成什么**，而不是文件里别处出现了版本字符串：

```bash
curl -fsSL https://raw.githubusercontent.com/openxlings/xim-pkgindex/main/pkgs/m/mcpp.lua \
  | grep -n '\["latest"\]'
```

恰好回来三行，每个平台表一行，且都指向刚发布的版本。出现第四行，或某一行指向上一个
发布，就是上述的重复键缺陷。

## 3. 验证一次发布

镜像脚本会自校验上传，但真正值得手工做的是那些**不信任边车文件**的检查：

```bash
V=<version>
# 双端都服务全部四个平台，字节数一致
for a in linux-x86_64.tar.gz linux-aarch64.tar.gz macosx-arm64.tar.gz windows-x86_64.zip; do
  for h in github.com gitcode.com; do
    curl -fsSL -o /dev/null -w "$h $a %{http_code} %{size_download}\n" \
      "https://$h/xlings-res/mcpp/releases/download/$V/mcpp-$V-$a"
  done
done
# 索引里的 sha256 与载荷相符（**重新计算**，不要读边车文件）
curl -fsSL -o /tmp/p.tgz "https://github.com/xlings-res/mcpp/releases/download/$V/mcpp-$V-linux-x86_64.tar.gz"
sha256sum /tmp/p.tgz   # 与 xim-pkgindex 的 pkgs/m/mcpp.lua 对照
```

要在本地针对 GitHub 公开资产重放 release gate：

```bash
V=<version>
TAG="v$V"
AUDIT=$(mktemp -d)
mkdir -p "$AUDIT/assets"
gh api "repos/mcpp-community/mcpp/releases/tags/$TAG" > "$AUDIT/release.json"
gh release download "$TAG" -R mcpp-community/mcpp --dir "$AUDIT/assets"
python3 scripts/release/generate_manifest.py \
  --release-json "$AUDIT/release.json" \
  --assets-dir "$AUDIT/assets" \
  --version "$V" \
  --tag "$TAG" \
  --commit "$(git rev-list -n 1 "$TAG")" \
  --output "$AUDIT/expected.json"
cmp "$AUDIT/assets/mcpp-release.json" "$AUDIT/expected.json"
```

这个命令会重新计算载荷哈希，不会从已发布 manifest 抄哈希，也不会盲信边车文件。

然后在 **clean-room `XLINGS_HOME`** 里真装一次 —— 绝不要用本机的 `~/.xlings`，
它的缓存状态会把一个坏掉的索引掩盖过去：

```bash
export XLINGS_HOME=$(mktemp -d)
xlings update
xlings install mcpp@$V -y
$(find "$XLINGS_HOME" -name mcpp -type f -path '*/bin/*' | head -1) --version
```

**索引传播不是即时的。** `xim-pkgindex` 是以 CDN artifact 而非 git clone 的形式
到达客户端的，所以刚合并的 bump 会有一段时间不可见（2026-07-30 实测 ~5 分钟，
记录在案的上限约 40 分钟）。clean-room 里仍然报旧的 `latest` 不是失败，是**还没追上**。
`ci-fresh-install` 的 `wait-index` job 正是把这件事编码成了 15 分钟有界等待。

## 4. 自举 pin：它是什么，什么时候该 bump

`.xlings.json` 的 `[workspace].mcpp` 是**自举的起点** —— 那个由
`xlings install mcpp` 装进 workspace、供 CI 从源码构建 mcpp 的已发布 mcpp。
它唯一的要求是：能构建**当前**这棵源码树。

**它不必每次发布都跟着动。** 索引保留每一个已发布版本（撰写时 105 个条目，
一直回溯到 0.0.x 系列），旧 pin 可以无限期继续解析 —— 这一点用「在当前索引下安装
一个隔了两个版本的旧版」实测验证过。

跟着 bump 仍然是合理的，也是本仓库的实际做法：bump 后 CI 一轮全绿，直接证明了
新发布能在每个平台上构建 mcpp 自己。把它当作**一项有用的检查**，而不是前置条件。

**唯一的硬约束是方向**：pin 绝不能指向一个尚不可安装的版本。只在发布已完成、
已镜像、**且已合入 xim-pkgindex 之后**再 bump —— 否则所有 CI 会以
`package 'mcpp@<unreleased>' not found` 失败。待其语法问题修复后，
`check_version_pins.sh` 能卡住较弱的「不得新于正在构建的版本」；索引那个条件需要人工把关。

「已合入 xim-pkgindex」是必要条件而非充分条件：客户端读的是 CDN 上的 artifact，不是
git 树。2026-09-05 那次，pin 的 commit 比索引 PR 的合入早到 `main` 19 秒，而它触发的
CI 作业在新指针资产被替换**之后** 51 秒才去解析索引 —— 拿到的仍然是上一份 artifact。
九条 workflow 全部死在 bootstrap，没有一条编译过一行。可观测的条件是指针本身：

```bash
curl -fsSL https://github.com/xlings-res/xim-index/releases/download/latest/xim-index-latest.json \
  | grep -E '"(index_version|source_commit)"'
```

推 pin 之前，`index_version` 必须等于 xim-pkgindex `main` 的短 SHA。没有任何东西强制
这一点，而在作业日志里，由此产生的失败与「版本名真的写错了」无法区分。

## 5. `MCPP_PIN` 改为推导，以及这为什么重要

`ci-fresh-install.yml` 过去带着 pin 的第二份手工副本。它们从来就不是一回事：
`MCPP_PIN` 是**被测版本** —— 永远是最新的已发布版本；而 `.xlings.json` 是
**自举来源**。

现在它由 `wait-index` job 从 releases API 推导一次，所有安装 job 消费同一个输出。
有两条性质必须成立，而写死的字面量只买到了第一条：

1. **版本必须是显式的。** 裸 `xlings install mcpp` 解析的是「runner 自己那份索引副本里
   的最新」，于是副本落后的 runner 会**悄悄测一个旧二进制然后报绿**。写明版本能让落后的
   索引以 `version not found` 响亮失败。推导出来的字符串与字面量一样显式。
2. **守卫等的版本必须就是 job 装的版本。** 2026-07-21 它们不是：索引守卫报
   「index tracks 0.0.102」，10 秒后 job 装的是 0.0.100，撞上 floor 为 0.0.101 的索引（#265）。
   守卫本来就推导出了正确答案，然后把它扔掉了。让两者吃同一个值，
   使这种不一致在结构上不可能发生。

`check_version_pins.sh` 会在字面量 `MCPP_PIN:` 重新出现时报错。不要重新引入字面量，
否则索引守卫与实际安装版本又会发生漂移。

> **更正。** commit `3b1cb6b`（"bootstrap pin -> 2026.7.29.2"）写着
> *"the index no longer serves .1"* 并引用了 `version '2026.7.29.1' not found`。
> **这个诊断是错的**：`2026.7.29.1` 在当前索引下能正常安装。真正的原因是
> **本地索引副本陈旧** —— 就是 §3 描述的那个传播滞后，只是从另一侧看到的。
> 发布不会移除任何旧版本，任何推理都不该建立在「会移除」这个前提上。

## 6. 检查清单

```
[ ] mcpp.toml + fingerprint.cppm 版本号已 bump（同一个 commit）
[ ] CHANGELOG 条目
[ ] `bash .github/tools/check_version_pins.sh` 通过（校验 `mcpp.toml` = `MCPP_VERSION`，且 `.xlings.json` 未领先）
[ ] 合入 main，CI 全绿
[ ] gh workflow run release.yml --ref main
[ ] release.yml 全绿（4 个构建 + 不可变 manifest + publish-ecosystem）
[ ] 下载的 mcpp-release.json 能从公开资产逐字节重新生成
[ ] 双端都服务四个平台，sha256 重新算过
[ ] 合并 xim-pkgindex 的 bump PR
[ ] 索引的 `latest` 在三个平台表里都解析到新版本
[ ] xim-index-latest.json 的 `index_version` 等于 xim-pkgindex `main`
[ ] clean-room XLINGS_HOME：xlings install mcpp@<version> 成功
[ ] （可选）bump .xlings.json —— 只在此刻，绝不提前
```
