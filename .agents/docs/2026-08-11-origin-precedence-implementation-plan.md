# `$ORIGIN` 优先级 + 共享库运行时契约 —— 实施计划

配套分析:`2026-08-11-runtime-search-origin-precedence-analysis.md`
范围:**A + B + C2 + C1**,单 PR(2026.8.11.3)
状态:**已实施**(实测结果见 §2.2,与计划的偏差见 §2.1)

---

## 0. 四个角度的取舍

### 优雅设计 —— 把「顺序」从字符串拼接提升为声明

缺陷的形状是:**一条链接命令行的顺序,由两个互不知情的生产者用 `+=` 决定。**
`flags.cppm` 把 farm 拼在全局 ldflags 末尾并注释「so it is LAST」;`plan.cppm` 把
`$ORIGIN` 拼在 per-unit;`ninja_backend` 渲染成 `$ldflags $unit_ldflags`。
三处都对,合起来是错的。

对策不是「换个地方拼」,而是新增 `src/build/link_line.cppm`
(`mcpp.build.link_line`):把 per-unit 尾部声明成**具名槽位**,相对顺序写在类型里,
由单测钉死。加一个新的调用方不再可能把顺序拧错 —— 它必须选一个槽。

### 架构稳定性 —— 消灭同一决策的第二处推导

`dist::default_contract(Role)` 自称「The role -> contract policy, in one place」,
**却没有任何生产调用方**(只有单测);真正的策略在 `flags.cppm:704-709` 独立算了
一遍。这正是 `distribution.cppm` 开篇声讨的那类债(「used to be derived
independently in five places」),只是换了个位置复发。

本次把 `default_contract` 变成**活的唯一真源**,`flags.cppm` 调它。

### 兼容性 —— 只有一个平台的行为改变,且正是有缺陷的那个

| 目标格式 | 共享库契约 变化 | 产物字节 |
|---|---|---|
| **ELF** | SelfContained → **ToolchainCoupled** | 变(这是修复) |
| Mach-O | 不变(SelfContained) | **不变** |
| PE | 不变(SelfContained) | **不变** |

`-Wl,-rpath,<farm>` 换槽位后在**没有共享库依赖的工程**上字节完全不变
(槽位为空即不渲染);有共享库依赖的工程只有 rpath 次序变化。

用户逃生舱保留:`cxx_runtime = { shared = "self-contained" }` 可恢复旧行为,
且此时自动补 C1 护栏,不会重新打开符号泛滥。

**缓存**:`.so` 的链接边在工程自己的 ninja 图里(`build bin/libX11.so : cxx_shared
…`),不来自依赖缓存;ninja 按命令行变化重跑链接。契约只影响链接标志、不影响编译
标志,因此对象缓存键无需改动。发版 bump 会改工程指纹 ⇒ 全量重建,不存在旧契约残留。

### 跨平台 —— 平台差异只准出现在已有的格式维度里

`distribution.cppm` 的机制表本来就是 `(contract × stdlib × format) → flags`。
共享库的危害本身就是格式相关的,所以把 `Format` 提到 `default_contract` 的入参,
是让既有维度承担它,而不是新开一条平台分支:

- **ELF**:一个全局符号命名空间,先加载的定义胜出 ⇒ 静态内嵌 libstdc++ 的 `.so`
  会把 2931 个 std 符号无版本导出(其中 **777 个是 GLOBAL 定义**,只可能来自
  `libstdc++.a`),可执行文件的 std 引用被绑到它身上
- **Mach-O**:self-contained 的机制本就是 `-Wl,-load_hidden,<archive>`,符号是
  hidden,dyld 不会归一 ⇒ **危害不存在**;且 toolchain-coupled 在 macOS 是已知死路(#202)
- **PE**:没有全局命名空间,导入按 DLL 逐个按名解析 ⇒ **危害不存在**;`-static`
  是那里的标准约定

`link_line::UnitTail` 保持格式中立:槽位按**职责**命名,不按标志拼写。
PE 的 `runtimeFallback`/`loaderTag` 天然为空,Mach-O 的 `dependencies` 装
`@loader_path` —— 不需要任何 `if (platform)`。

---

## 1. 步骤拆分

每一步都有独立判据,可单独 revert。

### S1 — 新增 `mcpp.build.link_line` 协议模块

`src/build/link_line.cppm`,导出:

```cpp
struct UnitTail {
    std::string dependencies;      // 1. -L/-l + $ORIGIN / @loader_path
    std::string cxxRuntime;        // 2. -static-libstdc++ / -load_hidden / --exclude-libs
    std::string runtimeFallback;   // 3. 兜底运行期搜索(今天=SubOS farm 视图)
    std::string loaderTag;         // 4. --disable-new-dtags,必须字面最后
    std::string render() const;    // 按上述顺序连接,自动补分隔空格
    bool empty() const;
};
```

每个槽位的注释必须写清**为什么在这个位置**,尤其:
`dependencies` 在最前,因为那些目录装的正是本次链接解析到的物理文件;
`runtimeFallback` 必须晚于它们,因为 farm 是 `xlings install` 会重写的视图;
`loaderTag` 字面最后,因为 ld 认最后一个 `--enable/--disable-new-dtags`。

**判据**:`tests/unit/test_link_line.cpp` 断言四槽顺序 + 空槽不产生多余空格
+ 任意槽为空时其余顺序不变。

### S2 — `CompileFlags` 增加 `ldRuntimeFallback`,farm 不再进 `f.ld`

`flags.cppm`:`farm_ld` 从 `f.ld`(`:996-999`)移出,赋给 `f.ldRuntimeFallback`。
字段名取 provider 中立的 "runtime fallback",与 `link_line` 的槽位同名。

**判据**:`test_build_flags.cpp` 断言 `f.ld` 不含 farm 路径、`f.ldRuntimeFallback` 含。

### S3 — `ninja_backend` 改用 `UnitTail` 组装 `unit_ldflags`

`ninja_backend.cppm:1400-1418` 的三次 `unit +=` 换成填槽 + `render()`。

**判据**:`test_ninja_backend.cpp` 新增用例 —— 带 `$ORIGIN` 的链接单元,
在 `ldflags + " " + unit_ldflags` 合成串里 `$ORIGIN` 位置 **早于** farm。
(仅 Linux 分支产出 farm,其它宿主 `GTEST_SKIP`)

### S4 — `Role` 拆出 `SharedLibrary`,`default_contract` 变 `(Role, Format)` 并成为唯一真源

1. `distribution.cppm`:`Role` **末尾**追加 `SharedLibrary`(不动既有枚举值,
   因为 `ldStdlibByRole` 按枚举值索引);导出 `kRoleCount`
2. `default_contract(Role, Format)`:`(SharedLibrary, Elf) → ToolchainCoupled`,
   其余一律 `SelfContained`(= 今天的行为)
3. `to_string(Role)` 补 `"shared-library"`
4. `flags.cppm:83-92` `role_of`:`LinkUnit::SharedLibrary → Role::SharedLibrary`
5. `flags.cppm:52,54`:`std::array<…, 3>` → `kRoleCount`
6. `flags.cppm:704-709`:`base` 改为调用 `default_contract`,并新增
   `sharedContract`;`mi.format` 的推导上移到契约计算之前
7. `wantsArchives`(`:768`)把 `sharedContract` 纳入

**判据**:`test_distribution.cpp` 逐格断言 `(Role × Format) → Contract` 全表。

### S5 — C1 护栏:ELF 上显式 self-contained 的共享库必须隐藏归档符号

`distribution.cppm` ELF 分支,`Role::SharedLibrary` 且 `effective ==
SelfContained` 时追加:

- libstdc++:`-Wl,--exclude-libs,libstdc++.a`
- libc++:`-Wl,--exclude-libs,libc++.a -Wl,--exclude-libs,libc++abi.a`
  (有 libunwind.a 时再加一条)

用**归档基名**而非路径:`--exclude-libs` 按归档文件名匹配,GNU ld 与 lld 一致。
显式列名而非 `ALL`,以免连用户自己的静态库一起隐藏。

**判据**:`test_distribution.cpp` 断言该单元格产出含 `--exclude-libs`,
且 `Role::Distributable` 不含。

### S6 — manifest:`cxx_runtime` 增加 `shared` 键

`types.cppm` 加 `cxxRuntimeShared`(`[build]` 与 `[target.<triple>]` 两处);
`toml.cppm:954-968` 的键白名单加 `"shared"`,错误文案同步。

**判据**:`test_manifest.cpp` 解析 `cxx_runtime = { default=…, tests=…, shared=… }`;
未知键仍报错。

### S7 — 测试:把假绿换成真判据

1. `tests/e2e/219_runtime_search_farm_is_last.sh`:断言 farm 是 DT_RPATH 的
   **字面**最后一项,删掉「last ABSOLUTE entry」放宽与那段理由
2. **新增 e2e:行为不变量**。构造 `$ORIGIN` 与 farm 同名 SONAME 的局面,
   用 `LD_DEBUG=libs` 断言解析到 `$ORIGIN` 那一份。
   ⚠️ **不得依赖崩溃** —— C2 落地后崩溃会消失,依赖崩溃的断言会立刻假绿
3. 新增 e2e:ELF 共享库**不得**导出 std 符号(`nm -D` 计数为 0)

### S8 — 文档 + 版本 + pin

- `docs/` 用户文档:`cxx_runtime` 的 `shared` 键、共享库默认契约按平台的说明
- 版本 bump(`YYYY.M.D.N`,月日不补零)
- pin 最新 xlings(唯一真源 `src/platform/xlings/xlings.cppm::kXlingsVersion`,
  由 `check_version_pins.sh` 机器校验 16 处)

---

## 2. 合并后的验证清单(缺一不可)

| # | 判据 | 方法 |
|---|---|---|
| V1 | farm 是 DT_RPATH 字面最后一项 | `readelf -d` |
| V2 | `libX11.so.6` 解析到 `$ORIGIN` | `LD_DEBUG=libs`,**看行为不看形状** |
| V3 | `bin/libX11.so` 导出 std 符号数 = 0 | `nm -D \| grep -cE '_ZNSt\|_ZNKSt\|_ZSt'` |
| V4 | exe 不再有 `U _ZNKSt13runtime_error4whatEv` | `nm -D --undefined-only` |
| V5 | helloegui GUI 真的起来 | 本机跑,`timeout` 退 143 |
| V6 | **revert 掉 S2+S3 后 V2 必须重新变红** | 证明 C2 没吃掉 A 的判据 |
| V7 | PE / Mach-O 产物字节不变 | CI 对应 job |
| V8 | 生态:mcpp-index workspace 全绿 | compat 包全是 `.so`,是重灾区 |

---

## 2.1 实施记录 —— 计划没写对的三处

**① `default_contract` 根本没有生产调用方。**
它自称「The role -> contract policy, in one place」,实际只有单测调用;真正的策略
在 `flags.cppm:704-709` 独立算了第二遍。所以 S4 不只是"加一个入参",而是把这个
函数**接回主路径**。这也是本次改动里架构收益最大的一处。

**② 「共享库导出零个 std 符号」这个判据一开始是错的。**
实测:一个用了 `std::string` 的 C++ 共享库,即使 toolchain-coupled,也会导出 **31 个**
std 符号 —— 全部是 `W`(weak/COMDAT 模板实例化,从头文件实例化进它自己的 TU),
GLOBAL 为 0。这些是 C++ ABI 的预期行为,进程内归一它们是**对的**。

真正的判据是 **GLOBAL 计数为 0**:`T` 符号只可能来自 `libstdc++.a`。
对照数据:坏版本的共享库是 **713 GLOBAL**(+ weak),好版本是 **0 GLOBAL**。
如果按"总数为零"写,断言不可满足,最后只会被删掉 —— 一条真不变量换成没有不变量。

**③ e2e 的工程形状换了两次才对。**
- `int main()`:DT_RPATH 里**根本没有 `$ORIGIN`**,整条断言链空转(这正是旧断言
  能被写成那样的原因 —— 它从未在有 `$ORIGIN` 的产物上跑过)
- 同包内的 `kind = "shared"` target:mcpp 把该包的模块对象**直接链进可执行文件**,
  没有 `-l`、也没有 `$ORIGIN`
- **消费一个 path 依赖提供的共享库**:才产生 `-Lbin -Wl,-rpath,'$ORIGIN' -lgreetdep`,
  与出问题的真实产物同形

另外接口里写内联定义(`export int f() { return 7; }`)会让符号被实例化进消费者、
依赖边消失,所以接口与实现必须分文件。

## 2.2 实测结果(helloegui:imgui + GLFW + X11)

| 判据 | 修复前 | 修复后 |
|---|---|---|
| DT_RPATH 尾部 | `… : <subos>/lib : $ORIGIN` | `… : $ORIGIN : <subos>/lib` |
| `libX11.so.6` 解析到 | farm(`xim:libX11 1.8.10`) | `$ORIGIN`(farm 未被试到) |
| `bin/libX11.so` 导出 std 符号 | 2931(777 GLOBAL) | **0** |
| `bin/libXau.so` | 9 557 936 B | **39 368 B** |
| `bin/libXdmcp.so` | 9 561 504 B | **41 552 B** |
| exe 未定义 `runtime_error::what` | 有 | **无** |
| 运行 | `symbol lookup error` | **GUI 正常启动** |

红测(两条新 e2e 对已发布 2026.8.11.2):219 报「farm is not the last entry」,
222 报「exports 713 GLOBAL standard-library symbols」—— 均以正确理由失败。

## 3. 已知风险

1. **C2 掩盖 A 的症状** —— 见 V6,这是本 PR 最大的假绿风险
2. **本机 e2e 噪声** —— 共享 gcc specs 污染、`pipefail`+`grep -q` 的 SIGPIPE
   flake;本机红必须逐条与已发布二进制比对,不可直接当回归
3. ~~**`--exclude-libs` 的链接器覆盖面**~~ —— **已实测**。两条路径都验证过:
   - libstdc++ 分支(`-lstdc++` 形式):e2e 222 不变量 4,GLOBAL 导出 713 → 0
   - libc++ 分支(**按完整路径**给归档):直接对拍
     `g++ -shared -nostdlib++ <abs>/libstdc++.a` ± `-Wl,--exclude-libs,libstdc++.a`
     ⇒ `_ZNKSt13runtime_error4whatEv` 导出数 **1 → 0**,证明它按**基名**匹配、
     与归档是 `-l` 还是完整路径给出无关

   若将来接入其它链接器,应在机制表里加格式/链接器维度,而不是加平台分支。

4. **`dist::CompileFlags::contractByRole` 只写不读**(既有,非本次引入)。
   它与 `TargetEntry::cxxRuntimeTests`(既解析不了也不生效)是同一类死字段,
   本次没有顺手清理 —— 删一个公开结构体字段是另一件事,不该混进修缺陷的 PR。
