# `mcpp pack` 生产侧与消费侧模型:整体架构 review

> 2026-08-20 · 承接 `2026-08-20-issue460-shared-library-runpath.md`
> 覆盖:架构完备性 / 跨平台 / 兼容性 / 易用性
> 状态:**B0 / B1 / B3 已实施,发布于 2026.8.20.1**(§7 的前三批);
> B2 / B4 / B5 / B6 仍是设计。已批准的两项(#460 P1-1 = B4,P2-1 = B3)见 §6.1 与 §7。

---

## 0. 总评

**消费侧是这套设计里完成度最高的一半,生产侧缺的是同一根骨架。**

- **消费侧**几乎没有结构性缺口。「包就是一个普通 mcpp 包」这个决定(零新 section、零新 key)
  是整个设计里最好的一步:它同时买到了老客户端可用、多种依赖形态(path/git/index)复用、
  以及「不需要为分发再写一条解析路径」。ABI tag 的 don't-care 规则、interface digest、
  中立链接通道与 GNU 拼写双写——每一条都有实测背书。真缺口只有两个(§3.2 / §3.3)。

- **生产侧**的三个缺口是**同一个形状**:应用侧做了一半、库侧一点没做,而中间**没有共同的
  骨架**去强制两边一致。#460 只是这个形状最先炸出来的那一个:

  | 缺的东西 | 应用侧 | 库侧 |
  |---|---|---|
  | 二进制格式抽象(ELF/PE/Mach-O) | 有一半(`binfmt` 只服务 PE 闭包;ELF 靠跑二进制;Mach-O 没有) | 完全没有(格式无关地 `copy_file`) |
  | 「什么不许带走」的策略点 | 硬编码成 `$ORIGIN` | 没有(#460) |
  | profile / strip 轴 | 没有 | 没有 |

- **跨平台的缺口不是「少支持一个平台」,而是「在做不到的平台上不拒绝」。** Windows 宿主
  有一段写得很好的拒绝;macOS 没有,而 macOS 走的那条路会**执行用户的程序**(§4.1)。
  这是本次 review 里优先级最高的一条,高于 #460。

---

## 1. 缺口全景(轴 × 两个 packer)

| 轴 | 应用打包 `build_and_pack` | 库打包 `build_and_pack_library` | 判断 |
|---|---|---|---|
| 路由输入 | `[targets.<n>].kind`,一处解析(`route.cppm`) | 同 | ✅ 好 |
| 格式判定 | `binfmt::identify` + `plan.targetIsPe` | 无 | ⚠️ 不对称 |
| 依赖闭包 | ELF=跑二进制;PE=读导入表;**Mach-O=无** | **无** | ⚠️ §3.4 / §4.1 |
| 可分发性重定位 | ELF 有(patchelf) | **无** | ❌ #460 |
| mode 轴(system/vendored/self-contained/static) | 四档 | 无(`--mode` 被 warning 忽略) | ✅ 合理(§6.2) |
| target 轴 | 恰好一个 | 多 leg(fat) | ✅ 合理,原因写在代码里 |
| ABI 轴 | n/a | **只有 triple**(`cfg_predicate_for` 只发 arch/os/env) | ⚠️ §3.2 |
| profile / strip | 无 | 无 | ❌ P2-1(已批准) |
| 宿主门 | Windows 拒绝 + 说理由;**macOS 不拒绝** | 无(不跑产物,但需要 relocate) | ❌ §4.1 / §4.3 |
| 归档确定性 | zip 确定(排序 + 无时间戳);tar **不确定** | 同 | ⚠️ §5.3 |
| 产物自洽性校验 | `ldd` 闭包顺带校验 | **无** | ⚠️ §3.4 |
| 老客户端兼容 | n/a | 静态 + 真实两半(e2e 252) | ✅ 很好 |

---

## 2. 生产侧:三个缺口是同一个形状

### 2.1 缺一个「二进制格式」抽象层

今天格式知识散在四处,各自用不同的方式回答同一个问题:

| 谁 | 怎么知道格式 | 怎么读 |
|---|---|---|
| `pack.cppm::run` | `plan.targetIsPe`(布尔) | PE 读导入表 / 否则跑二进制 |
| `pack.cppm::ldd_parse` | 不判定,假设 glibc | `LD_TRACE_LOADED_OBJECTS=1` |
| `binfmt.cppm` | `identify()` → `{Elf, Pe, MachO, Unknown}` | 有完整三态,**但只被 PE 路径用** |
| `library.cppm` | 不判定 | 不读 |
| `elf_runtime.cppm` | ELF only | 真正的解析器 |

`binfmt::Format` 已经是三态了,`elf_runtime` 已经是一个像样的 ELF 读取器了——缺的是把
「一个可分发二进制」抽象出来的那一层:

```
DistributableImage
  ├ format()                        Elf | MachO | Pe | NotAnImage
  ├ dependencies()                  DT_NEEDED / LC_LOAD_DYLIB / import table
  ├ search_paths()                  DT_RPATH+RUNPATH / LC_RPATH / (PE: 无)
  ├ strip_machine_local_paths()     #460 的动作,按格式实现,PE 上是 no-op
  └ id_name()                       SONAME / install_name / (PE: 无)
```

有了它,#460 的修复是「库 packer 多调一个方法」,而不是「再写一遍 patchelf 调用」;
Mach-O 的闭包也有了落点(§4.1);`is_machine_local` 有了唯一的调用方(§2.2)。

**这不是重构洁癖**:今天这四个答题者已经给出过两次不一致的答案(#460 是第一次:
应用侧剥、库侧不剥;§4.1 是第二次:PE 有宿主门、Mach-O 没有)。

### 2.2 缺一个「什么不许带走」的策略点

`runtime_search.cppm::is_machine_local` 的注释写着「`mcpp pack` asks this」,而
**没有任何 packer 调用它**(grep 只有 `prepare.cppm` 写 `resolution.json`、`doctor` 打印、
单测)。两个 packer 各自硬编码。

**修法**:relocate 一步按 `Origin` 逐条判定,而不是「全删」或「全换成 `$ORIGIN`」。
注意 #460 实测的约束——ELF 上**过滤后若结果非空,仍然会阻断继承链**,所以库包这一档的
正确答案仍然是删空 tag;但**判定过程**应该走 `is_machine_local`,这样:

- 「这条被剥掉了,因为它是 machine-local」可以打印/记录,而不是静默;
- 一条**不是** machine-local 却仍被剥掉的条目(例如 `$ORIGIN`)成为一个可以被诊断的事件
  ——它意味着这个 `.so` 依赖包里没有的兄弟库(§3.4);
- `resolution.json` 里报的 `[machine-local]` 与打包器实际剥掉的东西第一次是同一个答案。

### 2.3 缺 profile / strip 轴

`BuildOverrides` 里没有 profile,`PackConfig` 里只有
`default_mode / include / exclude / also_skip / force_bundle`。**两个 packer 发出去的都是
dev 构建**(#460 实测:未 strip、含发布者绝对源码路径)。已批准修复,设计见 §6.1。

---

## 3. 消费侧:完备的部分与两个真缺口

### 3.1 做对了的(不需要动)

- **包 = 普通 mcpp 包**。`[[runtime.artifacts]].provenance = "mcpp-pack …"` 作为标记,
  没有新 section、没有新 key ⇒ 老 mcpp 能构建、不能校验,而不是**加载失败**。这一条避开了
  「新键让整份 manifest 加载失败,已发布包永远无法采用」的老坑。
- **双写链接通道**:GNU 拼写的 `ldflags` + 方言中立的 `[target.<pred>.runtime]`,
  `merge_conditional_config` 里「中立形式**替换**库引用、但保留 `-Wl,-Bdynamic`」——
  这个「只替换它能表达的部分」的规则是对的,而且是 e2e 257 逼出来的。
- **ABI tag 的 don't-care 规则**:C surface 发短 tag,`standard` 按下界比较而不是相等。
  形状即声明,不需要 flag。
- **根目录拒绝**:`prepare.cppm:757` 拒绝把分发包本身当源码树构建,并给出可照抄的修法。
- **不重建**:`plan.cppm:1472` 跳过分发包的 shared target,理由写清楚了(否则 relink 会
  产出一个缺少所有实现单元的库)。
- **诊断质量**:`check_prebuilt` 的「最接近的拒绝」+ 逐维度 need/got,是能直接照做的。

### 3.2 缺口 A:leg 选择轴只有 triple,而 ABI tag 有四维

`cfg_predicate_for` 只发 `arch / os / env`。于是:

- 一个 fat 包**无法**同时携带同一 triple 的 gcc16 与 clang22 两条 leg——两个
  `[target.'cfg(all(arch="x86_64", os="linux", env="gnu"))'.build]` 会同时匹配,
  `-L`/`-l` 各来一份。
- 实际形态因此是「一个 ABI 一个包」:`dirName` 在单 leg 时带 abiTag,所以两次 pack 产出
  两个不同的目录/tarball。但**包名与版本相同** ⇒ 索引里只能有一个 `latest`。
- 结果:`check_prebuilt` 会给出一句很好的拒绝(「pin `[toolchain]` 到发布者用的那个」),
  但**生态层面没有出路**——gcc 用户和 clang 用户不能共用一个包名+版本。

**这是设计上的已知边界还是缺陷,需要你定调。** 两条可能的方向:

- **A1(小)**:承认边界,把它写进 docs/12 的 limitations 表(现在没有这一行),
  并让 `check_prebuilt` 的拒绝里提示「同一版本可能存在其他 ABI 的包」。
- **A2(大)**:给索引/解析加一条 ABI 轴——包身份从 `(ns, name, version)` 变成
  `(ns, name, version, abi)`,或在一个包里允许 `[target.'abi(...)']` 谓词。
  代价很大(触及 `SPEC-001` 包身份与索引 schema),**不建议现在做**,但值得记下来:
  这是「二进制生态」真正要长大时必然撞上的墙。

### 3.3 缺口 B:库产物的 digest 记录了,但从来没人校验

`manifest_emit` 为每条 leg 写 `digest = "fnv1a:…"`,`check_prebuilt` 只校验
`role == "interface"` 的那一条。于是:

- 「interface 与二进制是成对产生的、不可分别替换」这个论点**只钉了一半**;
- 一个被替换/截断的 `.so` 只会在链接或运行时报错,而不是在门口被指出来;
- 而记录一个从不被检查的字段,读代码的人会以为它被检查了(这正是 §2.2 的同一种病)。

**修法很小**:`check_prebuilt` 的第 1 步(「artifact 在不在」)顺手把 digest 也比了。
成本是每次 prepare 多读几个 `.so`/`.a`——可以只在 artifact 的 mtime/size 变化时算,
或者干脆接受(fnv1a 很快)。

### 3.4 半个缺口:`.so` 自己的依赖闭包没人看(#460 P1-3)

`run_library_pack` 只发 `targetName` 一个产物;`check_prebuilt` 只校验**声明过的**
artifact 存在。所以一个 `shared` 目标若链接了同工程的另一个 `shared` 目标,包里缺的那个
`.so` 不会被任何一侧发现。

`#460` 的修复正好把所需的一切放到 packer 手里(它已经要读 ELF 了),所以这两件事应该
**一起做**:剥路径时顺便把 `DT_NEEDED` 读出来,凡是既不是系统库、又不在包里、又不在
`[dependencies]` 里的,拒绝或警告并点名文件。

---

## 4. 跨平台

### 4.1 ⚠️ 最高优先级:macOS 宿主上 `mcpp pack`(应用)会**执行用户的程序**

**读码结论,需在 macOS 上核验。** 路径:

```
run(plan, cfg)
  → plan.targetIsPe ? run_pe            // 否
  → #if defined(_WIN32) 拒绝            // macOS 上不成立
  → #else …  ldd_parse(bundledBinary)   // ← 走到这里
        cmd = "LD_TRACE_LOADED_OBJECTS=1 '<binary>' 2>&1"
```

`LD_TRACE_LOADED_OBJECTS` 是 **glibc ld.so 的**变量。dyld 不认它(它的对应物是
`DYLD_PRINT_LIBRARIES`),所以这条命令在 macOS 上就是**把用户的程序跑起来**。之后:

- 程序退出码为 0 ⇒ 它的 stdout 被当作 ldd 输出解析 ⇒ 一条依赖都解析不出来 ⇒
  `toBundle` 为空 ⇒ `sandbox_patchelf` 在 macOS 上也不存在 ⇒ `if (!patchelf.empty())`
  整段跳过 ⇒ 产出一个只含二进制和 wrapper 的 tarball,并打印 **`Packed`**;
- 程序退出码非 0 ⇒ 报 `ldd failed on <binary>: command exited with N`,
  一个既不提 macOS 也不提 dyld 的错误;
- 程序是交互式/长驻的 ⇒ `mcpp pack` 挂住;
- 程序有副作用(写文件、发网络请求)⇒ 打包这个动作把它做了一遍。

`docs/02` 把 macOS dylib 列在「Planned Support」,所以**不支持是已知的**;
问题是**代码不拒绝**。而 Windows 那条完全同源的路径写了一段很好的拒绝:

> The dependency closure for that format is resolved by running the artifact under
> the target's own dynamic linker, which this machine has no way to do.

macOS 的情况**更糟**而不是更轻:Windows 宿主根本跑不动那个 ELF,macOS 宿主**跑得动**
那个 Mach-O,只是不会产生 trace。

**建议(P0,独立于 #460)**:

1. 立刻加一道拒绝——按 `binfmt::identify(builtBinary).format == MachO` 判定(而不是按宿主
   `__APPLE__`,理由与 `run()` 现有注释一致:这从来不是宿主的问题,是格式的问题),
   文案照 Windows 那段的形状写清楚为什么;
2. 判据必须**双向**:macOS 上跑一次断言「拒绝且信息提到 Mach-O」;同时保留一条断言
   「Linux 上同一命令仍然成功」——只钉拒绝分不清「门生效」和「pack 整个坏了」;
3. 真正的支持另开:Mach-O 的闭包是 `otool -L` / 解析 `LC_LOAD_DYLIB`,重定位是
   `install_name_tool -change` + `LC_RPATH`。这是 §2.1 那个抽象层的第一个真实客户。

> 同一段代码还有一个较小的隐患:即使在 Linux 上,`ldd_parse` 也是**执行产物**。
> 对交叉产物(比如 `--target aarch64-linux-gnu`)这同样跑不起来。今天靠什么挡住?
> 值得核验一遍——`binfmt::Ident` 里已经有「这台机器能不能跑这个文件」的语义,
> 说明这个问题被想过,但 `run()` 的分支只用了 `targetIsPe`。

### 4.2 三种格式的重定位机制不在同一层

| 格式 | 「不带走构建机」靠什么 | 在哪一层 | 守卫 |
|---|---|---|---|
| Mach-O | `-Wl,-install_name,@rpath/<name>` | **链接期**,无条件 | e2e 259,**删掉生产者构建树**后才断言 |
| ELF | 什么都没有(#460) | —— | 无(251 原地跑,永远绿) |
| PE | 不需要(无 rpath);DLL 由消费方 deploy 到 `bin/` | 消费期 | e2e 257 + wine |

三条各自都合理,但**没有一处写下「这三条是同一个问题的三种答案」**。§2.1 的抽象层就是那个
写下来的地方;在它落地之前,至少应该在 `loader_contract.cppm` 或新的 relocate 模块的头部
把这张表写进去——那个文件已经证明了「把规则写一次」在这个仓库里是有效的做法。

### 4.3 交叉打包:库侧没有宿主门,而修复所需的工具是 Linux-only

`run_library_pack` 里没有任何 `_WIN32` / 格式判定,这在今天是**对的**(它不跑产物),
而且是有用的:mcpp 支持 Windows 宿主产 Linux ELF(PR#339),所以一个 Windows CI 可以产
Linux 的库包。

但 #460 的修复会给这条路径引入一个 Linux-only 的依赖(`sandbox_patchelf` 只在
`<xim>/patchelf/*/bin/patchelf` 找)。若照抄应用侧的 `if (!patchelf.empty())`,
**非 Linux 宿主会安静地不剥**——把 #460 原样留给一半的宿主。

⇒ 这就是 #460 P0-2 选择「进程内改写 PT_DYNAMIC」的架构理由,不只是省一个依赖。

### 4.4 macOS 只能服务一个目标 ⇒ 不能产 fat 包

已在 docs/12 的 verification scope 里写明并标注 *impossible*(不是 gap)。✅ 无需处理。

---

## 5. 兼容性

### 5.1 做得好的

- **零新 section / 零新 key**,并且 e2e 252 用**两半**钉:静态(生成的 manifest 的 section
  集合是既有词汇的子集,字面列出而不是推导)+ 真实(用 `$MCPP_BOOT` 消费)。
  第二半在 CI 里因 xvm shim 而跳过——**这一点被诚实地写进了 docs/12**,值得保持。
- **`cfg()` 而不是裸 triple**:裸 triple 只在 `--target` 时匹配,生成 `cfg()` 才是
  「在每个 mcpp 上都是同一句话」。这一条是踩过坑的。

### 5.2 已发布的包无法追溯修复

#460 修好之后,**已经躺在 release / 索引里的 shared 包仍然是坏的**,而且坏在下游用户的
机器上、发布者永远看不到。所以消费侧的检测(#460 P2-2)不是锦上添花,它是这批包唯一的
出路:让使用者拿到一句能行动的话,而不是 `cannot open shared object file`。

**建议把它提到与 P0 同批**:检测的实现成本极低(读 `[[runtime.artifacts]]` 指向的 ELF,
看有没有 RPATH/RUNPATH),收益是把一个「无解的历史包袱」变成「一句可行动的诊断」。

### 5.3 确定性:zip 确定,tar 不确定,而文档只说了前者

`docs/02` 写着:

> The archive is **deterministic**: no timestamps are read, so two packs of the same
> tree are byte-identical and a published checksum means something.

这句话对 **zip 路径**(PE)成立——`run_pe` 里显式排序、`zip::write` 不读时间戳。
但 tar 路径是 `tar -czf <archive> -C <parent> <dir>`:

- 目录遍历顺序来自 readdir,不排序;
- tar 记录每个成员的 mtime / uid / gid / mode;
- gzip 头部默认写入时间戳。

⇒ **同一棵树两次打包,Linux/macOS 上得到两个不同的 sha256。** 这与「published checksum
means something」直接冲突,而索引条目正是靠 sha256 认包的。

**修法(低成本)**:`tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner`
+ `gzip -n`(或 `--format=ustar`)。注意 BSD tar(macOS 自带)不认 `--sort`,
需要按平台分支或改为自己产 tar——**这正好是「先量再改」的地方:先加一条 e2e 断言
「两次 pack 的 sha256 相同」,它今天应该是红的。**

### 5.4 未来加键的规则应该写下来

现有设计规避了「新键让老 mcpp 整份 manifest 加载失败」的坑,但那是**这次**的选择,
不是一条被写下来的规则。建议在 `manifest_emit.cppm` 头部把它升格为约束:

> 这个文件只允许输出在 `<某个版本>` 之前就已被解析的键。要表达新东西,先让它成为
> 一个**已被解析且被忽略**的键在生态里流通一个发布周期,再开始输出。

---

## 6. 易用性,以及已批准项的落地形状

### 6.1 P2-1 落地:默认 release + strip,可配置 —— **已批准**

**建议的形状**(两个 packer 共用,不是只给库包):

```toml
[pack]
profile = "release"      # 默认。可写 "dev";未来若有更多 profile 直接沿用同一词汇
strip   = true           # 默认。false = 保留符号与 debug 段
```

```
mcpp pack <target> [--profile dev|release] [--no-strip] [--debug-symbols <dir>]
```

**六个必须一起定的点**:

1. **默认值改变是一次行为变更。** 今天所有人拿到的是 dev 产物;改默认之后同一条命令产出
   不同的二进制。⇒ 输出里必须说一句(`Packing … (release, stripped)`),
   并写进 CHANGELOG 的 breaking 段。
2. **次序**:`strip` 必须排在 **relocate 之后、`file_digest(dst)` 之前**。三个动作都改
   字节,而 digest 是包的凭证。次序写死在一个地方,不要让两个 packer 各排一次。
3. **strip 用什么**:`llvm-strip`/`strip` 来自**当前 leg 的工具链**(和 `archive_tool`
   同一个来源,`dialect_for(ctx->tc)` 已经有这个抽象),**不能用宿主的 strip** ——
   交叉 leg 上宿主 strip 可能不认目标格式。若解析不到,应该像 `archiver` 那条一样**拒绝**
   而不是静默跳过(`library.cppm:303` 已经确立了这个立场)。
4. **静态库的 strip 要小心**:`strip` 一个 `.a` 会删掉符号表,库就不可链接了。
   静态归档只能 `--strip-debug`,不能 `--strip-all`。**这一条必须有单独的 e2e**,
   否则「打包成功、消费方 undefined reference」。
5. **与 `dropObjects` 的关系**:先删成员再 strip,还是反过来?建议先 `ar d` 后 strip,
   这样 strip 处理的是最终归档。两者都改归档 ⇒ 同样在 digest 之前。
6. **debug 符号不要直接丢掉**:`--debug-symbols <dir>` 或默认在 `target/dist/` 旁边留一份
   `*.debug`(`objcopy --only-keep-debug` + `--add-gnu-debuglink`),否则用户拿到崩溃栈时
   没有任何东西可用。这一条可以排在后面做,但**接口要现在留出来**,不然默认 strip 之后
   再补就是第二次行为变更。

**判据**:packed `.so`/`.a` 的 `file` 输出为 `stripped`;`strings` 里不含发布者的绝对
源码路径;**静态库 strip 后消费方仍能链接并跑出正确结果**(第 4 点);
`--no-strip` 与 `[pack] strip = false` 两条通道各钉一次。

> 与 #460 守卫的交互(重要):#460 的「无构建机路径」判据**必须写明只查动态段**。
> 如果写成「全文不含 `$MCPP_HOME`」,它在 P2-1 落地前会因 DWARF 而红,落地后又会因为
> strip 掉了而变绿——两次都不是因为 relocate。**一个测试只量一件事。**

### 6.2 `--mode` 对库目标只是 warning

```
--mode is an application-bundle depth and does not apply to the library target 'x' yet; ignoring it
```

这个处理是对的(拒绝会让 `mcpp pack --mode static` 在混合工程里变得难用),但那个
**「yet」**暗示以后会有。值得现在就想清楚:**库包需要 mode 轴吗?**

我的判断:**不需要 mode,但需要一条「第三方 `.so` 怎么办」的答案。** 今天 docs/12 写着
「bundling dependencies into the package ❌ declare them instead」,这是一个清楚的立场,
而且和 `[dependencies]` 的传递是自洽的。⇒ 建议把 warning 里的「yet」去掉,改成指向那条
立场的一句话。措辞暗示的路线图,读的人会当承诺。

### 6.3 消费者拿到 127 时的可操作性

这是当前整条链路里**唯一一处诊断质量明显低于本仓库水准**的地方,而且正好是终端用户所在的
位置:`mcpp` 在生产侧和链接侧的每一条错误都能照做,而运行期失败落到的是 ld.so 的
`cannot open shared object file`——它不提包名、不提 mcpp、不提该做什么。

**建议**:`mcpp run` / `mcpp test` 在子进程以 127 退出且 stderr 匹配
`cannot open shared object file` 时,接管这条信息:用已有的 `resolve_runtime_closure`
跑一遍,把「哪个对象需要它 / 它的搜索路径是什么 / 为什么继承没生效」打出来。
P1-1(闭包模型学会 RUNPATH 抑制规则)落地后,这段诊断才**说得对**——所以它是 P1-1 的
自然下游,建议排在同一批。

### 6.4 缺一个「验证这个包」的入口

今天要回答「我发出去的包能用吗」,只有「换一台机器试试」。建议加
`mcpp pack --verify <dir|tarball>`(或 `mcpp doctor --package <dir>`):

- 每个 ELF 的 loader tag 契约 + 有无 machine-local 路径;
- `[[runtime.artifacts]]` 指向的文件都在、digest 都对(§3.3);
- `.so` 的 DT_NEEDED 闭包被包 + `[dependencies]` 覆盖(§3.4);
- interface digest 与 `interface/` 一致。

它同时是 §5.2 那批**存量坏包**的检测入口,也是 e2e 守卫可以直接调用的东西——
守卫用产品自己的检查,比守卫各写一份 python 解析器更不容易腐坏。

---

## 7. 建议的落地顺序

| 批次 | 内容 | 理由 |
|---|---|---|
| **B0** | §4.1 macOS 宿主拒绝(格式判定,不是宿主判定)+ 双向 e2e | 唯一一条「会执行用户程序」的缺陷,且修复极小 |
| **B1** | #460 P0-1/P0-2/P0-3:relocate(进程内 ELF 写)+ digest 次序 + 别名从 `dst` 拷 + 双向守卫 | 用户已报;不带守卫的修复会再次隐身 |
| **B2** | §5.2 / #460 P2-2:消费侧与 `doctor` 检测存量坏包 | 已发布的包唯一的出路,成本极低 |
| **B3** | P2-1 release + strip + 可配置(§6.1 六点)| 已批准;必须排在 B1 之后,否则两个改动在同一个判据上互相干扰 |
| **B4** | P1-1 闭包模型学会 RUNPATH 抑制(先诊断、跑全量 e2e 对照、再考虑升级为 blocking)+ §6.3 的 127 诊断 | 已批准;下游有真实收益,但有把绿判红的风险 |
| **B5** | §2.1 `DistributableImage` 抽象 + §2.2 `is_machine_local` 收敛 + §3.3 digest 校验 + §3.4 闭包校验 | 结构性;做完之后 Mach-O 的真实支持才有落点 |
| **B6** | §5.3 归档确定性(先加红测试)、§6.4 `--verify`、§6.2 措辞 | 收尾 |
| **暂不做** | §3.2 A2(包身份加 ABI 轴) | 触及 SPEC-001 与索引 schema;先按 A1 把边界写进文档 |

---

## 8. 判据清单(review 时对照)

| # | 主张 | 证据等级 |
|---|---|---|
| 1 | macOS 宿主上应用打包走 glibc 路径并执行产物 | **已按格式拒绝(2026.8.20.1)**。旁证:此前 macOS 上 249/250 是绿的,而它们检查的正是那次「运行用户程序」产出的 bundle —— 加上拒绝后这两条立刻变红 | **读码 + macOS CI 实测** |
| 2 | e2e 的 `pack` 能力 = ELF + patchelf ⇒ 应用打包在 macOS 上一条 e2e 都不跑 | **读码**(`tests/e2e/run_all.sh:43-76` 的 `Linux)` / `Darwin)` 分支) |
| 3 | `binfmt::Format` 已是三态但只服务 PE 路径 | 读码 |
| 4 | `is_machine_local` 无 packer 调用方 | **grep** |
| 5 | leg 选择只有 arch/os/env 三维 | 读码(`manifest_emit.cppm:142-161`) |
| 6 | 库 leg 的 digest 记录但从不校验 | 读码(`prebuilt.cppm:137-139` 只匹配 `role == "interface"`) |
| 7 | tar 路径不确定、zip 路径确定,而文档只声明了确定性 | 读码(`pack.cppm:797` vs `:917`)+ docs/02:306 |
| 8 | `PackConfig` 无 profile / strip 键 | 读码(`types.cppm:791-800`) |
| 9 | 静态库不能 `--strip-all`(会删符号表) | **已实测**:`ld: 归档没有索引;run ranlib`,而 `--strip-debug` 后 2988→1244 字节且消费方链接并跑通(e2e 265 两侧都钉) |
| 10 | 老客户端兼容用静态+真实两半钉,且 CI 上只跑了静态那半 | 读码 + docs/12 自述 |
| 11 | `--mode` 对库目标 warning 且措辞含 "yet" | 读码(`cmd_publish.cppm:78-82`) |
