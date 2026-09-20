---
subject: plan
status: active
---

# C 环境生态方案：执行计划

- 依据：`.agents/docs/2026-09-20-openkal-c-environment-ecosystem-design.md`
- 日期：2026-09-20
- 原则：每个仓库一个 PR；测量先行，数据不通过就停在测量。

## 0. 起点状态（2026-09-20 实测）

| 仓库 | 版本 | 状态 |
| --- | --- | --- |
| mcpp | 2026.9.18.3 | main 干净 |
| openkal | 0.14.0 | 已发布并登记 |
| openkal-musl | 0.16.0 | 已发布并登记 |
| openkal-llvm-runtime | 0.12.0 | 已发布并登记 |
| mcpp-index `pins.toml` | `runtime = "0.10.0"` | **未抬**，测量图里没有任何包声明 `[c-abi]` |

窗口仍开着：包侧 `_WIN32` 适配已撤（#439），引擎侧实现未在测量中生效。

## 1. 轨道与依赖

```
E1 抬 pins + 重测 ──────────────────────┐  （独立，最高价值，先跑）
                                        │
A  mcpp 引擎（P0/P1/P5-L2/P7-L3/absent）─┼─→ E2 refused + 新引擎 pin
                                        │
B  openkal tools + 文档（SURFACE→接口集）─┼─→ D 各实现填 provides-interfaces
                                        │
C  openkal-musl（absent + requires）─────┘
                                        │
                                        └─→ F 发布 + 沙箱验证
```

| 轨 | 仓库 | 内容 | 依赖 |
| --- | --- | --- | --- |
| **E1** | mcpp-index | `pins.toml` runtime 0.10.0 → 0.12.0，重测 30 成员 | 无 |
| **A** | mcpp | P0.1 探针 `--target`、P0.2 注释、P1 冻结语义、P5-L2 解析期集合包含、P7-L3 链接期集合差、`[c-abi.absent]` 解析与诊断 | 无 |
| **B** | openkal | `tools/interfaces-from-surface.sh`、README 记述四级阶梯 | 无 |
| **C** | openkal-musl | `[c-abi.absent]` 声明 + CI 断言、`requires-interfaces` | A（字段语义）、B |
| **D** | openkal-linux / -windows / -macos | `provides-interfaces` 由产物生成 + CI 断言 | B |
| **E2** | mcpp-index | `refused` status、新引擎 pin、重测 | A、E1 |
| **F** | 全部 | 发布、gtc 镜像、沙箱验证 | 全部 |

## 2. 判据

| 轨 | 判据 |
| --- | --- |
| E1 | 重测后 Windows 腿：第一类（`_WIN32` 选错分支）应自愈；逐条记录未自愈的与原因 |
| A | 单测覆盖新字段的解析与拒绝；`riscv64-none-elf` 探针 dump 含 `__riscv` 不含 `__linux__`；未声明的包命令行逐字节不变 |
| B | 脚本对 `SURFACE.txt` 产出 16 个接口名；删掉一个组，产出少一个 |
| C | 把 `fork` 的 `form` 从 `link` 改成 `enosys`，CI 必须红 |
| D | 删掉某实现的一个接口定义，CI 必须红并指名该接口 |
| E2 | `refused` 与 `fails` 分开计；`cfg(c-abi = ...)` 的出现次数记录在案 |
| F | 沙箱中只写版本号即可解析并构建 |

## 3. 不做

- P3（撤 `__CYGWIN__`）：需要 E1 的重测数据才能判断代价，本轮只记录证据，不落地。
- P4（合成节点层）：openkal-musl 的移植工作量独立，另轮。
- P6（openkal-win-ucrt）：需要 c++-abi 侧配套，另轮。

---

## 4. 执行记录(2026-09-20)

### 4.1 E1 测量:声明第一次生效

`tests/openkal/pins.toml` 的 `runtime` 从 0.10.0 抬到 0.12.0,重测 30 个成员
(mcpp-index run `35503820978`,mcpp 2026.9.18.3,llvm@22.1.8)。

| target | 之前 | 之后 |
| --- | --- | --- |
| `x86_64-linux-gnu` | 27 runs / 3 fails | 27 runs / 3 fails |
| `x86_64-windows-gnu` | 15 runs / 15 fails | **23 runs / 7 fails** |

**八个成员零适配转绿**:asio(经 cmp-module)、catch2、cli11、eigen、fmtlib.fmt、
libpng、re2、lua(经 capi-lua)。全部是按 `_WIN32` / `__MINGW32__` 选分支的那一类。

**剩余七个分三组**,与评审文档的预测逐条对上:

| member | 诊断 | 预测 | 结果 |
| --- | --- | --- | --- |
| mimalloc | `atomic.h:16 'windows.h'` | `__CYGWIN__` 挡住 | 确认 |
| sqlite3 | `sqlite3.c:29506 'windows.h'` | `__CYGWIN__` 挡住 | 确认 |
| archive(xz) | `tuklib_physmem.c:21 'windows.h'` | 本来就要 configure | 确认 |
| c-ares | `ares_setup.h:81 'windows.h'` | 同上 | 确认 |
| curl | `"too small curl_off_t"` | 同上 | 确认 |
| doctest | `undefined symbol: __cxa_thread_atexit` | **未预测** | 新发现 |
| spdlog | 同上 | **未预测** | 新发现 |

### 4.2 本轮新发现

1. **`__CYGWIN__` 的 trade-off 已可结算。** mimalloc 的守卫注释写着
   "we use windows locks on cygwin, but otherwise treat it at unix",sqlite3 的
   `SQLITE_OS_WIN` 检测列表含 `__CYGWIN__`。**上游用这个名字回答的是"Win32 可用",
   不是"对象格式是 PE"。** 这是 P3 的直接判据,可在下一轮落地。

2. **`__cxa_thread_atexit` 缺口。** doctest 与 spdlog 此前停在缺头文件,现在编译
   过去、停在链接。这是 openkal 之上 C++ 运行时的缺口(libc++abi 用来登记
   `thread_local` 析构的钩子),在环境正确之前到不了。属 openkal-llvm-runtime /
   openkal-musl,另轮。

3. **clang 20.1.7 在 Windows 上对三种写法都崩。** `[c-abi.absent]` 的解析块写成
   `parse_string` 内的语句块、写成模块导出 purview 里返回
   `expected<vector<struct-with-strings>, string>` 的自由函数、以及用成员指针作
   sort 投影,在 Windows 上各崩一次,其他宿主全过。最终形态是匿名命名空间里的内部
   helper + 出参 + `optional<string>` + 比较器。

4. **两个新键的向后兼容性不同,且这条差别要写进文档。** `[kernel-abi]` 是未知
   **顶层表**,旧引擎忽略(实测 2026.9.17.1 静默接受);`[c-abi.absent]` 是已知表里的
   新键,旧引擎拒绝整份清单。因此前者不需要抬 floor,后者需要。

### 4.3 PR

| 仓库 | PR | 内容 |
| --- | --- | --- |
| mcpp | #678 | 探针带目标、撤 hostStripMacros、`[kernel-abi]`、`[c-abi.absent]`、冻结 `presents` |
| openkal | #42 | `check-surface.sh --interfaces/--toml`,接口集由产物派生 |
| openkal-musl | #39 | 0.17.0 `[c-abi.absent]` + CI 断言 |
| openkal-linux | #29 | 0.15.0 `provides-interfaces`,CI 重新生成并 diff |
| mcpp-index | #444 | 抬 pins、重测、`refused` |
