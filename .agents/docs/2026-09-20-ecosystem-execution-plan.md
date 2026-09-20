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
