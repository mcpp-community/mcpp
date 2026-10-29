# SPEC-013:平台

| 项 | 值 |
|---|---|
| 规范编号 | SPEC-013 |
| 标题 | 平台:一类目标的可复用配置,其声明、选择、合并、对依赖图的补全、链接与封装、运行器与载体 |
| 状态 | 评审中 v1.3 |
| 版本 | 1.3 |
| 最后修改 | 2026-10-30 |
| 对应实现 | mcpp >= 2026.10.27.1;§10 的执行宿主与 §11 的载体架构 mcpp >= 2026.10.29.1;`runner.hosts`、载体列表、`descriptor_keys` 与 `universal` 的弃用 mcpp >= 2026.10.30.1;逐条标注 |
| 相关设计文档 | `.agents/docs/2026-10-28-platforms-design.md`、`.agents/docs/2026-10-28-platforms-part2-packaging-and-distribution-design.md`、`.agents/docs/2026-10-29-openkal-run-test-pack-design.md` |
| 相关 issue | mcpp#846、mcpp#847、mcpp#848 |
| 相关规范 | [SPEC-002](target-side.md)(目标侧的层)、[SPEC-004](manifest-semantics.md)(条件化形状)、[SPEC-006](toolchain-management.md)(工具链选择)、[SPEC-007](build-plugins.md)(构建程序与协议)、[SPEC-011](concurrency.md)(原子发布)、[SPEC-014](packaging.md)(打包) |
| 使用文档 | [35 —— 平台](../zh/35-platforms.md) |

用语按 RFC 2119:**必须 / 禁止**为强制,**应当**为建议,**可以**为可选。

## 0. 范围与词汇

本规范规定**平台**:一类目标的一份可复用、带版本、可发布的配置。平台由一个包在自己的清单中以
`[platform]` 声明,由引擎选择并合并,其内容进入工具链的决定、依赖图、编译与链接的命令、产物的
命名与封装、运行器与打包。引擎不持有任何一个平台的知识;它提供本规范规定的机制。

| 词 | 含义 |
|---|---|
| 平台包 | 清单含 `[platform]` 段的包 |
| 目标图 | 为 `--target` 构建的包 |
| 宿主图 | 为构建机器构建的程序:构建程序(`build.mcpp`、平台插件)与本机工具 |
| 层 | `kernel-abi`、`c-abi`、`c++-abi`、`compiler-runtime`(SPEC-002) |
| 工具 | 在构建机器上运行的程序(例:`okld`) |
| 封装(seal) | 链接之后、发布之前对产物本身执行的步骤 |
| 运行器 | `mcpp run`、`mcpp test` 执行产物的方式;`runners.<name>` 为命名的设备动作 |
| 载体 | 把依赖加载器的产物与加载器合成为一个本机程序的宿主程序包(M3) |
| 平台陈述 | 一份平台数据(`Statement`):本规范 §1 的全部字段,每个值带来源 |

## 1. `[platform]` 段

一个包的清单含 `[platform]` 段时,该包是平台包。`[platform]` **必须**含 `targets`;否则清单被拒绝。
**已实现**

```toml
[platform]
targets   = ["<triple>", ...]
toolchain = "<spec>"
sysroot   = "<spec>" | { path = "<dir>", provides = [...] }
runner    = ["<prog>", "{}"] | { cmd = [...], host = "same-arch" | "any", hosts = ["<os>-<arch>", ...], exclusive = <bool> }
runners   = { <name> = ["<prog>", ...] }
carrier   = { package = "<spec>", section = "<name>", main = "<file>", hosts = [...], build = "row" } | [ {...}, ... ]
descriptor_keys = ["<key>", ...]
plugin    = "build.mcpp"

[platform.layers]   <layer> = <package> | false
[platform.tools]    <name>  = <package> | { program = "<name>" }
[platform.build]    cflags / cxxflags / asmflags / ldflags = [...]
[platform.build.'cfg(<predicate>)']   同上
[platform.link]     driver, flags, start, script, inputs, exe, shared, seal
[platform.versions] <package> = "<version>"
[platform.hosted]   targets, toolchain, layers
```

### 1.1 键

| 键 | 类型 | 语义 |
|---|---|---|
| `targets` | 非空字符串数组 | 平台服务的目标三元组 |
| `toolchain` | 字符串 | 约定的工具链规格(SPEC-006),例 `llvm@23.1.3` |
| `sysroot` | 字符串或 `{ path, provides }` | 预构建的目标基,写法同 `[target.<triple>].sysroot`;`path` 相对于平台包 |
| `runner` | 字符串数组或表 | §10 |
| `runners` | `<name> = [argv]` | 命名的设备动作,每个 argv 非空 |
| `carrier` | 表或表的数组 | §11;每项的 `package`、`section`、`main` 三键都**必须**给出;`hosts` 为该项服务的 `<os>-<arch>`(省略:服务每一对);`build` 为载体程序的构建模型,缺省 `"row"`(每个 `<os>-<arch>` 一个程序,经平台的 hosted 安排构建),本 mcpp 不认识的模型的项被跳过并警告。**mcpp >= 2026.10.30.1**;更早的 mcpp 读到数组即拒绝,写数组的平台**必须**声明相应的 `mcpp` 下限 |
| `descriptor_keys` | 非空字符串数组 | 平台目标读取描述符的哪些按平台块及其顺序(第一个存在的块被读取),取值为 `linux`、`macosx`、`windows`、`android`、`ios`、`emscripten`、`openkal`、`none`;未声明时为目标行自己的键(§14)。**mcpp >= 2026.10.30.1** |
| `plugin` | 字符串 | 只能为 `"build.mcpp"`:平台包自身的构建程序(SPEC-007)在平台阶段运行(§12);其他值拒绝 |
| `universal` | 布尔 | 已弃用,不起作用(docs/51 的 `platform-universal`):打包的腿只是命令命名的目标(SPEC-014 §1.4);读到时写出提示。**mcpp >= 2026.10.30.1** |
| `layers` | 表 | §7.2;键只能是 §0 的四个层名 |
| `tools` | 表 | §7.3 |
| `build` | 表 | 四个标志列表,以及键形如 `cfg(...)` 的子表(同四个键) |
| `link` | 表 | §9 |
| `versions` | `<package> = "<version>"` | 版本组(§7.5) |
| `hosted` | 表 | 宿主三元组选用本平台时适用的部分(§3.4);键只能是 `targets`、`toolchain`、`layers` |

**包引用。** `layers`、`tools`、`carrier.package` 与选择平台的值都是包引用:字符串
`"[<namespace>.]<name>@<version>"`,或表。表的键为 `name`、`version`、`path`、`git` 与
`rev` / `tag` / `branch` 之一、`features`、`default-features`;**必须**恰好给出 `version`、`path`、
`git` 之一;`version` 与 `name` 同时出现;`path` 相对于写它的文件;`git` 的引用同时给出 `name`;`path` 的引用**可以**
省略 `name`,此时由该目录中包自己的清单命名。`false` 只在 `layers` 中有效;`{ program = "<name>" }` 只在
`tools` 中有效,且不带其他键。**已实现**

**argv 约定。** argv 中的 `{}` 为产物;程序以名字书写,按"图中为本机构建的程序 → 声明的载荷 →
`PATH`"查找。**已实现**

**错误。** 类型错误是拒绝,消息写出键;`[platform]` 与 `[platform.hosted]`、`[platform.link]` 等子表中
引擎不认识的键记为警告(`--strict` 下拒绝),使为更新的引擎写的平台包仍能被读取;平台包以
`[package] mcpp = ">=<release>"` 声明下限(SPEC-007 R9.8)。**已实现**

### 1.2 只能声明的键与可由代码陈述的键

`targets`、`toolchain`、`sysroot`、`layers`、`tools`、`carrier`、`versions`、`hosted` 在解析工具链与依赖图
时需要,那时没有构建程序可以运行,因此**只能**在清单中声明。`build`、`link`、`runner`、`runners` **可以**
由平台插件或构建程序陈述(§12)。**已实现**

## 2. 三种写法与层次

同一组字段有三种同义的写法,解析为同一结构,每个值带来源:

| 写法 | 位置 | 层次 |
|---|---|---|
| 约定 | 平台包的 `[platform]` | Platform |
| 陈述 | 平台插件与构建程序的协议 17 指令(§12) | Program |
| 覆盖 | `config.toml` 的 `[target.<selector>]`、`[host]` | Config |
| 覆盖 | 工程 `mcpp.toml` 的 `[target.<selector>]`、`[host]` | Manifest |
| 覆盖 | 环境变量 `MCPP_TARGET_<T>_PLATFORM`、`MCPP_TARGET_<T>_RUNNER`、`MCPP_HOST_PLATFORM` | Env |
| 覆盖 | `--platform`、`--host-platform` | Flag |

层次按 Platform < Program < Config < Manifest < Env < Flag 排列,后者按字段替换前者(§6)。另有两个
来源:`built-in`(目标表或宿主约定)与 `graph`(依赖图中的包已经提供,§7.2)。**已实现**

`[target.<selector>]` 中与平台对应的键为 `platform`、`layers`、`tools`、`carrier`、`link`,以及表原有的
`runner`、`runners`、`toolchain`、`sysroot`;`[host]` 只接受 `platform`。覆盖的键名与 `[platform]` 相同。
`<T>` 为三元组大写、非字母数字字符改为 `_` 的形式(例:`MCPP_TARGET_X86_64_OPENKAL_PLATFORM`)。**已实现**

## 3. 目标图的选择

### 3.1 顺序

目标图的平台由第一个给出答案的来源决定:

1. `--platform <值>`,即本次调用的 `MCPP_PLATFORM`;
2. `MCPP_TARGET_<T>_PLATFORM`;
3. 工程的 `[target.<selector>] platform`(工作空间层先于根包,§13);
4. `config.toml` 的 `[target.<selector>] platform`;
5. 目标表的平台列(§14);
6. 无:宿主目标的内置约定,或无平台。

第 3、4 步在命中的表中取具体程度最高者(SPEC-004 §3.1.1)。**已实现**

### 3.2 值

| 值 | 含义 |
|---|---|
| `none` | 不使用平台 |
| `"<name>@<version>"`、包引用表 | 一个平台包,按普通依赖解析、获取与锁定 |
| `path:<dir>`,或写成路径的值(以 `/`、`./`、`../`、`~` 开头,Windows 盘符,或含路径分隔符且不含 `@`) | 一个目录中的平台包;命令行与环境变量中的相对路径相对于 mcpp 的启动目录 |

一个目录的清单不可读时,选择**必须**以该清单的错误失败。**已实现**

### 3.3 `platform` 的位置限制

`platform` **禁止**写在询问平台的选择器下(`[target.'cfg(platform = "...")'] platform`),因为该选择器的
答案由 `platform` 决定;清单被拒绝,消息给出可用的选择器(三元组,或 `os`、`arch`、`env` 的 `cfg`)。
**已实现**

### 3.4 宿主三元组选用平台

被选中的平台不服务所求目标时:

- 目标在 `targets` 中:适用整个平台。
- 目标是宿主三元组,平台有 `[platform.hosted]`,且 `hosted.targets` 未写或含该三元组:只适用
  `hosted` 的 `toolchain` 与 `layers`,以及 `versions`;报告与 JSON 标为 `hosted`。
- `hosted.targets` 不含该三元组:拒绝(`platform-target-unserved`),消息列出 `hosted.targets`,并在其中
  有同系统同架构的三元组时给出改用它的命令。
- 平台无 `[platform.hosted]`:拒绝(`platform-target-unserved`),消息列出 `targets`。

**已实现**

## 4. 宿主图的平台

### 4.1 选择

宿主图的平台由第一个给出答案的来源决定:`--host-platform` / `MCPP_HOST_PLATFORM`;工程的 `[host] platform`;
`config.toml` 的 `[host] platform`;否则宿主约定。选择只在最外层的准备中进行。**已实现**

### 4.2 模型

宿主图选用平台 P 时,构建程序与其库编译为 `<本机架构>-<P 的目标>` 的产物(P 的 `targets` 中与本机架构
相同的那个三元组),由 P 的运行器运行;构建程序的身份含 P 的身份。报告多一行
`Host platform <name> <version> (<来源>): build programs are <triple> images run by <runner>`。**已实现**

### 4.3 自举边界

一个平台的 `tools` 与运行器所需的程序,在任何图中都**必须**按宿主约定构建:运行器不能由它自己运行。
嵌套的准备(工具的子构建、宿主目标的子构建、构建程序图)不选择平台,除非调用者为它指定。因此不形成
循环:P 的工具属于原生宿主图,构建程序属于 P 的宿主图。**已实现**

## 5. `cfg(platform = ...)`

`cfg()` 谓词增加键 `platform`。它匹配所选平台的名字(平台包短名去掉结尾的 `-platform`,`openkal-platform`
的名字是 `openkal`)或平台包的完整短名。它在具体程度中计一个分量,与 `arch`、`env` 相同,低于 `os`(计
`os` 与族两个分量)与三元组;`all(...)` 中的分量相加(SPEC-004 §3.1.1)。无平台时,`cfg(platform = ...)`
不匹配。**已实现**

## 6. 合并

一个图的平台陈述按"平台 → 平台插件与构建程序 → `config.toml` → 工程 → 环境变量"的顺序合并;每一层只改
它写出的字段:

| 字段 | 规则 |
|---|---|
| `build` 的四个列表、`link.flags` | 拼接,平台的在前;使用者只能追加,不能删除平台的约定 |
| `build` 的 `cfg(...)` 子表 | 追加 |
| `link.inputs`、`link.seal` | 整体替换 |
| `layers`、`tools`、`runners`、`versions` | 按键替换 |
| `runner` | `cmd` 替换;`host`、`exclusive` 各自独立替换,只写 argv 的覆盖保留平台的 `host` 与 `exclusive` |
| 其余 | 按字段替换 |

`MCPP_TARGET_<T>_RUNNER` 以空白分隔的 argv 替换 `runner.cmd`;值为 `none` 时清除运行器。**已实现**

## 7. 依赖图的补全

### 7.1 平台包是根的依赖

被选中的平台包作为根的第一个依赖进入依赖图,按普通包解析、获取、缓存与锁定;其库目标可被 `import`。
无 `plugin` 且无源文件的平台包不编译。被选中的包没有 `[platform]` 段时拒绝(`platform-not-a-platform`)。
**已实现**

### 7.2 层

第一轮遍历之后,对 `layers` 中的每一层:

1. 值为 `false`:平台不为该层提供包;报告为 `none`。
2. 图中已有包以 `provides` 提供该层:平台让位;该包由平台加入时报告为 `platform`,否则为 `graph`。
   判断读 `provides`,不比较包名。
3. 否则:该包作为根的依赖加入(来源 `the platform's <layer>`),一个包提供的多层只加入一次。

加入的包再次遍历;补全最多进行三轮。一个包提供多个层时,关闭其中一层不移除该包:例如 `c++-abi = false`
而 `compiler-runtime` 仍由同一包提供时,该包的 `provides` 使 `c++-abi` 由图提供。**已实现**

### 7.3 工具

`tools` 中的每个包引用作为根的构建依赖加入,带 `tools = [<names>]`,为本机构建;同一包的多个工具只构建一次。
根已经以构建依赖请求同名工具时让位;`{ program = "<name>" }` 不构建,在声明的载荷与 `PATH` 中查找。
**已实现**

### 7.4 工作空间成员

工作空间的计划中,平台加入的层的包同时成为每个被选中成员的依赖边,使成员的链接闭包含有这些层。**已实现**

### 7.5 版本组

`versions` 中的包以不同于组中版本的版本出现在图中时拒绝(`platform-version-group`),消息写出该包、两个版本、
请求链与两条出路:选择携带所需版本的平台版本,或在请求处把该包写成组中的版本。只为本机构建的包(工具)
与以 `path`、`git` 取得的包不受版本组约束。检查在每一轮补全之前与最后一轮之后进行,使工程固定的一个版本以
版本组的冲突报告,而不是以平台加入的包遇到第二个版本报告。**已实现**

### 7.6 多版本改名的边界

两个版本的同一个包以改名并存只改 C++ 模块名。导出头文件目录的包与以 `provides` 提供 `mcpp:` 层的包
**禁止**以改名并存:两个版本同时被请求时拒绝,消息列出两个请求者,有平台时另给出换平台版本的出路。
**已实现**

## 8. 工具链

平台的 `toolchain` 位于目标表钉住的工具链的位置:它替换默认值,**禁止**替换工程或本机为该目标写出的工具链
(`[target.<selector>] toolchain`、`[toolchain]`、`--toolchain`)。图中提供 `kernel-abi` 的包不取消平台的
工具链。工具链报告行说明它是平台的,以及它替换了哪个默认值与覆盖它的键。**已实现**

平台写 `sysroot` 时,目标基在依赖图存在之前决定;平台包不在本地时,第一遍准备获取它,第二遍以它的
`sysroot` 重新决定目标基。**已实现**

## 9. 编译、链接、命名与封装

### 9.1 编译

平台陈述的 `build` 标志进入目标图每个翻译单元的命令行(`cflags` 给 C,`cxxflags` 给 C++,`asmflags` 给汇编,
`ldflags` 给链接,`driver = "lld"` 时位于 `link.flags` 之后、直接给 `ld.lld`),`cfg(...)` 子表按目标求值后追加在无条件的标志之后。**已实现**

### 9.2 链接

| 键 | 语义 |
|---|---|
| `driver` | `"compiler"`(编译器驱动链接,缺省)或 `"lld"`(`ld.lld` 直接链接) |
| `flags` | `driver = "lld"` 时给 `ld.lld` 的参数,位于其他链接参数之前 |
| `start` | 入口符号(SPEC-004 §14);入口未定义是链接错误 |
| `script` | 平台包中的链接脚本,以 `-T` 给出 |
| `inputs` | `{ file = "<包内路径>" }`,或 `{ from = [argv], as = "<文件名>" }`:运行程序,把其标准输出保存为 `as`;以 `.S`、`.s` 结尾时先汇编,否则原样参与链接 |
| `exe` | 可执行产物的名字模式,**必须**以 `{name}` 开头 |
| `shared` | 共享产物的名字模式,`{name}` 恰好一次,不含目录 |
| `seal` | 封装步骤,每步一个 argv(§9.3) |

`driver = "lld"` 时引擎在编译器旁依次查找 `ld.lld`、`lld`、`lld-link`(Windows 载荷只带 `lld-link`);都没有时拒绝
(`lld-required-absent`),消息写出平台的工具链。程序名不是 `ld.lld` 时引擎加 `-flavor gnu`。链接或封装命名的程序既非图中为本机构建、也不在本机可找到时拒绝
(`platform-tool-absent`),消息写出负责构建它的包,或写出 `[target.'cfg(platform = "<name>")'.tools]` 的写法。
**已实现**

### 9.3 封装

`seal` 的步骤在链接器写出的临时产物上依次执行,`{}` 为该临时产物;参数 `{needed}` 展开为该产物所需的共享产物,
每个一个参数,当它前面的参数是取值的选项(以 `-` 开头、不含 `=` 与 `{}`)时,该选项在每个产物前重复,
不需要任何共享产物时二者都省略。全部步骤成功后产物原子发布(SPEC-011 §3);任一步失败则不发布,保留上一次
发布的产物,错误给出该步的 argv 与输出。步骤属于链接边的命令行,改变步骤即重新链接。**已实现**

## 10. 运行器

`runner` 的数组形式为 argv;表形式:

| 键 | 语义 |
|---|---|
| `cmd` | argv,非空;`{}` 为产物 |
| `host` | `"same-arch"`:产物只在其架构的机器上运行;`"any"`:运行器可以从任何机器到达产物(模拟器、探针) |
| `exclusive` | 为真时该目标的运行不重叠,`mcpp test` 串行运行 |
| `hosts` | 运行器程序存在的 `<os>-<arch>`(`os` 为 `linux`、`macos`、`windows`):需要该运行器的包能在哪里运行(SPEC-014 §4.4 的 `runs_on.with`)。`host = "any"` 时与产物的架构无关。**mcpp >= 2026.10.30.1** |

`host = "same-arch"` 而产物的架构不是本机架构时,`mcpp run` 与 `mcpp test` 拒绝运行,消息给出
`[target.<triple>] run_under` 的写法;`run_under` 存在或给出 `--no-runner` 时不拒绝。运行器的合并顺序与 §6
相同;`mcpp run` 与 `mcpp test` 读合并后的值。**已实现**

运行器的程序是 `cmd` 首项所命名的程序;首项是平台的工具(§7.3)时,为构建为本机定位的该工具。`host = "same-arch"`
的运行器在产物架构的机器上运行,因此产物的架构不是本机架构、且给出了执行宿主(`run_under`,含
`MCPP_TARGET_<三元组>_RUN_UNDER`;SPEC-002 §6)而未给 `--no-runner` 时,首项为平台工具的运行器的程序**必须**是产物架构的:
平台的载体以运行器形式(§11,不携带任何内容)为本机系统与产物架构构建的程序,构建于
`target/carrier-pack/<首项>-<os>-<arch>/`,与 `self-contained` 的树放在 `libexec/<首项>/<os>-<arch>/` 中的程序相同。
平台未声明 `carrier`,或本 mcpp 不为该系统与架构构建程序(SPEC-014 §4.4)时,运行**必须**被拒绝并写出原因,**禁止**改用
本机的工具。`host = "any"` 的运行器从本机到达产物,其程序是本机的。`mcpp test` 为一次构建构建一次该程序,不运行测试时
(`--no-run`、已确定的拒绝)不构建。**已实现**(2026.10.29.1)

例:x86_64 Linux 上以用户态模拟器作为执行宿主运行 `aarch64-openkal` 的映像:

```toml
[target.aarch64-openkal]
run_under = ["qemu-aarch64"]
```

`mcpp run --target aarch64-openkal` 执行 `qemu-aarch64 <aarch64 的运行器> <映像> <参数>`。

`mcpp test` 的人类输出在第一个因拒绝而未运行的测试之前以警告写出一次完整的原因(每次构建一次),摘要行写出其第一行。
**已实现**(2026.10.29.1)

## 11. 载体(M3)

对一个依赖加载器的产物,`mcpp pack` 在两处生成一个系统程序的工程(`target/carrier-pack/<name>-<os>-<arch>/`):
`self-contained` 的树的运行器(不携带任何内容,SPEC-014 §4.4)与映像的单文件(该程序携带产物,SPEC-014 §5.2);
`mcpp run` 与 `mcpp test` 为执行宿主构建另一架构的运行器(§10)时亦然。携带映像的程序(M3)只属于单文件
(2026.10.30.1)。单文件的程序的架构为该腿的架构,系统按 `--systems`(缺省为本机系统)。工程依赖 `carrier.package`;
入口是平台包中的 `carrier.main`;一个汇编文件以 `.incbin` 把携带的字节放进节 `carrier.section`(ELF 写作该名并带
`"a"` 标志,COFF 写作该名并带 `"dr"` 标志,Mach-O 写作 `__<大写名>,__carried`),给出符号 `mcpp_carried_begin`、`mcpp_carried_end`、`mcpp_carried_name`
(入口的名字)与 `mcpp_carried_digest`(字符串)。

携带的字节**必须**是以下之一,载体的入口按内容区分:

| 携带 | 判别 | 载体的行为 |
|---|---|---|
| 无(`mcpp_carried_begin == mcpp_carried_end`) | 长度为 0 | 运行器形式:运行第一个参数所指的映像,其余参数传给它,如同平台的运行器程序 |
| 一个映像 | 产物本身 | 从内存运行该映像,映像以程序自己的名字为名 |
| 一组映像 | 以 `PK\x03\x04` 开头的 stored zip,各条目数据按 64 KiB 对齐 | 从内存运行名为 `mcpp_carried_name` 的映像,映像之间按名字相互解析 |

程序按其系统的约定构建(自举边界),不使用平台:Linux 上为 `<arch>-linux-musl` 行的静态程序,该行的工具链被
显式写出;macOS 上为 `<arch>-macos`、Windows 上为 `<arch>-windows-gnu` 行,系统与架构同本机时取本机的行,只依赖
系统库(映像的单文件在 Windows 上以 `system` 模式打包)。引擎不含任何加载器的调用。平台未声明 `carrier` 时,
需要载体的打包被拒绝并写出该键;本 mcpp 不构建程序的行(SPEC-014 §4.4)上的载体在生成工程之前被拒绝,写出系统、架构与行
(2026.10.29.1)。

**载体项的选择(mcpp >= 2026.10.30.1)。** 为 `<os>-<arch>` 构建载体程序时(上述三处与 §10 的执行宿主),所用的是
`hosts` 列出该对的第一项,否则为第一个未写 `hosts` 的项;两者都没有时,如同平台未声明 `carrier`,打包或运行被拒绝,写出
该对。`build` 不是 `"row"` 的项留给将来的构建模型(一个文件服务多个系统与架构的载体,mcpp#849),本 mcpp 读到时跳过并警告。

## 12. 平台阶段与协议 17

### 12.1 平台阶段

平台包的 `plugin` 在**平台阶段**运行:`MCPP_PHASE` 为 `platform`,`mcpp::phase()` 返回 `"platform"`。该阶段只能
陈述编译与链接标志、`link_script`、`link_start` 与协议 17 的指令、`runner` 与命名运行器、`run_exclusive`、平台
提供的包格式(`pack-format` 与 `role = "artifact"` 的 action,SPEC-014 §6),以及重跑条件、警告与诊断;其他指令
(含其他角色的 action)被拒绝(`platform-phase-directive`),消息点名第一条不属于该阶段的指令。陈述工具链在平台
阶段被拒绝。**已实现**

### 12.2 协议 17 的指令

| `mcpp.core` 函数 | 指令 | 字段 |
|---|---|---|
| `link_driver(d)` | `mcpp:link-driver=<lld\|compiler>` | `link.driver` |
| `link_input(path)` | `mcpp:link-input=file:<path>` | `link.inputs` 的 `file` |
| `link_input_from(as, token)` | `mcpp:link-input=from:<as>:<token>` | `link.inputs` 的 `from` 与 `as`,同一 `as` 的 token 依次组成 argv |
| `artifact_name(kind, pattern)` | `mcpp:artifact-name=<exe\|shared>:<pattern>` | `link.exe`、`link.shared` |
| `seal(step, token)` | `mcpp:seal=<step>:<token>` | `link.seal`,按步号分组 |
| `run_host(h)` | `mcpp:run-host=<same-arch\|any>` | `runner.host` |

协议 17 的指令也可以由任何构建程序在构建阶段发出(板级支持包的写法),在 Program 层次合并。格式错误的
指令被拒绝,消息写出合法形式。**已实现**

## 13. 工作空间

`[target.<selector>]` 的平台键与 `[host]` 是根位置的键,从计划的根读取。工作空间层的
`[workspace.target.<selector>]` 先于成员自己的表合并:同一选择器的两张表,成员的字段胜出;不同选择器之间
按具体度,与 `[target.<selector>]` 其他键的合并相同。`[host]` 不是工作空间层的表。虚拟根携带第一个被选中
成员的平台键与 `[host]`;根位置的比较键包含适用的平台表与 `[host]`,使二者不同的成员不共用一个计划。
**已实现**

## 14. 内置平台与目标表

宿主目标(Linux、macOS、Windows、Android、iOS、Emscripten)的约定留在引擎中,无平台包。目标表有平台列:
`x86_64-openkal`、`aarch64-openkal` 的值为 `mcpplibs.openkal-platform@<version>`,即这两个目标的内置支持;
选择来源报告为 `built-in`。项目可以不等引擎更新,以 `[target.<selector>] platform` 选择另一个版本。
**已实现**

**描述符键(mcpp >= 2026.10.30.1)。** 每个目标行声明它读取依赖描述符的哪些按平台块及其顺序,第一个存在的块被读取;
`xpm` 下的版本与 `mcpp` 段的按平台表都按此读取,构建机不参与:

| 目标 | 依次读取的块 |
|---|---|
| Linux(含 Android 与 musl)、macOS、Windows | `linux`、`macosx`、`windows` 分别 |
| `wasm32-emscripten` | `emscripten`,其次 `linux` |
| `<arch>-openkal` | `openkal`,其次 `linux`;平台的 `descriptor_keys`(§1.1)取代之 |
| iOS | `ios`,其次 `macosx` |
| 裸机(`os = none`) | `none` |

平台在首次准备时尚未读到(须先取得平台包)而又声明了 `descriptor_keys` 时,准备以读到的平台重新开始,与 `sysroot`
相同。宿主工具(宿主图)按宿主的键安装。此前,不是三者之一的目标读构建机的块(mcpp#851)。

ok 映像是 x86_64 与 aarch64 上位置无关的 ELF,只导入 `kal_*`,运行时需要加载器发布的可执行内存;okb 只装 ok 映像。
openkal-platform 的 `targets` 因此不含 WebAssembly。Web 经 `wasm32-emscripten` 目标(宿主目标,无平台包)到达,打包为
页面经 `dist-web` 提供者;以 openkal API 写的代码经 openkal 的 Emscripten 实现(openkal-emscripten)到达 Web。

## 15. 锁、身份与报告

### 15.1 锁

平台加入的包,以及只经由它们到达的包(层的实现、工具与插件的库),在 `mcpp.lock` 中带 `origin = "platform"`(当解析
它的每个配置都这样看时);无 `origin` 的条目属于工程。每个条目以 `targets` 写出解析它的配置(目标三元组,排序);原生
配置——不带 `--target` 的构建解析到的目标,即 `[build] target`、本机的缺省目标或本机三元组——独有的条目不写
`targets`,读取时无 `targets` 的条目属于写锁的机器的原生配置。一次配置把自己的目标加入它解析到的条目、从没有解析到的
条目中去掉,不再被任何配置解析的条目被删除;一个条目的版本改变时其 `targets` 只剩这次的配置;条目按身份排序。为本机的
构建与为 `x86_64-openkal` 的构建、打包因此不互相改写锁。**已实现**

### 15.2 身份

平台包与它加入的包是依赖图的成员,进入配置指纹的依赖部分;换平台或平台版本即换构建目录。平台的身份为
`<package>@<version>`,`path`、`git` 来源另带来源;宿主图平台的身份进入构建程序的身份。**已实现**

### 15.3 报告

目标报告中,平台加入的层的来源写作 `platform`,图已提供的写作 `graph`。层之后为平台块:

```
    Platform openkal-platform 0.17.3 (built-in)
             tools             okld, okrun (openkal-tools@0.1.3, platform)
             runner            okrun (platform)
             link              lld, start ok_main (platform)
```

来源取值:`built-in`、`platform`、`build-program`、`config.toml:<行>`、`mcpp.toml:<行>`、环境变量名、
`--platform`、`graph`。宿主三元组选用平台时来源后加 `, hosted`。**已实现**

### 15.4 机器输出

`--message-format json` 的 `plan` 事件与 `result.json` 的 `configuration` 含 `platform` 与 `hostPlatform`,无平台时为
`null`;对象形式:

```json
{"name": "mcpplibs.openkal-platform", "version": "0.17.3", "source": "built-in", "hosted": false,
 "fields": {"toolchain": {"value": "llvm@23.1.3", "source": "platform"},
            "layers.c-abi": {"value": "openkal-musl@0.22.6", "source": "platform"}}}
```

`fields` 的键为 `toolchain`、`runner`、`runners.<name>`、`layers.<layer>`、`tools.<name>`(值为所用程序的路径)、
`link.driver`、`link.start`、`link.exe`、`link.shared`、`carrier`。**已实现**

## 16. 命令

| 命令 | 规定 |
|---|---|
| `--platform <值>` | `build`、`run`、`test`、`why`、`pack`、`emit build-database`、`self env`、`toolchain install`;即 `MCPP_PLATFORM` |
| `--host-platform <值>` | `build`、`run`、`test`、`why`、`pack`、`emit build-database`、`self env`;即 `MCPP_HOST_PLATFORM` |
| `mcpp self env --target <t> [--format json]` | 准备并报告一个配置:三元组、编译器与其来源、平台、工具、运行器与其程序、`run_under`;在工程中为该工程的配置,工程外为家目录缓存中的空程序;工程源码会被拒绝的原因(例:无 C++ 运行时而 `import std`)作为警告报告,配置照常报告。运行器的程序按 §10 的规则报告:其架构(JSON `runner.arch`),另一架构的运行器是否已构建(`runner.built`,不为报告而构建),执行宿主的程序(`runUnderProgram`),以及本机拒绝运行时的原因(人类输出的 `run` 行,JSON `runner.refusal`)(2026.10.29.1) |
| `mcpp self env --host` | 宿主图的配置;其构建程序已构建过时,报告宿主平台的运行器与其程序、工具 |
| `mcpp toolchain install --target <t>`(不指定编译器) | 安装该目标的工具链后准备该配置:获取平台包,构建其工具 |
| `mcpp toolchain list` | 目标行有 PLATFORM 列,JSON 有 `platform` 字段 |
| `mcpp test`(在平台包中,未给 `--target`) | 对 `targets` 中的每个目标以本包为平台构建测试;`host = "same-arch"` 时运行本机架构的目标与给出执行宿主的目标(§10),其余报告为已构建未运行 |

**已实现**

## 17. 拒绝码

机器可读的拒绝码出现在结果记录的 `reason` 中(docs/50)。

| 码 | 条件 |
|---|---|
| `platform-not-a-platform` | 被选中的包没有 `[platform]` |
| `platform-target-unserved` | 平台不服务该目标(§3.4) |
| `platform-tool-absent` | 链接或封装命名的程序既非图中构建、也未找到(§9.2) |
| `platform-version-group` | 图中的包与版本组冲突(§7.5) |
| `platform-phase-directive` | 平台阶段陈述了不属于该阶段的指令(§12.1) |
| `lld-required-absent` | `driver = "lld"` 而工具链无 `ld.lld` |

`platform-tool-absent` 取代 2026.10.26.2 的 `image-tool-absent`。**已实现**

## 18. 判据

1. `[platform]` 及其子表、`[host]` 与包引用中每个键的类型错误在读清单时拒绝,消息写出键与文件;未知键为警告并被忽略。
2. 选择按 §3.1、§4.1 的顺序;`none` 不使用平台;目录形式读取目录中的清单。
3. `cfg(platform = ...)` 下写 `platform` 被拒绝。
4. 图已提供某层时平台不加入该层的包,报告为 `graph`;`false` 不加入。
5. 版本组冲突在任何链接之前拒绝,消息给出两条出路。
6. 封装失败不发布产物,保留上一次的产物。
7. `same-arch` 的运行器不运行其他架构的产物,并给出 `run_under` 的写法;`mcpp test` 的人类输出给出完整的原因恰好一次(e2e 960)。
8. 平台的工具在任何图中按宿主约定构建。
9. 锁中平台加入的包带 `origin = "platform"`;报告与 JSON 给出每个字段的来源。
10. 一个与 openkal 无关的平台包的编译标志、命名、封装与运行器到达程序,`cfg(platform)` 下的覆盖取代对应字段,
    `--platform none` 不使用平台,没有 `[platform]` 的包被拒绝为平台(e2e 952)。
11. 宿主图选用平台时构建程序与其库作为映像构建并经平台的运行器运行,平台的工具按宿主约定构建,
    `--host-platform none` 时构建程序为本机程序(e2e 953)。
12. 给出执行宿主时,`mcpp run` 与 `mcpp test` 交给它的运行器程序是产物架构的 ELF;本机有该架构的 qemu 时映像经它运行、
    测试通过;`mcpp self env` 报告拒绝的原因,给出执行宿主时报告另一架构的运行器(e2e 960)。
13. 映像的单文件的载体是映像架构的程序;`--mode self-contained` 的单目标包是树,携带该架构的运行器(e2e 961)。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 1.0 | 2026-10-28 | 首版(mcpp 2026.10.27.1,mcpp#847):`[platform]` 的七组键与 `versions`、`hosted`;三种写法与层次;目标图与宿主图的选择;`cfg(platform)`;合并;依赖图的补全、版本组与改名边界;工具链;链接、命名与封装;运行器的表形式;载体;平台阶段与协议 17;工作空间;目标表的平台列;锁、报告、机器输出、命令与拒绝码。 |
| 1.1 | 2026-10-28 | `[platform] universal`;载体的三种携带形式(运行器形式、映像、映像组的 zip)、`mcpp_carried_digest`、Mach-O 节名 `__carried`、按系统的行;平台阶段可声明包格式与 artifact action;`self env --host` 报告运行器与工具;判据 10、11(e2e 952、953)。 |
| 1.2 | 2026-10-29 | mcpp 2026.10.29.1:§10 给出执行宿主时 `same-arch` 运行器的程序为产物架构的载体运行器形式,`mcpp test` 写出完整的拒绝原因一次;§11 单目标包与单文件的程序为产物架构的,不构建的行在生成工程之前被拒绝;§14 ok 映像与 Web;§16 `self env` 报告运行器程序的架构、执行宿主与拒绝,平台包的 `mcpp test` 运行给出执行宿主的目标;判据 7、12、13(e2e 960、961)。 |
| 1.3 | 2026-10-30 | mcpp 2026.10.30.1(设计 2026-10-29 §4.3–§4.5、§13.3):§1.1 `universal` 弃用、不起作用;`runner.hosts`;`carrier` 接受列表,每项的 `hosts` 与 `build`;`descriptor_keys`;§10 执行宿主的运行器程序按 `hosts` 选择载体项;§11 携带映像的载体只属于单文件,树携带运行器形式;判据 13 改写(e2e 961)。 |
