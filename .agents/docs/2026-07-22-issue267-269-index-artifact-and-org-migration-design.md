# 索引组织迁移 + 采纳 xlings 0.4.68 per-repo artifact 来源 — 设计方案

**日期**: 2026-07-22
**关联**: mcpp#267(索引 URL 指向已迁移旧组织 + 被判定自定义索引强制走 git)、
mcpp#269(采纳 xlings 0.4.68 per-repo artifact)、
openxlings/xlings#377(功能实现,PR #379)、#378(compact::git CA 探测)、#380(shim env 去重)
**上游设计文档**: xlings 仓 `.agents/docs/2026-07-22-issue377-custom-index-artifact-design.md`
**上游 schema**: xlings 仓 `docs/spec/xlings-json-schema.md`(0.4.68+ `artifact` / `source` 字段)

---

## 一、问题陈述

两个 issue 收敛为同一条主线:**mcpplibs 索引的同步路径健壮化**,外加一个正确性前提(URL 组织迁移)。

### #267-A:索引 URL 仍指向已迁移的旧组织(正确性,可独立修)

索引仓库已从 `mcpp-community/mcpp-index` 迁到 `mcpplibs/mcpp-index`(工具仓库
`mcpp-community/mcpp` 未迁移)。mcpp 目前**完全靠 GitHub 仓库重定向**才能工作,
该依赖脆弱:旧名一旦被再次占用即静默指向错误内容;部分镜像/代理不跟随重定向。

### #267-B / #269:索引同步硬依赖 git → 采纳 artifact 路径

xlings 0.4.68 前,自定义 `index_repos`(mcpplibs 即是)恒走 git
(`repo.cppm` 准入三分支全要求 `isDefaultOfficial`),拿不到官方索引 artifact 路径的
adaptive mirror reorder + stall watchdog。实测症状:同一次 `mcpp index update` 里
主索引 artifact 成功、mcpplibs git 因宿主 CA 路径问题失败。

xlings 0.4.68(#377/#379)已落地:任何 `index_repos` 条目可声明 `artifact` 来源,
`source: auto`(默认)下 artifact 优先、git 自动回退。**发布端零改动**——
`xlings-res/mcpp-index` 现有布局(仓根 `mcpp-index-pointers.json` + `v<sha>` release +
`mcpp-index-<sha>.tar.gz`)与 xlings 消费端已实测逐字节兼容(v2e23e20:sha256 一致、
sole-entry key 兜底覆盖 `mcpp` ≠ `mcpplibs` 命名差异)。

mcpp 侧要做的只是:**声明 + 透传 + 治愈存量 + 升 pin**。

---

## 二、现状盘点(涉改位置)

### 代码(旧 URL 硬编码 4 处)

| 位置 | 作用 |
|---|---|
| `src/config.cppm:360` | `write_default_config_toml` 模板 `[index.repos."mcpplibs"] url` |
| `src/config.cppm:407` | `canonicalize_legacy_index_names` 名字迁移分支的旧 URL 联合条件 |
| `src/config.cppm:575` | `add_default("mcpplibs", <旧URL>)` 内存级默认 |
| `src/publish/pipeline.cppm:158` | `mcpp publish` 打印给用户的 Fork 地址 |

### 数据流(artifact 字段要穿过的层)

```
config.toml [index.repos."mcpplibs"]          ← 模板 + TOML 解析(config.cppm:535-543,现只读 url)
        │
        ▼
IndexRepo { name, url }                        ← src/config.cppm:36,需扩字段
        │
        ▼
seed_xlings_json(span<pair<name,url>>)         ← src/xlings.cppm:250/1110,pair 表达不了新字段
        │
        ▼
registry/.xlings.json "index_repos"            ← xlings 0.4.68 消费 artifact/source
        │(仅在缺失时 seed;存量安装走 migrate_xlings_json_index_names,config.cppm:581-586)
        ▼
xlings sync(0.4.68:artifact 优先 + git 回退)
```

另有两个旁路消费者,改动时必须不破坏:

- `src/pm/package_fetcher.cppm:252-300` — 手写解析 `.xlings.json` 的 `index_repos`
  (按 key 容错读 name/url,多余字段应天然无害,需单测锁住);
- 项目级 `ensure_project_index_dir`(`src/config.cppm:671-714`)— 从 `IndexSpec`
  构造 `customRepos` pair 后 seed 项目 `.mcpp/.xlings.json`。

### 迁移钩子(现成的,扩展即可)

| 钩子 | 位置 | 现状 |
|---|---|---|
| `migrate_config_toml_index_names` | `src/fallback/config_migration.cppm:55` | 文本替换 `mcpp-index`→`mcpplibs`(名字) |
| `migrate_xlings_json_index_names` | `src/fallback/config_migration.cppm:69` | 同上,针对 `.xlings.json` 的 `"name"` 字段 |
| `canonicalize_legacy_index_names` | `src/config.cppm:400` | 内存级名字迁移 + 去重 |

### 版本 pin

- CI:`release.yml:99/281-345`、`cross-build-test.yml:105/218`、`ci-linux-e2e.yml:126`
  全部 `0.4.67`;
- 代码常量:`src/xlings.cppm:36` `pinned::kXlingsVersion = "0.4.51"` —— **已陈旧**,
  仅被 `config.cppm:34` 的 `kXlingsPinnedVersion` re-export,且后者无任何消费点
  (捆绑 xlings 实际来自 release bundle / `MCPP_VENDORED_XLINGS` / 系统 PATH,
  见 `src/fallback/xlings_binary.cppm`)。本次一并对齐,消除"版本常量双源"旧债
  (0.0.93 批次教训)。

### 文档(非阻塞,顺带)

`docs/00-getting-started.md:98`、`docs/04-build-from-source.md:96` 及 zh 对应文件
仍链接 `mcpp-community/mcpp-index`。

---

## 三、设计

**核心理念:默认值与治愈逻辑都收敛在代码常量一处;文件层迁移只做最小文本手术。**
新 URL 与 artifact base 定义为单一常量对,所有消费点(模板、add_default、canonicalize、
迁移、publish 提示)引用它,不再各写一份字面量(避免下次迁移再来一遍 4 处 grep)。

```cpp
// src/config.cppm(或独立常量区)
inline constexpr std::string_view kMcpplibsUrl      = "https://github.com/mcpplibs/mcpp-index.git";
inline constexpr std::string_view kMcpplibsUrlLegacy= "https://github.com/mcpp-community/mcpp-index.git";
inline constexpr std::string_view kMcpplibsArtifact = "https://github.com/xlings-res/mcpp-index";
```

### D1. URL 组织迁移(#267-A,可独立发)

1. **4 处硬编码**全部改为新 URL(经上面常量)。
2. **`canonicalize_legacy_index_names` 扩展**(`config.cppm:400`)——顺序陷阱是
   issue 里点名的坑:现有名字分支条件是 `name=="mcpp-index" && url==<旧URL>`,
   若先重写 URL 再进名字分支,老配置就匹配不上了。**先 URL 归一、名字分支只看 name**
   (归一后 URL 恒为新值,url 条件不再有区分意义;原 url 条件本意是避免误伤同名
   自定义仓,归一后改为接受新旧两个官方 URL 即等价):

   ```cpp
   for (auto r : cfg.indexRepos) {
       if (r.url == kMcpplibsUrlLegacy) r.url = kMcpplibsUrl;          // ① 组织迁移
       if (r.name == "mcpp-index" && r.url == kMcpplibsUrl) r.name = "mcpplibs"; // ② 名字迁移
       // ③ 去重逻辑不变 —— URL 归一后,"老条目+新默认条目"并存会在此折叠成一条
   }
   ```

   注意去重是 `name+url` 联合判等:URL 归一必须发生在去重**之前**,否则
   `mcpplibs@旧URL` 与 `mcpplibs@新URL` 会被当成两个仓保留。现在的单循环结构
   天然满足(逐条归一后再查重),保持即可。
3. **`migrate_config_toml_index_names` 扩展**(文本层,治愈磁盘上的 config.toml):
   增加 `replace_all(updated, 旧URL, 新URL)`。幂等:替换后源串不再出现。
4. **`migrate_xlings_json_index_names` 扩展**(治愈 registry `.xlings.json`
   ——存量安装只走 migrate 不走 seed,见 `config.cppm:582-586`):同样
   `replace_all` 旧 URL→新 URL(带/不带空格两种 JSON 间距变体,与现有 name
   替换的双变体写法一致)。
5. `publish/pipeline.cppm:158` Fork 地址、docs 4 处链接改新 org。

### D2. IndexRepo/seed 管线支持 artifact(#269 主体)

1. **`IndexRepo` 扩字段**(`config.cppm:36`):

   ```cpp
   struct IndexRepo {
       std::string name;
       std::string url;
       std::string artifact;   // 可选:artifact 来源 base(空 = 未声明,纯 git)
       std::string source;     // 可选:"auto"|"artifact"|"git"(空 = 不发射,xlings 默认 auto)
   };
   ```

   本轮 `artifact` 只支持 string 形态;xlings 侧的 region 对象
   `{"GLOBAL":..,"CN":..}` 留待 CN 镜像仓实际部署后再扩(见 §六)。

2. **TOML 解析**(`config.cppm:535-543`):`[index.repos.NAME]` 增读 `artifact`、
   `source` 两个可选 string key。旧 mcpp 二进制读新 config.toml 时因只查 `url`
   而自然忽略新 key,向后安全。

3. **`seed_xlings_json` 签名升级**(`src/xlings.cppm:250/1110`):
   `span<const pair<string,string>>` 表达不了新字段。在 `mcpp::xlings` 内新增轻量
   结构(保持该模块不依赖 `mcpp.config`):

   ```cpp
   struct SeedRepo {
       std::string name, url, artifact, source;
   };
   void seed_xlings_json(const Env&, std::span<const SeedRepo>, ...);
   ```

   发射逻辑:`artifact` 非空才写 `"artifact"` key;`source` 非空才写 `"source"` key
   ——未声明的仓输出逐字节不变,与 xlings 侧"未声明 = 纯 git 旧行为"对齐。
   两个调用方(`write_default_xlings_json`、`ensure_project_index_dir`)同步改为
   构造 `SeedRepo`。旧签名直接删除(内部 API,无兼容负担)。

4. **默认配置模板**(`config.cppm:359-361`):

   ```toml
   [index.repos."mcpplibs"]
   url      = "https://github.com/mcpplibs/mcpp-index.git"
   artifact = "https://github.com/xlings-res/mcpp-index"
   # source = "auto"  # 默认:artifact 优先,git 回退;可改 "git" 强制走 git
   ```

5. **`add_default` 对齐**(`config.cppm:571-575`):默认条目带 artifact。另在
   `canonicalize_legacy_index_names` 末尾补一条**内存级治愈**:凡 `url == kMcpplibsUrl`
   且 `artifact` 为空且用户未显式 `source = "git"` 的条目,填充默认 artifact。
   这样即使用户的 config.toml 是旧模板(没有 artifact 行),每次运行也拿到
   artifact 声明——**config.toml 不做 artifact 注入的文本手术**(用户可编辑文件,
   注入易碎且没必要;代码级默认已覆盖)。用户想退出 artifact 路径的显式出口就是
   `source = "git"`。

6. **`print_env`**(`config.cppm:663-668`)顺带打印 artifact 标记,便于 doctor 场景确认。

### D3. 存量 registry `.xlings.json` 的 artifact 注入(#269 建议 2)

registry `.xlings.json` 只在缺失时 seed,存量安装唯一的治愈通道是
`migrate_xlings_json_index_names`。扩展它(改名为语义更准的
`migrate_xlings_json_index_entries`,或保名加逻辑,倾向后者少动调用点):

```
若 文本含 "xlings-res/mcpp-index"            → 已注入,跳过(幂等闸)
否则 对 "url": "<新URL>" 与 "url":"<新URL>" 两种间距变体:
    replace_all(..., "\"url\": \"<新URL>\"",
                     "\"url\": \"<新URL>\", \"artifact\": \"<res base>\"")
```

顺序上依赖 D1-4 已先把旧 URL 归一为新 URL(同一次 migrate 调用内先 URL 后 artifact),
故只需匹配新 URL。幂等性由"已含 res base 即跳过"保证——这是必须的:
纯 `replace_all` 二次运行会重复注入,替换后的文本仍包含原匹配串。

文本手术的安全性论证:该文件由两个写者维护——mcpp 的 `seed_xlings_json`
(pretty,`"key": "value"`)与 xlings 自身的序列化器(compact 可能无空格),
双变体替换覆盖两者;URL 串带 `/` 不会出现在其它 value 里(项目级 `.xlings.json`
不经此迁移,只有 registry 的走这条路)。**不整文件重生成**——文件里有 xlings 的
subos/版本绑定状态,重生成会丢(0.0.9x 批次已确立的原则)。

旧 xlings(<0.4.68)读到 `artifact`/`source` 字段安全忽略(上游实测),
所以注入不需要先确认捆绑 xlings 版本,与 D4 无顺序耦合。

### D4. 捆绑 xlings pin → 0.4.68

- `release.yml`(x86_64 两处 + aarch64 硬编码 `0.4.67` 两处)、
  `cross-build-test.yml` ×2、`ci-linux-e2e.yml` quick_install 参数,全部 → `0.4.68`;
- `src/xlings.cppm:36` `kXlingsVersion` `"0.4.51"` → `"0.4.68"`,并给
  `config.cppm:34` 的无消费 re-export 补个用途或删除(倾向:doctor 输出里打印
  "bundled xlings pinned: X",让常量有单一消费点,防再度失同步);
- 低于 0.4.68 的 xlings 忽略 artifact 字段、行为不变,所以 pin 升级只是"吃到收益"
  的开关,不是正确性前提——**D1–D3 可以先合,D4 跟随下一次 release 节奏**。

### D5.(可选,#269 建议 4)项目级 `[indices]` artifact

`IndexSpec`(`src/pm/index_spec.cppm:14`)增加 `std::string artifact; std::string source;`,
mcpp.toml `[indices]` 长格式与 `config.toml [indices]`(`config.cppm:546-563`)同步增读,
`ensure_project_index_dir` 构造 `SeedRepo` 时透传。

**与 pin 的交互是这里唯一的设计点**:`rev`/`tag`/`branch` 锁定的索引,artifact 通道
只跟 latest pointer,无法表达任意 rev。规则:**凡 `rev`/`tag`/`branch` 任一非空,
不发射 artifact 字段**(等效强制 git),并在解析时对"pin + artifact 并存"给一条
warn,说明 artifact 被忽略。本地 `path` 索引同理天然不发射。

此项不阻塞主线,可作为同批次的独立 commit 或推迟。

---

## 四、兼容性矩阵

| 组合 | 行为 |
|---|---|
| 新 mcpp + xlings ≥0.4.68 | artifact 优先,git 回退(完整收益) |
| 新 mcpp + xlings <0.4.68 | artifact 字段被忽略,纯 git,行为同今天(仅吃不到收益,不出错) |
| 旧 mcpp + 新 config.toml | TOML 解析只读 url,忽略 artifact/source 行,正常 |
| 旧 mcpp + 已注入 artifact 的 .xlings.json | `package_fetcher` 手写 parser 按 key 读 name/url,多余字段无害(单测锁) |
| 用户显式 `source = "git"` | 透传后 xlings 强制 git,且 D2-5 的内存级 artifact 填充跳过 |
| 旧 URL 仅存于用户自定义条目(name ≠ mcpplibs/mcpp-index)| URL 归一按 url 匹配不看 name,同样治愈(它就是官方仓的别名条目) |

风险点与对策:

- **`package_fetcher.cppm` 手写 JSON parser**:entry 对象内新增 key 后,其
  按-brace 切分/按-key 提取逻辑必须仍取对 name/url。加一条含 artifact 字段的
  fixture 单测。
- **文本迁移的双写者间距**:双变体 `replace_all` + "已含 res base"幂等闸,
  且 unit test 覆盖 mcpp-pretty 与 xlings-compact 两种输入。
- **GitHub 重定向窗口期**:D1 合入前旧 URL 一直可用(靠重定向),合入后新旧 URL
  都直连有效,无停机窗口。

---

## 五、测试计划

单测(`tests/unit`):

1. `canonicalize_legacy_index_names`:旧 URL+旧名 → 新 URL+`mcpplibs`;
   旧 URL+新名;新 URL 无 artifact → 填默认 artifact;`source="git"` 不填;
   老条目与默认条目 URL 归一后折叠为一条(去重顺序回归)。
2. `migrate_config_toml_index_names`:旧模板文本 → URL 替换,幂等(跑两遍字节相同)。
3. `migrate_xlings_json_index_names`:pretty/compact 两种输入 × {旧URL, 新URL无artifact,
   已注入} 矩阵,幂等,subos/envs 等无关 key 逐字节保留。
4. `seed_xlings_json`:带/不带 artifact/source 的发射形态;无 artifact 条目输出与
   现行为逐字节一致(零 diff 门,沿用 CommandDialect 批次的做法)。
5. `package_fetcher` index_repos parser:含 artifact 字段的 entry 仍正确提取 name/url。

e2e(`tests/e2e`):

6. fresh init:生成的 config.toml 与 registry `.xlings.json` 含新 URL + artifact 字段。
7. 存量升级模拟:预置旧 URL、无 artifact 的 config.toml + `.xlings.json`(pretty 与
   compact 各一),跑任一命令后两文件被治愈,再跑一遍无变化。

实机验证(release 前,沿用发布闭环流程):Ubuntu 宿主(可复现 `/etc/ssl/cert.pem`
缺失环境更佳)fresh install → `mcpp index update` → 确认 `data/mcpplibs` 迁为
artifact 管理(`.git` 消失、`.xlings-index-version` 出现)且 git 不可用时同步仍成功
——issue #269 已实测过该路径,e2e 复核即可。

---

## 六、落地顺序

| 阶段 | 内容 | 依赖 |
|---|---|---|
| P1 | D1 URL 迁移(4 处 + 三个迁移钩子 + docs) | 无,可立即发 |
| P2 | D2 管线扩展 + D3 存量注入(同一 PR,D3 依赖 D1 的 URL 归一先行于同一次 migrate) | P1 |
| P3 | D4 pin → 0.4.68(CI + 常量对齐) | 随下一次 release |
| P4 | D5 项目级 `[indices]` artifact(可选) | P2 |
| P5 | CN region 对象:gitcode.com/xlings-res/mcpp-index 镜像仓部署后(mirror_res.sh 基建已有),`kMcpplibsArtifact` 升级为 region 对象发射,`SeedRepo.artifact` 相应扩为 GLOBAL/CN 两值 | P2 + 镜像仓就绪 |

P1+P2+P3 目标合入同一个版本(0.0.103),单 PR 分 commit 或两个小 PR 均可;
P5 留待 CN 镜像仓真实存在后再动,避免声明一个 404 的 base(xlings auto 模式下
artifact 失败会回退 git,不至于坏,但会白付一次探测)。

## 七、非目标

- 不改 xlings 侧任何代码(0.4.68 已全部就绪);
- 不改 mcpp-index / xlings-res 发布端(实测逐字节兼容,零改动);
- 不做 config.toml 的 artifact 文本注入(代码级默认覆盖,见 D2-5);
- `artifact` 的 region 对象形态本轮不进 TOML schema(P5 再扩)。
