# `ci-fresh-install` 11 个 job 全红 —— 两个独立缺陷的修复方案

> 状态：**已实施**（2026.8.4.1）。实施记录见 §8。
> 范围：`.github/workflows/ci-fresh-install.yml`（+ 一处共享机制）
> 相关：`ci-aarch64-fresh-install.yml`（已用另一种方式规避了缺陷 A）、
> `.github/tools/install_pinned_mcpp.sh`（已记录过同一族问题）

---

## 0. 摘要

11 个 job 全红看起来像「很多问题」，实际是**两个独立缺陷**，各自都能单独把整个矩阵打红，
而且**都不是本次发布引入的** —— 每次发布后必现。

| | 缺陷 | 触发条件 | 症状 |
|---|---|---|---|
| **A** | 仓库根 `.xlings.json` 的 workspace pin 伏击了被测版本 | 最新发布 ≠ bootstrap pin（**每次发布后的常态**） | `xlings: version '2026.8.3.2' not found for 'mcpp'` |
| **B** | `wait-index` 守的是**另一条分发通道** | 发布后索引 artifact 还没传播开 | `package 'mcpp@2026.8.3.5' not found; searched repos: [xim]` |

两者形状相同，也和今天修的另外两个 bug 形状相同：**验证的对象不是使用的对象**（§3）。

---

## 1. 缺陷 A：仓库的 workspace pin 伏击了被测版本

### 证据

2026-08-03 13:41 手工触发那次（索引早已就绪，排除掉 B）：

```
✓ 1 package(s) installed                      ← mcpp@2026.8.3.4 装好了
[error] xlings: version '2026.8.3.2' not found for 'mcpp'
[error]   available: 2026.8.3.4
```

**装的是对的，跑的是别的。** 11 个 job 全部死在同一步 —— 第一次执行 `mcpp` 的地方
（`Install mcpp and config mirror`，distro 矩阵是 `Configure mcpp`）。

### 机制

两个 pin 撞在一起，而它们**按设计就应该不同**：

| 位置 | 值 | 用途 |
|---|---|---|
| `MCPP_PIN`（`wait-index` 推导） | `2026.8.3.4` | **被测版本** = 最新发布 |
| `.xlings.json` → `workspace.mcpp` | `2026.8.3.2` | **自举**版本，手工维护，**故意滞后** |

job 的步骤顺序：

1. `- uses: actions/checkout@v4` ← **第一步**，于是仓库的 `.xlings.json` 落在工作目录里
2. `xlings install "mcpp@${MCPP_PIN}" -y -g` ← 只装了 `2026.8.3.4`，装到全局
3. `mcpp --version` ← xvm shim 看到 workspace pin，去解析 **`2026.8.3.2`** —— 从没装过

本机实测复现（同一台机器、同一个 shim）：

```
目录里有 .xlings.json（pin 2026.8.3.2）  →  mcpp 2026.8.3.2
目录里没有                                →  mcpp 2026.8.3.3   （全局默认）
```

**workspace pin 赢。** 在我本机两个版本都装着所以只是静默切换；在 CI 上只装了被测版本，
于是直接报错。

### 为什么「每次发布后必现」

workflow 自己的头注释就说了这两者不是一回事：

> NOT to be confused with the .xlings.json workspace pin … it stays hand-maintained.

作者知道它们不同 —— 但 job 仍然在**仓库目录里**执行 `mcpp` 命令，而在那里 workspace pin
拥有最终解释权。所以只要「最新发布 ≠ bootstrap pin」，也就是**每一次发布之后**，
这个 workflow 必红。历史记录与之吻合：`workflow_run` 触发的每一次都失败，
偶尔的绿色出现在两者恰好相等的窗口里。

### 为什么调整 checkout 顺序不够

`ci-aarch64-fresh-install.yml` 遇到过同一个坑，并用**把 checkout 放到最后**解决了，
注释写得很完整：

> this repo's .xlings.json declares an `mcpp` WORKSPACE pin, so with the checkout present
> in $GITHUB_WORKSPACE `xlings install mcpp` installs workspace-scoped instead of globally

**但这个办法在 `ci-fresh-install` 用不了**：它的 `Default: build mcpp` 等步骤要在仓库里跑

```yaml
- name: "Default: build mcpp"
  run: |
    mcpp clean
    mcpp run
```

—— 自举构建本来就必须发生在仓库目录内。checkout 不能挪到它们后面。

---

## 2. 缺陷 B：`wait-index` 守的是另一条分发通道

### 证据

2026-08-03 18:11 发布后那次，`wait-index` **成功**，随后 11 个 job 全部：

```
[error] package 'mcpp@2026.8.3.5' not found
[error]   searched repos: [xim]; run `xlings update` if the package was just published
```

时间线：

| 时刻 | 事件 |
|---|---|
| 18:11 | release 完成 → `workflow_run` 触发 ci-fresh-install |
| ~18:14 | 索引 bump PR 合入 ⇒ **git 上的 `pkgs/m/mcpp.lua` 立刻带上 2026.8.3.5** |
| 18:15:05 | `Publish Index Artifact` 生成 artifact |
| — | `wait-index` 轮询 git 文件命中 ⇒ **判定「索引已就绪」，放行 11 个 job** |
| 18:16:41 | job 执行 `xlings update` + install ⇒ 拿到的索引里**没有** 2026.8.3.5 |

### 机制

`wait-index` 轮询的是 **git 真源**：

```bash
curl -fsSL "https://raw.githubusercontent.com/openxlings/xim-pkgindex/main/pkgs/m/mcpp.lua" \
  | grep -q "\"$VER\""
```

而 job 里的 `xlings update` 取的是**发布出来的 artifact**
（`xlings-res/xim-index` 的 `xim-index-latest.json` → tarball）。

**这是两条延迟完全不同的通道。** git 文件在 PR 合入的瞬间就更新了；artifact 还要等
`Publish Index Artifact` 打包、再经 release CDN 传播 —— 实测边缘缓存滞后可达数十分钟。

⇒ **守卫在测量一条没人从那里安装的通道。** 它报告「索引已就绪」时，
job 要用的那条通道可能还差得远。

这跟 workflow 头注释里记载的 #265 是同一类事故的再现 ——
当时是「守卫等的版本」与「job 装的版本」不一致，这次是「守卫查的通道」与
「job 用的通道」不一致。**上次修的是值，这次要修的是通道。**

---

## 3. 共同的形状（今天第四次）

| 场景 | 检查的东西 | 使用的东西 |
|---|---|---|
| #344 cache | 条目**自述的**文件表 | 消费方**自己算的**地址 |
| #345 e2e 初稿 | 「构建成功」 | 真正要保的是「零 compile 边」 |
| E0006 e2e 初稿 | 「输出里出现 E0006」 | 真正坏的是**最后一条**错误 |
| **本文 A** | 装了哪个版本 | shim 实际**解析**到哪个版本 |
| **本文 B** | **git** 通道有没有该版本 | job 用的是 **artifact** 通道 |

一句话判据：

> **验证的对象，必须就是使用的对象。**

每一次的修法都一样：不是把检查写得更严，而是**把检查挪到被使用的那个东西上**。

---

## 4. 方案

### F1（缺陷 A）· 在这个 workflow 里，让被测版本成为唯一的答案

`ci-fresh-install` 的职责是「验证**已发布的** mcpp」。仓库的 bootstrap pin 是给
**自举 CI**（ci-linux 等）用的，在这里没有任何发言权 —— 它只会把被测对象悄悄换掉。

checkout 之后立刻中和它：

```yaml
      - uses: actions/checkout@v4

      # This workflow tests the RELEASED mcpp (MCPP_PIN). The repo's
      # .xlings.json carries a WORKSPACE pin for the self-host CI's bootstrap —
      # a different version, deliberately lagging — and inside this checkout it
      # wins over anything installed globally: `mcpp` would resolve to the
      # bootstrap version, or fail outright when (as here) only the version
      # under test is installed. Neither is what this workflow is for.
      #
      # Removing the file is the fix rather than moving the checkout later:
      # the `build mcpp` steps below run `mcpp clean && mcpp run` INSIDE the
      # repo, so a checkout has to be present while mcpp is being invoked.
      # (ci-aarch64-fresh-install.yml can and does order it away, because its
      # fresh-install steps never need the repo.)
      - name: Neutralize the repo's bootstrap pin (see above)
        shell: bash
        run: rm -f .xlings.json
```

同时，安装步骤补上**激活**与**断言**：

```yaml
      - name: Install mcpp and config mirror
        shell: bash
        run: |
          xlings update
          xlings install "mcpp@${MCPP_PIN}" -y -g -u   # -u activates; install alone does not

          # ASSERT the shim resolves the version under test. `install` reports
          # success for "the bytes are on disk", which is not the same claim as
          # "`mcpp` now runs it" — and every later step in this job runs the
          # bare shim. Without this, any future ambient override (a workspace
          # pin, a stale activation, a PATH surprise) silently retargets the
          # whole matrix at a DIFFERENT binary and still reports green.
          got="$(mcpp --version | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)"
          [ "$got" = "$MCPP_PIN" ] || {
            echo "::error::mcpp resolves to '$got' but the version under test is '$MCPP_PIN'"
            echo "hint: something is redirecting the shim — a .xlings.json workspace pin in"
            echo "      \$GITHUB_WORKSPACE is the usual culprit; xvm activation is the other."
            exit 1
          }
          mcpp self config --mirror GLOBAL
```

`-u` 不是可选项：`install_pinned_mcpp.sh` 的头注释已经写过为什么 ——
*"`-u` activates the version just installed … that is the piece a plain `install` leaves alone,
and the reason CI could install one version and then run another."*
这个 workflow 是唯一没享受到那份修复的地方，因为它没走那个脚本。

**断言是这条修复里唯一能防住「下一个」的部分。** 它把任何未来的静默替换
（不只是 workspace pin）变成一条点名的失败。

落点：5 个 job（3 个 bash + 2 个 PowerShell）。两个 Windows job 的安装步现在是
`shell: pwsh`，改成 `shell: bash` 即可复用同一段（同 job 内已有 `shell: bash` 的步骤，
Git Bash 在 windows runner 上可用）。

### F2（缺陷 B）· 守卫必须查 job 真正消费的那条通道

把 `wait-index` 的轮询从 git 换成 **artifact**：

```yaml
      - name: Wait for the published index ARTIFACT to carry the released mcpp
        if: ${{ github.event_name == 'workflow_run' }}
        run: |
          # NOT the git file. `xlings update` installs from the published
          # artifact (xlings-res/xim-index → xim-index-latest.json → tarball),
          # and that channel lags git by however long Publish Index Artifact
          # plus release-CDN propagation takes — tens of minutes, measured.
          # Polling git says "ready" while the channel the jobs actually use is
          # still serving the previous index. Check what the consumer consumes.
          for i in $(seq 1 40); do
            ptr=$(curl -fsSL "https://github.com/xlings-res/xim-index/releases/download/latest/xim-index-latest.json" || true)
            name=$(printf '%s' "$ptr" | python3 -c "import json,sys; print(json.load(sys.stdin)['artifact']['name'])" 2>/dev/null || true)
            if [ -n "$name" ] && curl -fsSL "https://github.com/xlings-res/xim-index/releases/download/latest/$name" \
                 | tar -xzO --wildcards '*/pkgs/m/mcpp.lua' 2>/dev/null | grep -q "\"$VER\""; then
              echo "published index artifact tracks $VER (after $((i*30))s)"; exit 0
            fi
            sleep 30
          done
          echo "::error::the published index artifact never tracked $VER within 20min"
          exit 1
```

**更稳妥的补充（建议一起做）**：在 job 内给 `xlings update` 加一个有界重试，
直到本地索引真的能解析出被测版本再往下走。理由是 CDN 是**按边缘节点**传播的 ——
守卫所在的 runner 看到了，不代表另一个 runner 也看到了。
守卫把窗口收窄到「几乎总是就绪」，job 内的重试兜住剩下的边缘差异。

> 这两层的分工要说清楚：F2 的守卫**省掉 11 个 job 各等 20 分钟**，
> job 内重试**保证正确性**。只做守卫会留下按节点抖动的偶发红；
> 只做重试则每次发布都让整个矩阵陪跑等待。

### F3（通用化）· 让这条知识不再靠人记住

缺陷 A 的知识**存在于仓库里** —— 完整地写在 `ci-aarch64-fresh-install.yml` 的注释里，
而 `ci-fresh-install.yml` 从来没学到。这是典型的「同一决策两处推导，其中一处不知情」。

两条低成本措施：

1. **F1 里那条断言就是主要防线** —— 它不依赖任何人读注释，
   且能捕获**未知的**替换机制，而不只是 workspace pin 这一种。
2. **加一条机器判据**到 `.github/tools/check_version_pins.sh`（它已经在扫 `.github/`，
   并且已经有一条同类守卫 —— 拒绝字面量 `MCPP_PIN:`）：

   > 任何 workflow 里出现 `xlings install ... mcpp@`（按显式版本安装），
   > 其所在 job 必须要么不 checkout 本仓库，要么中和 `.xlings.json`。

   精确的可执行近似：**禁止 workflow 直接手写 `xlings install "mcpp@…"`**，
   统一走一个共享脚本（`.github/tools/install_released_mcpp.sh`），
   脚本里封装「中和 pin + `-u` 激活 + 断言」。
   守卫只需 grep：`.github/workflows/` 下不得出现 `xlings install` 与 `mcpp@` 同现。
   `ci-aarch64` 的裸 `xlings install mcpp -y`（无版本，故意如此）不会被误伤。

---

## 5. 不采纳

| 方案 | 理由 |
|---|---|
| 把 `.xlings.json` bump 到最新发布 | **方向反了。** bootstrap pin 故意滞后（它必须是一个「已发布且能构建当前树」的版本），把它跟着发布走正是 `release-bootstrap-pin-two-groups` 记载过的、会让全部 CI 去装不存在版本的经典错误。 |
| 把 checkout 挪到所有安装步骤之后 | 对 `ci-aarch64` 有效，对本 workflow **无效**：`build mcpp` 步骤必须在仓库内执行（§1）。 |
| 只删 `.xlings.json`，不加 `-u`/断言 | 把一个**响亮的失败**换成一个**静默测错版本**的可能 —— 而这个 workflow 的头注释说，它存在的理由之一正是防止「静默测了旧二进制还报绿」。 |
| 只修 A 不修 B | 手工/cron 触发会绿，**发布后触发仍然红** —— 而那正是这个 workflow 最该起作用的时刻。 |
| 让 `wait-index` 多等固定时间 | 猜一个常数。CDN 传播是按边缘节点的、无上界的；固定等待要么不够要么浪费。查真实通道 + job 内重试才是判据。 |

---

## 6. 验证

- **先红后绿**：两个缺陷都能在**不发版**的情况下复现 ——
  - A：手工 `workflow_dispatch` 触发（索引早已就绪），当前必红于 `version '<pin>' not found`；
  - B：需要一次发布后触发；退而求其次，可在 `wait-index` 里临时把轮询目标改回 git
    并断言 artifact 尚未跟上，以证明两条通道确实会分叉。
- 修复后：`workflow_dispatch` 一次（覆盖 A），下一次真实发布覆盖 B。
- **断言自证**：临时把 `MCPP_PIN` 改成一个已装但非目标的版本，F1 的断言必须变红。
- `bash .github/tools/check_version_pins.sh` 仍须通过（F3 若加了新守卫，同样要先看到它红）。

---

## 7. 实施顺序

| # | 项 | 依赖 | 效果 |
|---|---|---|---|
| 1 | **F1** 中和 pin + `-u` + 断言（5 个 job） | 无 | 手工/cron 触发恢复绿；发布后触发不再死在第一步 |
| 2 | **F2** 守卫改查 artifact + job 内有界重试 | 无 | 发布后触发也能绿 |
| 3 | **F3** 共享脚本 + `check_version_pins.sh` 守卫 | F1 | 这条知识不再靠人记住 |

F1 与 F2 可以在同一个 PR 里；F3 建议同 PR，因为它的价值恰恰在于**趁现在**把知识固化下来
—— 这个坑已经被独立发现过两次了。


---

## 8. 实施记录（2026.8.4.1）

落点：`.github/tools/install_released_mcpp.sh`（新）、`ci-fresh-install.yml`（5 处调用点 + `wait-index`）、
`src/xlings.cppm`（xlings pin）、`.github/**`（16 个 pin 点）。

**F1+F3 合并落地**：没有在 5 个 job 里各写一段，而是收敛进一个共享脚本
（`install_pinned_mcpp.sh` 的兄弟）。理由就是 §3 那条 —— 这个坑已经被**独立发现过两次**，
知识散在注释里必然有第三次。两个 Windows job 的安装步从 `shell: pwsh` 改为 `shell: bash`，
以复用同一份实现（同 job 内早已有 `shell: bash` 的步骤）。

**F2 的 `wait-index`**：改查 artifact 通道后**用真实指针实测过**提取逻辑 ——
`xim-index-e8ad461.tar.gz`，解包后 `pkgs/m/mcpp.lua` 确实含 `"2026.8.3.5"`。

### 实施中被证伪的两处

**① 断言的对象一开始就写错了。** 初版是

```bash
MCPP="$XL_HOME/.xlings/subos/default/bin/mcpp"     # 猜一个安装路径
```

而后续步骤敲的是裸 `mcpp`，走 **PATH**。**探测一个没人执行的路径，等于验证了一个副本**
—— 正是本文 §3 那张表在说的事，我自己又犯了一次。改为 `command -v mcpp` 优先，
已知位置只作 PATH 尚未导出时的兜底。

**② `-u` 不够，补 `xlings use`。** 隔离环境实测：payload 里的二进制自报
`mcpp 2026.8.3.5`（正确），而 shim 报 `2026.7.29.1`。
`xlings install -u` 是安装期激活，`xlings use` 是显式切换 ——
它们在 xlings 里是两条不同代码路径，而这里承重的只有其中一条。
现在两个都做，**断言仍然是最终判据**（补切换是把激活做完，不是给失败开脱）。

> 这两处都是断言自己抓出来的。**它在本机就抓到了两个真问题，而不是等到 CI 红** ——
> 这正是加它的理由：把「静默测了错的二进制还报绿」变成一条点名的失败。

### xlings 2026.8.4.1 的验证

- `xlings index list --json` 已可用，`requires` 字段按设计透传（当前 xim 索引尚未声明 `requires`，
  history 只有 1 条 —— 机制是新的，历史会累积）。
- 实测 mcpp 在该版本下 `new` / `build` / `run` 正常（把 2026.8.4.1 塞成 mcpp 的
  vendored xlings 后跑通）。
- 四平台产物齐全（含 aarch64，release.yml 的三处字面量依赖它）。

**mcpp 侧消费索引路由是后续工作**：需要索引侧先在 `index-compat.json` 里声明
`requires.mcpp`，mcpp 再读 `xlings index list --json` 按自己的 `min_mcpp` 过滤并
`xlings index use`。届时
`.agents/docs/2026-08-03-index-availability-must-not-decide-mcpp-availability.md` 的
M2（本地快照历史）与之互补：M2 覆盖本机见过的快照，路由覆盖全部已发布快照。
