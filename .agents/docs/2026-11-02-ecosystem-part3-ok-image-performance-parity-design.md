---
subject: openkal-performance
status: active
---

# 生态设计 part 3：ok 映像的性能对等——引擎路径、openkal 运行时与观测

日期：2026-11-02（rev 4，实施记录）。基线：mcppls-clangd #16（`6336bf1`，引擎 23.1.0-mcppls.8）、mcppls #75（0.0.20）、
openkal-platform 0.20.2（openkal 0.19.0、openkal-linux 0.20.0、openkal-macos 0.17.1、openkal-windows 0.15.0、openkal-musl 0.25.0、
openkal-llvm-runtime 0.16.15、openkal-image 0.1.9）。前文：[part 2](2026-10-30-ecosystem-part2-ide-and-language-service-design.md)
§25.3（B4–B8）、§25.9（H1–H8）、§25.10（openkal 的边界）。

**修订记录**

- rev 1：综合 2026-11-01 至 11-02 的测量与讨论。已定：引擎的优化与 openkal 的适配分放两个 series；引擎按能力而不是按
  操作系统选择实现；越底层的改动越要一轮做完，并在本层以本层的测试证明，引擎与 mcppls 的长周期 CI 每轮只走一次；
  openkal 增加可选的观测接口，采集在 okrun，分析在单独的 okprof。多视角自我评审 14 条（§15），待评审项见 §16。
- rev 2：rev 1 的待评审项全部定下（§16.1）：分配器为 mimalloc，按 mwrap 规范在 mcpplibs 下新建 mimalloc-m（§7.1）；job 的两种
  执行方式、观测接口两组、接纳记忆、线程指针不改、引擎不做 LTO/PGO、门禁阈值、两项 L1 内的后备做法都按 rev 1 进入本轮。
  选定 mimalloc 带出一项规范改动：§5.3 由单一的 `kal_mem_discard` 扩为内存区域五个操作（`kal_alloc` 是带锁的通用分配器，不能
  给出按大粒度对齐、延迟提交的区域）。第二轮自我评审 9 条（§15.2），新的待评审项见 §16.2。
- rev 3：按评审改写（§16.3）：mallocng 保留为 feature，mimalloc 为默认；openkal 的内存区域只陈述通用的虚拟内存语义，
  C 库的 `mmap`/`munmap`/`madvise` 建在它上面；mimalloc-m 只把 `compat.mimalloc` 的接口映射为模块，嵌入 C 库所需的全部适配
  在 openkal-musl。mcpp 增加依赖的补丁与嵌入（§14）：打过补丁的库是一个新的库（变体），唯一性按程序而不按工作区判定；同一个
  mcpp 版本修正 mcpp#875（外部工具链找不到 mcpp 安装的 Ninja）与 mcpp#881（`clean --stale` 的报告自相矛盾）及其附带的两处
  不一致。第三轮自我评审 17 条（§15.3），待评审项见 §16.4；四项均采纳（§16.5），本文定稿，按 §12 实施。
- rev 4：实施中的两项决定（§16.6）与修订（§17）：冷阶段的差距来自装入时复制映像的全部段，openkal 0.21 增加从文件映射指令的
  可选操作；L1 改为报告，门禁在 L2、L3。

## 1. 问题与结论

mcppls 0.0.19 起四个系统都分发 `clangd.okb`。ux-mcpp 的"已有缓存的新会话"（U2-restarts）首次就绪：原生 Linux 引擎 3.3–5.1 s，
映像 21.9–33.1 s，预算 12 s。part 2 把它依次归于处理器数（H1）、锁的所有者（H3）、C 库的分配与字符串（H6、H7）与提供者索引的
重复 mangle（H8），逐项修正后仍为 26.7–33.1 s。

本轮以同一份引擎源码的三组对照（§2.2）把差距分为两层：

1. **引擎的路径选择（约 4 倍）。** mcppls-clangd 的 series 0017、0019 以 `#ifdef __linux__` 选择模块缓存与 worker：Linux 得到
   自有缓存（以按打开文件持有的字节范围锁作租约，读模块文件不复制）与隔离 worker；其余系统得到进程内构建与读时复制。
   对照表明首次就绪取决于 worker 策略，自有缓存决定就绪之后的 CPU（§2.2）。映像的
   目标是 `<arch>-openkal`，在预处理器看来不是 Linux，因此即使运行在 Linux 主机上也取后者。可移植路径另有一处缺陷：H8 的修正
   （0038）在服务端从未生效（§3 E2）。
2. **映像的运行时（约 1.6 倍）。** 同一路径上映像比原生多用 1.6 倍以上的 CPU，缺页多两个数量级。差距在 openkal-musl：分配器
   以 mmap/munmap 往返（没有 `brk`，释放即解除映射）、字符串函数、以三次定位拼成的 `pread` 与持锁跨系统调用的描述符表。
   okrun 与 openkal 的调用本身约占 3 %。

由此得到本轮的四条原则（§4）与各层的增补（§5–§10）。

## 2. 测量

### 2.1 方法

- **探针**：mcppls 的 `probe/ux-warm-profile` 分支（不合入）。nightly 的 ux 只运行 ux-mcpp 一轮：冷阶段，随后已有缓存的新会话
  在 `perf record -F 499 -g` 与 `perf stat` 下运行 25 s，再在 `strace -c` 下运行一次。`payload-run` 选择被测的包；
  `engine-run` 以 mcppls-clangd 某次 CI 的原生 linux-x64 引擎替换包中的引擎（同一源码，另一种格式）；`engine-args` 给原生
  引擎附加参数。
- **符号**：映像的样本由 `clangd.oke` 的符号表还原（映像在 perf 看来是匿名可执行映射，基址取引擎启动数秒后的
  `/proc/<pid>/maps`）；原生 .6 的样本由发布的符号包还原；.8 原生构件不带符号，只用计数。
- **指标**：首次就绪受机器差异影响（同一 .7 映像两次为 21.9 s 与 32.6 s，波动约 ±25 %）；CPU 时间、缺页与系统调用次数在
  不同机器上稳定得多，作为主要指标。

### 2.2 历史与对照

**历史**（mcppls CI 的 ux-mcpp；U2-bmi 的 176/176 是 conformance 不认识可移植路径的副本命名，`Built module` 为 0，不是重建）：

| run | 引擎 | warm 就绪 | U10 | cold 就绪 | U4 p95 | 峰值 RSS | U2-bmi |
|---|---|---|---|---|---|---|---|
| 41197322259 | .5 原生 | 4.0 s | 4.9 s | 106.5 s | 0.45 s | 1799 MB | 0/176 |
| 41189230523 | .5 原生 | 5.0 s | 6.1 s | 135.0 s | 0.58 s | 1491 MB | 0/176 |
| 41245537861 | .6 原生 | 4.2 s | 5.3 s | 110.6 s | 0.48 s | 1659 MB | 0/176 |
| 41255535926 | .6 原生 | 3.3 s | 3.9 s | 81.2 s | 0.31 s | 1617 MB | 0/176 |
| 41269126447 | .7 映像（0.0.19） | 32.6 s | 33.4 s | 174.5 s | 0.82 s | 1380 MB | 176/176 |
| 41275927227 | .7 映像（0.0.19） | 21.9 s | 22.3 s | 113.8 s | 0.56 s | 1331 MB | 176/176 |
| 41289474740 | .7 映像（musl 0.25.0） | 30.5 s | 29.2 s | 173.7 s | 0.90 s | 1337 MB | 176/176 |
| 41306200720 | .8 映像（0038） | 26.7 s | 26.0 s | 147.5 s | 0.76 s | 1270 MB | 176/176 |

**同源对照**（引擎 23.1.0-mcppls.8，`6336bf1`；perf stat 的 25 s 窗口自引擎启动起）：

| 组 | 引擎与路径 | run | warm 就绪 | CPU | 缺页 | 上下文切换 |
|---|---|---|---|---|---|---|
| A | 原生，Linux 路径（默认） | 41317756737 | 4.8 s | 3.1 s | 55 k | 75 |
| B | 原生，`--modules-builder-worker-policy=in-process --modules-builder-owned-cache-payload-mib=0`（映像所取的路径） | 41319432641 | 20.4 s | 21.9 s | 7.7 k | 3.3 k |
| B2 | 原生，`--modules-builder-owned-cache-payload-mib=0`（worker 保留，自有缓存关闭） | 41326088748 | 5.4 s | 14.5 s | 164 k | 8.2 k |
| C | 映像 | 41314556906 | 33.1 s | ≥ 35.6 s（窗口内未结束） | 792 k | 20.6 k |

A→B 是路径，B→C 是运行时。B2 把路径的两半分开：**首次就绪由 worker 策略决定**（B2 5.4 s，B 20.4 s）；自有缓存关闭不影响就绪，
但就绪之后的 CPU 是 A 的 4.7 倍（副本与其清理）。进程内策略为何使每个单元的模块规划变慢，机制尚未定位（原生 .8 构件不带
符号）；§9.3 把它列为引擎候选之前的定位任务。

### 2.3 映像内的 CPU 分层（C 组与 .7 的同一探针，self 样本）

| 层 | 占比 | 构成 |
|---|---|---|
| 引擎代码（clang、clangd、LLVM）与 libc++ | 48 % | 与原生同一份代码；其中 `indexProducerCommands` 含被调用者共 33 %（`CommandMangler` 29 %、`OptTable::ParseArgs` 27 %） |
| C 库（openkal-musl） | 34 % | `strlen` 11.3 %（八成来自 `matchOption`）、分配约 12 %（`__malloc_tc_take` 4.0、`get_meta` 3.0、`free` 1.5、`malloc` 1.3、`alloc_slot` 0.9）、`memcmp` 3.8 %、`memcpy` 3.3 % |
| 内核 | 14.5 % | 缺页、清零页、TLB 击落（`hyperv_flush_tlb_multi`）、路径查找 |
| okrun、openkal-musl 的端口层、模拟的线程局部变量 | 约 3 % | okrun 1.5 %、`__okm_get_tp` 0.9 %、`__emutls_get_address` < 0.1 % |

### 2.4 系统调用（strace -c，warm 阶段一次）

| 调用 | A 原生 Linux 路径 | B 原生可移植路径 | C 映像 |
|---|---|---|---|
| `pread64` | 648 k | 12 k | — |
| `read` / `lseek` | 1.7 k / 0.1 k | 0.3 k / 0.1 k | 66 k / 71 k（`pread` 拼接：定位、读、定位回） |
| `newfstatat` / `stat` | 6.6 k / 34 k | — / 124 k | 708 k / — |
| `fstatfs` | — | — | 71 k（每次要执行位的 stat 多一次） |
| `mmap` / `munmap` | 0.9 k / 0.9 k | 0.2 k / 0.2 k | 24 k / 22 k |
| `sched_yield` | — | — | 73 k（描述符表的自旋锁） |
| `clock_gettime` | —（vDSO） | —（vDSO） | 23 k |
| `futex` | 6.4 k | 3.8 k | 52 k |

### 2.5 对 part 2 的更正

- **B8 的结论更正。** part 2 §25.3 B8 以 40 个模块的 `--check` 测得"映像构建与复用模块比原生快"，据此判定隔离 worker 与自有缓存
  不是差距的原因。ux 的 warm 阶段测的是另一件事：每个单元的模块规划在可移植路径上重复做（§3 E1、E2）。同源对照 A→B 表明
  路径本身贡献约 4 倍。B8 的"能力对等"条件保留，判定依据改为 §11 的 warm-prime 验收。
- **H8 的结论更正。** 0038 在服务端从未生效：所有日志中 `reused unmangled` 为 0 次，C 组的 profile 与 0038 之前相同。
  30.5 s → 26.7 s 在机器波动之内。原因见 §3 E2。

## 3. 归因

| # | 项 | 事实 | 层 | 修正（节） |
|---|---|---|---|---|
| E1 | 路径按操作系统选择 | 0017 的 worker 策略默认值、0019 的自有缓存额度、`ModuleBuildCache.cpp` 的全部实现都在 `#ifdef __linux__` 之下。自有缓存所用的设施——`openat`/`unlinkat`/`mkdirat`/`renameat`/`fstatat`、`fdopendir`、`flock`、`F_OFD_SETLK(W)`、`fsync`/`fdatasync`、`F_DUPFD_CLOEXEC`——openkal-musl 0.24.0 起全部提供（OFD 锁经 `kal_fs_lock`，三个实现都是按打开文件持有：Linux 与 macOS 为 `F_OFD_*`，Windows 为 `LockFileEx`）；只有 worker 组的内存预算（cgroup v2）没有对应 | mcppls-clangd 主 series | §9.1 |
| E2 | 0038 不生效 | `OverlayCDB::getProjectModules` 为 MDB 设置 mangler 的标识；`ModulesBuilder` 随后对每个请求调用 `MDB->setFileSystem(TFS)`（`ModulesBuilder.cpp` 七处），`CompoundProjectModules::setFileSystem` 重建 `CompileCommandsProjectModules` 并转发 mangler，新对象的标识为空，快路径的条件恒不成立。单元测试未经 `setFileSystem` | mcppls-clangd 主 series（0038） | §9.1 |
| E3 | 读时复制 | 可移植路径在每次读取前把已发布的 BMI 复制一份（warm 启动复制全部 176 个，0.8–4.2 s 内在后台完成），并有周期的副本清理 | mcppls-clangd 主 series | §9.1（有租约时不复制） |
| R1 | 分配器 | openkal-musl 没有 `brk`，mallocng 的组都来自 mmap，空组即 munmap；再用时缺页并清零。C 组缺页 792 k（B 组 7.7 k）。单线程一次 malloc+free 31–38 ns（glibc 约 4 ns） | openkal 规范（内存区域）→ mimalloc-m → openkal-musl | §5.3、§7.1、§7 |
| R2 | 字符串函数 | 0.25.0 的 `strlen` 为 SSE2 与对齐字，`memcmp` 为 8 字节；glibc 按处理器选择 AVX2/EVEX 实现，短串有专门的入口 | openkal-musl | §7 |
| R3 | 定位读写 | openkal 没有按偏移的传输，`pread`/`pwrite` 为"定位、传输、定位回"三步，且在描述符表的全局锁内；同一锁在 `openat`、`close` 中跨越系统调用持有；等锁以 `sched_yield` 自旋 | openkal 规范 → 实现 → openkal-musl | §5.1、§7 |
| R4 | stat | 要执行位时每次 stat 多一次 `fstatfs`（卷属性） | openkal-musl | §7 |
| R5 | 单调时钟 | openkal-linux 以系统调用取时间，不经 vDSO | openkal-linux（实现） | §6 |
| R6 | 线程指针 | 每次取 `errno`、线程私有数据与模拟的线程局部变量都经 `kal_task_current` 与一次散列查表，约 1 % | 不改（§5.5） | — |
| W1 | worker 的启动 | 自有缓存要求隔离 worker；映像的 worker 是一个新的 okrun 进程再接纳 146 MB 的 okb（分块并行校验，CI 的 x64 机器约 0.04–0.14 s）。可移植路径不起 worker，所以这项代价今天测不到，E1 修正后才出现 | openkal-image | §8 |
| T1 | 测试缺口 | 验收的用例是计算密集的单次 `--check`；ux 只在 Linux 上运行，且只测过原生引擎；macOS、Windows 的原生引擎同样走可移植路径，从未被测 | mcppls-clangd、mcppls | §11 |

## 4. 原则

1. **按能力，不按操作系统名。** 引擎问"这个平台能否提供按打开文件持有的锁、目录句柄、打开时可删除、worker 组预算"，
   在启动时于缓存目录上实测一次，不问"是不是 Linux"。openkal 以属性位（规范 §6.2）陈述能力，映像据此选择；原生构建
   以编译期可得的定义与同一次实测选择。
2. **两个 series 分开。** `patches/`（主 series）只含平台无关的缺陷修正与产品功能，原生与映像都带，不出现 `__OPENKAL__`；
   `openkal/patches/` 只把主 series 的平台接口映射到 openkal，不含优化；openkal 的规范与实现不出现 clangd。
3. **底层一轮做完。** 本轮需要的规范增补、实现、C 库与加载器的改动由一张清单列全（§5–§8），在各自的 CI 中以本层的测试与
   相对原生的基准门禁（§11）证明，然后只出一次引擎候选、只跑一次 mcppls 的完整 CI。
4. **边界不变。** 增补遵守 part 2 §25.10：下层的东西不向上穿透；实现之间可以在下层不同（内核执行或监督执行），规范留下的
   余地都有名字（属性位、陈述的几种结果）。

## 5. openkal 0.20 的规范增补

### 5.1 按偏移的传输（必需）

```c
/* Transfers at `offset' without using or changing the file's position. Version 0.20. */
kal_intptr kal_fs_read_at(struct kal_file, void* buf, kal_uintptr len, kal_u64 offset);
kal_intptr kal_fs_write_at(struct kal_file, const void* buf, kal_uintptr len, kal_u64 offset);
```

- 语义：结果与错误同 `kal_fs_read`/`kal_fs_write`；文件的位置在调用前后不变；同一文件上的两个按偏移传输互不影响位置，
  也不影响并发的顺序传输所见的位置。
- Linux、macOS：`pread`/`pwrite`。Emscripten：WASI `fd_pread`/`fd_pwrite`。
- Windows：同步句柄上带偏移的 `ReadFile` 会把位置移到传输之后，不满足"位置不变"。openkal-windows 改为**自己保存每个文件
  句柄的位置**，所有传输都以显式偏移发出：顺序传输读写保存的位置并前移，按偏移传输不碰它。每次传输仍是一次系统调用；
  `kal_fs_seek` 只改保存的位置。句柄的复制在 openkal 中是 C 库的描述（共享同一 `kal_file`），位置随之共享，与 POSIX 一致。
- 没有文件系统的实现（openkal-opensbi、openkal-uefi）不声明 `kal_fs_*`，不受影响。

### 5.2 程序单元的用量与内存限额

```c
/* kal_program_info gains two fields (self_size grows; present says which are filled). Version 0.20. */
#define KAL_PINFO_MEMORY  ((kal_u32)1u << 4)   /* memory_bytes, memory_peak_bytes */

struct kal_job_usage { kal_u32 self_size; kal_u32 members; kal_u64 memory_bytes; kal_u64 memory_peak_bytes; };
int kal_process_job_usage(struct kal_job, struct kal_job_usage* out);          /* KAL_PROCESS_PROP_JOB */

#define KAL_JOB_LIMIT_MEMORY ((kal_uintptr)1)
int kal_process_job_limit(struct kal_job, kal_uintptr kind, kal_u64 value);     /* optional */

#define KAL_PROCESS_PROP_JOB_LIMIT_HARD       /* the environment enforces the limit */
#define KAL_PROCESS_PROP_JOB_LIMIT_SUPERVISED /* the implementation enforces it by watching */
```

- **用量**：内存的计量由实现陈述（Linux：cgroup 的 `memory.current`，否则成员常驻集之和；macOS：成员 `phys_footprint` 之和；
  Windows：作业的已提交私有内存）。规范只要求同一实现前后一致、单位为字节、`peak` 不减。使用者把它当作护栏，不当作计费。
- **限额**：单元的内存超过 `value` 时，单元的所有成员被结束，`kal_process_wait` 对它们报告的结束原因为"超出限额"
  （`kal_program_info.ended` 增加一个值）。两种执行方式：
  - `HARD`：环境执行。Linux 在调用者所在的 cgroup v2 已委派写权限时，为单元建子组（成员经 `CLONE_INTO_CGROUP` 或
    `cgroup.procs` 进入）并写 `memory.max`；Windows 为作业对象的 `JOB_OBJECT_LIMIT_JOB_MEMORY`。Windows 超出时是分配失败而不是
    结束，实现经作业的完成端口收到 `JOB_OBJECT_MSG_JOB_MEMORY_LIMIT` 后结束单元，使两个系统的可观察结果相同（事件驱动，不采样）。
  - `SUPERVISED`：实现执行。第一次设限时起一个监督线程，按距限额的远近自适应采样（< 50 %：2 Hz；50–80 %：10 Hz；> 80 %：
    50 Hz）。成员是经本实现启动进单元或加入单元的程序，退出以事件跟踪（Linux pidfd，macOS kqueue `NOTE_EXIT`），不扫描进程表。
    超出量的上界为"采样间隔 × 单元的分配速度"，实现在文档中陈述它的间隔。Linux 在 cgroup 未委派时与 macOS 都是这一方式。
  - 两者都不能时报告 `kal_err_not_supported`。
- **代价**：不设限额的程序没有监督线程。16 个成员、每次采样约 10 µs、20 Hz 时约 3 ms/s（一个核的 0.3 %）；`HARD` 没有额外代价。
- 边界核对：cgroup、作业对象、`phys_footprint` 都只在实现内部；上层观察到的是字节数、两种执行方式之一与一个结束原因。

### 5.3 内存区域（可选，`KAL_MEMORY_PROP_REGIONS`）

`kal_alloc` 陈述的是"取得一块可用的内存"，实现建在环境的分配器之上（Linux、macOS 的实现带小尺寸类与一把锁；Windows 建在
进程堆上，超过堆对齐的请求以多分配实现）。虚拟内存的另一组能力——按大粒度对齐地保留地址、按需提交、交还驻留而保留地址、
整段释放——规范至今没有陈述。它是 POSIX `mmap`/`munmap`/`madvise` 与 Win32 `VirtualAlloc`/`VirtualFree` 的公共部分，通用分配器、
垃圾回收的堆、编译器与解释器的区域分配、稀疏的大缓冲区都建在它上面。规范以五个操作陈述它，语义只取自虚拟内存模型：

```c
/* openkal.memory, version 0.20. Offered where KAL_MEMORY_PROP_REGIONS is in kal_memory_props(). */
void* kal_memory_reserve(kal_uintptr size, kal_uintptr align, kal_uintptr flags); /* KAL_MEMORY_COMMIT: usable at once */
int   kal_memory_commit(void* addr, kal_uintptr len);    /* pages become usable; their contents read as zero */
int   kal_memory_decommit(void* addr, kal_uintptr len);  /* resident memory and commit charge returned; commit before reuse */
int   kal_memory_discard(void* addr, kal_uintptr len);   /* resident memory may be reclaimed; stays usable; contents unspecified */
void  kal_memory_release(void* addr, kal_uintptr size);  /* the whole reservation */
kal_uintptr kal_memory_props(void);                      /* KAL_MEMORY_PROP_REGIONS, KAL_MEMORY_PROP_OVERCOMMIT */
```

- 地址与长度是 `kal_memory_granularity()` 的倍数；`align` 是 2 的幂且不小于粒度。访问未提交的页是未定义的。
- `KAL_MEMORY_PROP_OVERCOMMIT`：环境不分开计提交（Linux、macOS），保留即可用，`commit` 不需要系统调用。
- `commit` 之后读到零；`discard` 之后内容未指明（POSIX `MADV_FREE`、Win32 `MEM_RESET` 的语义）。不陈述保护：规范没有对映射
  保护的操作（`memory.h` 已写明），本轮不变。

| 操作 | Linux | macOS | Windows |
|---|---|---|---|
| reserve | `mmap` 多取 `align` 后裁去两端 | 同左 | `VirtualAlloc2` 带 `MEM_ADDRESS_REQUIREMENTS`（不可用时保留后按对齐重试） |
| commit | 无操作（OVERCOMMIT） | 无操作（OVERCOMMIT） | `VirtualAlloc(MEM_COMMIT)` |
| decommit | `madvise(MADV_DONTNEED)` | `madvise(MADV_FREE_REUSABLE)` | `VirtualFree(MEM_DECOMMIT)` |
| discard | `madvise(MADV_FREE)`（内核不支持时 `MADV_DONTNEED`） | `madvise(MADV_FREE_REUSABLE)` | `VirtualAlloc(MEM_RESET)` |
| release | `munmap` | `munmap` | `VirtualFree(MEM_RELEASE)` |

**C 库建在它上面**（openkal-musl，§7），这也是通用性的检验——任何程序自带的分配器（jemalloc、应用的区域分配）都因此得到
真实的语义，而不只是 C 库自己的 `malloc`：

| POSIX | 声明 `REGIONS` 时 | 不声明时（现状） |
|---|---|---|
| 匿名 `mmap`（`PROT_NONE` 只保留） | `reserve`（`PROT_NONE` 不带 `COMMIT`） | `kal_alloc` |
| `munmap`（整段） | `release` | `kal_free` |
| `madvise(MADV_FREE)` | `discard` | 空操作 |
| `madvise(MADV_DONTNEED)` | `decommit`；不带 OVERCOMMIT 时随后 `commit`（POSIX 要求之后读到零且可直接使用） | 空操作 |
| `mprotect` | 不变：`ENOSYS` | `ENOSYS` |

没有虚拟内存的实现（openkal-opensbi、openkal-uefi、openkal-emscripten）不声明 `REGIONS`。

### 5.4 观测（可选，`openkal.prof` 与 `openkal.debug`）

```c
int  kal_prof_available(void);                                                 /* 0 unless the loader profiles */
int  kal_prof_counter(const char* name, kal_uintptr len, volatile kal_u64* cell); /* registered once; read at snapshots */
void kal_prof_mark(const char* name, kal_uintptr len, int begin);              /* a phase's begin or end */
kal_u64 kal_prof_cycles(void);                                                 /* a monotonic counter, cheap */
kal_intptr kal_debug_symbolize(kal_uintptr pc, char* buf, kal_uintptr len);    /* from the image's symbol table */
kal_intptr kal_debug_backtrace(kal_uintptr* pcs, kal_uintptr max);             /* frame pointers */
```

- 加载器不在剖析时，`kal_prof_available` 为 0，C 库与程序不注册任何东西；注册的计数器是程序自己的内存，加载器只在快照时读，
  不增加热路径的代价。
- 只出现次数、时长、镜像内的地址与程序给的名字，不出现环境的标识。

### 5.5 不改的项

线程指针（R6）：实测约 1 %，涉及线程寄存器的约定，属于 openarch；本轮不改规范。决定的依据与数据记录于此。

## 6. 三个实现（openkal-linux 0.21、openkal-macos 0.18、openkal-windows 0.16、openkal-emscripten 0.9）

| 项 | Linux | macOS | Windows | Emscripten |
|---|---|---|---|---|
| §5.1 | `pread`/`pwrite` | `pread`/`pwrite` | 自存位置，全部传输带偏移 | `fd_pread`/`fd_pwrite` |
| §5.2 用量 | cgroup `memory.current` 或 `/proc/<pid>/statm` 之和 | `proc_listpgrppids` + `proc_pid_rusage` | `QueryInformationJobObject` | 不声明 |
| §5.2 限额 | 已委派：HARD；否则 SUPERVISED | SUPERVISED | HARD | 不支持 |
| §5.3 | 见 §5.3 的表 | 见 §5.3 的表 | 见 §5.3 的表 | 不声明 |
| §5.4 | 由 okrun 提供（§8） | 同 | 同 | 不声明 |
| R5 单调时钟 | 经 vDSO（okrun 的 `AT_SYSINFO_EHDR`） | 已是用户态 | 已是用户态 | — |

一致性测试（openkal/conformance）：按偏移传输不改位置、与顺序传输并发；用量单调、成员计数；限额：HARD 与 SUPERVISED 都在
超出后结束全部成员、结束原因可见、超出量不超过实现陈述的上界；区域：对齐、未提交与已提交的边界、`decommit` 后重新 `commit` 读到零、`discard` 后可读写且能测得驻留的系统上驻留下降、`release` 后地址可再保留。

## 7. openkal-musl 0.26

| 项 | 做法 | 证明（§11 L1） |
|---|---|---|
| 分配器（R1） | 两个 feature，恰好一个：`malloc-mimalloc`（默认）、`malloc-mallocng`（musl 自己的，含 0.25.0 的线程缓存，代码与 0.25.0 相同）。两个都开或都不开时编译期报错，并指出依赖边上的写法（`default-features = false` 再选另一个）。mimalloc 以 `compat.mimalloc` 嵌入（§14 的 `embed`，用本库的编译参数与头文件编入本库，不另成一个库，因此没有"C 库依赖分配器、分配器依赖 C 库"的环），适配只在本库的 `port/`（§7.1）。线程记录中的分配器字段两种分配器共用一个指针 | 两个 feature 都跑：1/2/4/8 线程的 malloc+free、LLVM 尺寸分布的回放、每 GB 分配与释放的缺页数；mimalloc ≤ glibc，缺页 ≤ glibc 的 2 倍；mallocng 作为基线记录 |
| 内存的 POSIX 面 | `mmap`/`munmap`/`madvise` 按 §5.3 的表建在内存区域上 | `MADV_DONTNEED` 之后读到零、`MADV_FREE` 之后可写、`PROT_NONE` 保留后不可提交即访问为未定义（只测前两者） |
| 字符串（R2） | `memcpy`/`memmove`/`memset`/`memcmp`/`memchr`/`strlen`/`strchr`/`strcmp`：x86_64 启动时以 CPUID 选 AVX2 或 SSE2 实现，aarch64 用 NEON；短串（≤ 32 B）有不进循环的入口；读取不越过字符串未到达的页 | 0–4096 B、每种对齐，结果与逐字节实现一致；页边界用例；时间 ≤ glibc |
| 定位读写（R3） | `pread`/`pwrite`/`preadv`/`pwritev` 直接调用 `kal_fs_read_at`/`kal_fs_write_at` | 4 线程并发 `pread` 同一描述符：结果正确、位置不变、≤ glibc 的 1.15 倍 |
| 描述符表（R3） | 查找无锁（槽位原子读，带代数）；`openat`/`close`/`dup` 只在保留与发布槽位时持锁，系统调用在锁外；等锁经 futex 而非 `sched_yield` | 8 线程 open/close/stat 的吞吐 ≥ glibc 的 0.85 倍；`sched_yield` 为 0 |
| stat（R4） | 卷属性按设备号缓存，`fstatfs` 每个卷一次 | stat 的系统调用数 = 1 |
| 统计 | 带 `kal_prof_available` 时注册：分配次数与字节、驻留、discard 次数、锁等待、描述符表争用 | okprof 报告中可见 |
| worker 的峰值内存 | `getrusage(RUSAGE_CHILDREN)` 与 `wait4` 的 `ru_maxrss` 取自 `KAL_PINFO_MEMORY` | 单元测试 |

### 7.1 mimalloc 的三处归属

| 包 | 内容 | 不含 |
|---|---|---|
| `compat.mimalloc`（mcpp-index 的内联描述，与 `compat.boost-*` 同一形式） | 上游 v3.6.1 的源码包与 SHA256、构建描述（源文件、宏、include 目录），Linux、macOS、Windows | 任何 openkal 的东西 |
| `mcpplibs.mwrap.mimalloc`（新仓库 `mcpplibs/mimalloc-m`，按 mcpp-index `docs/mwrap.md`） | 依赖 `compat.mimalloc`，把 `mi_*` 与类型映射为模块 `mcpplibs.mwrap.mimalloc`，`MI_*` 映射为 `.macros`，不能映射的宏在 `<mwrap/mimalloc/macros.hpp>`；默认模板；两份 README；版本 `3.6.1`，标签 `mwrap-3.6.1` | port、补丁、实现 |
| openkal-musl（`malloc-mimalloc`） | `port/src/okm_mimalloc_prim.c`：mimalloc 的 OS 层（取代上游 `src/prim/prim.c` 的平台选择，经 §14 的 `exclude` 不编译它），只调用 `kal_*`：内存区域（不声明时退到 `kal_alloc`）、粒度、单调时钟、随机数、`MIMALLOC_*` 环境变量、标准错误、进程信息（`KAL_PINFO_CPU`、`KAL_PINFO_MEMORY`）；保护页与大页回答不支持。`port/src/okm_mimalloc_glue.c`：`malloc` 族的名字、线程堆指针放在线程记录中（上游的 `MI_TLS_PTHREAD_SLOT_OFS`）、`fork` 前后与线程结束的钩子接 musl 已有的位置 | 对上游源码的修改：优先只用上游的扩展点（prim 文件、配置宏）。确需修改时以 §14 的 `patches` 记录在本库，并在 `musl/PATCHES.md` 旁的 `port/mimalloc/PATCHES.md` 写明理由 |

**一个程序一份实现：** 在 `os = "openkal"` 上，选了 `malloc-mimalloc` 的 C 库嵌入了 `compat.mimalloc`；程序若再经 mimalloc-m 依赖
同一变体的 `compat.mimalloc`，按 §14 的规则由嵌入者供给，程序只编译接口；变体不同（C 库带了补丁）则报错，指出两条边。

**测试：** mimalloc-m 的 CI 跑上游的 `test-api`、`test-stress` 与模板（三个系统原生）；mcpp-index 的 installed consumer 测试按 mwrap
的规定。映像上的运行在 openkal-musl 的 L1（`malloc-mimalloc` 与 `malloc-mallocng` 两份）。

## 8. openkal-image 0.2（okrun、okld、okprof）

- **okrun 的剖析**：`okrun --prof=calls,samples[,counters] -o <file>`，或环境变量 `OKRUN_PROF`（映像由其他程序启动时，例如
  mcppls 启动 `clangd.okb`）。
  - `calls`：装入时把 `kal_*` 的导入表项换成计数桩，记录每个接口的次数、总时长与分位数、错误、字节，按线程；不剖析时导入表
    直接指向实现。
  - `samples`：按线程的定时采样，帧指针回溯，以 `.oke` 的符号表还原；Linux 上同时写 `/tmp/perf-<pid>.map`，系统的 perf 可直接
    还原映像的符号。
  - `counters`：§5.4 注册的计数器与阶段标记。
- **okprof**（openkal-tools 中的单独程序）：`okprof record -- <cmd>`、`okprof report`（按层归类：引擎、C 库、libc++、端口层、
  okrun、内核时间）、`okprof flame`、`okprof diff`（映像对原生、两次之间；作为 CI 门禁的输入）。
- **worker 的启动（W1）**：接纳的校验按文件身份记忆。okrun 校验通过后，在用户缓存目录写一条记录（设备、inode、大小、
  `mtime`、`ctime`、清单摘要，记录本身以清单摘要为键）；同一文件再次启动时身份与记录一致即跳过分块校验，只校验清单签名。
  文件被替换或修改则 `ctime` 变化，记录失效。记录只对同一用户有效（目录权限 0700）。验收报告 worker 的启动时间。

## 9. mcppls-clangd 0.0.20（#16，一个 PR）

### 9.1 主 series（`patches/`，原生与映像都带）

- **0038 修正（E2）**：`CompoundProjectModules` 保存 mangler 的标识，`setFileSystem` 重建子对象后转发；单元测试经
  `ModulesBuilder` 的请求路径（含 `setFileSystem`）断言每个清单快照只 mangle 一次。0038 尚未发布，直接修改。
- **按能力选择（E1）**：新补丁（UP-03）把 `#ifdef __linux__` 换成一个平台接口 `ModuleCachePlatform`：
  - 能力：按打开文件持有的字节范围锁、目录句柄的相对操作、打开时可删除、worker 组预算（`HARD`/`SUPERVISED`/无）；
  - 操作：锁范围、相对目录操作、启动 worker、设预算、读用量。
  - 实现：POSIX（Linux 与 macOS 原生，`F_OFD_*` 存在即可）；Linux 的 cgroup 预算在一个单独的文件中；Windows 原生回答"无"，
    走可移植路径。
  - 选择：启动时在缓存目录实测一次（加锁、相对创建与删除、打开时删除），实测失败退到可移植路径并在 `--check` 与日志中
    报告原因。worker 策略与自有缓存额度的默认值由能力决定。
- **可移植路径（E3）**：有租约时不复制；只在拿不到租约的平台读时复制。

### 9.2 openkal series（`openkal/patches/`，只有映像带）

- `ModuleCachePlatform` 的 openkal 实现：能力取自 `kal_fs_props`（`LOCKS`、`REMOVE_OPEN`）与 `kal_process_props`
  （`JOB`、`JOB_LIMIT_*`）；预算经 `kal_process_job_limit`，用量经 `kal_process_job_usage`；不含 cgroup。
- worker 经 `kal_env_loader` 启动（已有，B4）；0017 中映像无地址空间上限的说明改为"由单元的内存限额约束"。

### 9.3 引擎候选之前的定位

- 进程内策略使模块规划变慢的机制（§2.2）：mcppls-clangd 的候选 CI 上传原生引擎的符号包（与发布同一做法），探针以
  `engine-run` + `engine-args` 跑 A、B、B2 三组并符号化，定出进程内路径上多做的工作。若它是主 series 的缺陷，修正进入 §9.1，
  使原生 Windows（仍走进程内）同样受益；本轮的引擎候选只出一次，这一步在候选之前完成。

### 9.4 验收与门禁

- **warm-prime**（新）：LSP 回放一个 176 个模块的工作区（ux-mcpp 的清单与 481 条编译命令），冷一次、已有缓存的新会话三次，
  报告首次就绪、CPU、缺页、worker 启动时间。四个系统 × {原生, 映像}，映像对原生 ≤ 1.15（中位数）。
- **路径对等**：映像在三个系统上报告自有缓存与 worker 为启用；原生 Windows 报告可移植路径（并测其时间，作为记录）。
- `parallel-index`、`module-parallelism` 等已有门禁不变。

## 10. mcppls 0.0.20（#75）

- 引擎的身份增加特性 `owned-module-cache`；mcppls 对报告该特性的引擎给 `--modules-builder-owned-cache-payload-mib`（part 2 B6 的
  条件由"有无 runner"改为"引擎是否具备"），单元测试覆盖四种组合。
- conformance：U2-bmi 认识可移植路径的副本命名（`<stem>-<时间戳>.-<随机>.pcm`）；U11 的锁注入认识
  `producer-chain-v2` 一级目录。可移植路径仍可能出现（原生 Windows，或实测失败的环境）。
- nightly 的 ux 增加 macOS arm64 与 Windows x64 的 ux-mcpp（只在 nightly，不在 PR）。

## 11. 测试与门禁

| 层 | 仓库 | 内容 | 门禁 |
|---|---|---|---|
| L1 原语 | openkal-musl（bench，hosted 与映像各一份，同一机器） | 字符串（长度 × 对齐）、分配（1/2/4/8 线程、LLVM 尺寸分布、缺页数）、锁、线程私有数据、`stat`/`open`/`close`/`pread`（单线程与 4 线程）、时钟 | 映像 / 原生 ≤ 1.15，目标 ≤ 1.0；缺页 ≤ 2 倍 |
| L1 规范 | openkal conformance | §6 所列 | 全部通过，三个系统 |
| L2 中层 | openkal-llvm-runtime（bench） | `OptTable` 解析 481 条真实命令、`StringMap`/`DenseMap` 的增删、Bitstream 读取、Dex 建索引 | 映像 / 原生 ≤ 1.15 |
| L3 引擎 | mcppls-clangd 验收 | §9.4 | 映像 / 原生 ≤ 1.15；路径对等 |
| L4 产品 | mcppls ux | U2、U10、U11、U15 | 预算不变（warm 12 s） |

每层的 CI 都附 okprof 的分层报告（L1–L3），与上一版本做 `okprof diff`。

## 12. 交付与依赖

```
mcpp 2026.11（§14）──────────────────────────────────────────┐
openkal 0.20 ─┬─ openkal-linux 0.21 ──┐                      │
              ├─ openkal-macos 0.18 ──┤                      │
              ├─ openkal-windows 0.16 ┼─ openkal-image 0.2 ──┴─ openkal-musl 0.26 ── openkal-llvm-runtime 0.17 ── openkal-platform 0.21 ── os 0.2.7
              └─ openkal-emscripten 0.9┘          compat.mimalloc 3.6.1（mcpp-index）─┘                                                      │
                                                  └─ mimalloc-m 3.6.1（不在关键路径）                                                         │
                                                       mcppls-clangd 0.0.20（#16）── mcppls 0.0.20（#75）── mcpp-ide 0.2.0（#3）── xim-pkgindex
```

1. mcpp 的 PR（§14）与底层各仓库同时开始；底层开同名分支 `openkal-0.20`，以 working-trees CI 联调；mcpp 的候选二进制供 openkal-musl
   的分支 CI 使用。L1、L2 全部达标前不发布。
2. 引擎 #16 在底层分支上出一次候选（working-trees 构建），探针跑 A/B/C/D 四组（D：新映像），L3 达标。
3. 发布：mcpp → `compat.mimalloc` 与 mimalloc-m 进入索引（`pkgs/c/compat.mimalloc.lua`、`pkgs/mwrap/mimalloc.lua`、
   `tests/mwrap/mimalloc/`）→ openkal → 三个实现 → 索引 → openkal-image → 索引 → openkal-musl → openkal-llvm-runtime → 索引 →
   openkal-platform → os；引擎发布；mcppls 切换锁、完整 CI、发布；mcpp-ide；索引与 xlings 的实机验证。

**并行**：等底层 CI 时写引擎的两个补丁与 warm-prime 验收；等引擎候选时写 mcppls 的 conformance 修正与 nightly 的 ux 矩阵。

## 13. 预期

| 指标 | 现在（C） | 本轮之后 |
|---|---|---|
| warm 就绪（Linux x64） | 26.7–33.1 s | ≤ 原生 × 1.15（约 6 s） |
| warm 的 CPU | ≥ 35.6 s | ≤ 原生 × 1.15（约 3.6 s） |
| 缺页 | 792 k | ≤ 原生 × 2 |
| macOS、Windows | 可移植路径（未测） | 映像走自有缓存与 worker，L3、L4 有数 |

拆分：E1 换到 worker 路径，就绪约 4 倍（B→B2）；自有缓存使就绪后的 CPU 降约 4.7 倍（B2→A）；E2 去掉进程内路径约 33 % 的 CPU；R1–R4 去掉约 1.6 倍（B→C）。

## 14. mcpp 2026.11：依赖的补丁与嵌入、#875、#881

### 14.1 补丁与嵌入

C 库这一类包要用上游的源码而不拷贝它：在源码上加几处改动，或者把它编进自己。现在只能把上游整份放进自己的仓库（openkal-musl
对 musl 正是如此，因为它本身是 musl 的移植），这违背"依赖，不拷贝"。mcpp 在依赖边上增加两个字段：

```toml
[dependencies]
"compat.mimalloc" = { version = "3.6.1", embed = true, exclude = ["src/prim/prim.c"],
                      patches = ["port/mimalloc/0001-xxx.patch"] }
```

**变体。** 打过补丁的库是另一个库：它的身份是 `(包, 版本, 补丁集)`，补丁集的摘要按顺序取各补丁规范化后的内容（不取文件名）。

- 源码：在已校验的上游源码上按序应用，得到变体自己的源码目录（`<cache>/src/<包>/<版本>+p<摘要>`）；任何一段不能精确应用即
  报错，指出补丁与段（不做模糊匹配，结果确定）。补丁格式为 unified diff 或 `git format-patch`，路径相对于包的根目录；应用器
  内置于 mcpp，不依赖系统的 `patch` 或 git，字节精确（换行不转换，由补丁本身决定）。
- 构建、BMI 与缓存目录按变体区分；同一补丁集在不同成员、不同项目中得到同一变体，共享构建。
- `mcpp.lock` 为变体记录版本、各补丁的路径与 sha256 和补丁集摘要；补丁改动即重新解析与构建。
- 只有以源码分发的包能打补丁；只给预编译产物的包拒绝 `patches`。补丁文件属于声明它的包，随它发布。

**变体是一个新的库；约束只在一个程序之内。** 变体的身份与上游不同，mcpp 已有的规则因此直接适用，不需要为补丁另立规则：

- 工作区（docs/05"In a workspace"）拒绝的是"一个身份以两种来源或两个引用被声明"——一次配置只构建一个包的一份检出。两个变体
  是两个身份，两个成员各用一个不触犯这一条；它们在同一次配置中各自构建在自己的目录中。
- 一个程序中不能有两个库提供同一个模块名或同一组符号——这对任意两个库都成立，变体也不例外。同一个包的两个变体（包括"上游"与
  "带补丁"）导出相同的模块名与符号，因此不能在一个程序的链接闭包中相遇。

| 情形 | 结果 |
|---|---|
| 工作区中两个程序各用一个变体 | 允许；构建、BMI、缓存各自独立 |
| 同一程序中同一变体经多条边到达 | 合一 |
| 同一程序中同一个包的两个变体 | 报错，列出引入两者的边，指出它们提供相同的模块名 |
| 已发布的库在公开的依赖边上带补丁 | 允许：它的使用者得到这个变体；与另一变体在同一程序中相遇时按上一行报错 |
| 两个成员以两个补丁集声明同一个包、同时被一个命令选中 | 允许（两个身份）；只有它们进入同一程序时报错 |

**嵌入（`embed = true`）。** 依赖的源文件（按它的构建描述与 feature 选出，再减去 `exclude`）以使用者的编译参数、include 目录与宏
（再加上依赖自己的 include 目录与宏）编入使用者，不另成一个库；依赖不为目标单独构建，因此不需要目标的 C 库——C 库可以嵌入
它所需的代码而不成环。嵌入者即为该变体在程序中的供给者：同一程序中其他边依赖同一变体时，只编译接口、不再链接实现；依赖
另一变体时按上表报错。`exclude` 只在 `embed` 下允许（普通依赖去掉源文件会改变它的公开行为，那应当是一个补丁）。依赖的链接
参数与库不进入程序：定义由嵌入者提供，建立在嵌入者选定的操作系统层之上，链接什么由嵌入者陈述（rev 4，§17.2）。

**与已有机制的关系。** `provides` 的能力选择不变；变体是包身份的扩展，不是新的能力。xim 的安装钩子与描述中的
`generated_files` 在补丁之后执行。先例：vcpkg 的 portfile 补丁、Conan 的 `patches`、Nix 的 `patches`、Spack 的 `patch()`；Cargo 的
`[patch]` 是整份源码替换，粒度不同。

本轮的使用者是 openkal-musl 对 `compat.mimalloc`（`embed`、`exclude`，预期不需要 `patches`）。musl 本身留在 openkal-musl 中：它是
移植而不是几处补丁（`musl/PATCHES.md` 中是成体系的改写与新增的 port 层），以变体表达反而失去它作为移植的可读性。

### 14.2 mcpp#875：外部工具链找不到 mcpp 安装的 Ninja

`ninja_program_for`（`src/build/ninja_backend.cppm`）只在编译器所属的 provider 包旁找 Ninja，找不到就返回裸名 `ninja`，交给 PATH。
mcpp 在首次初始化时已把 Ninja 装进自己的 home（`ensure_ninja`，`xim:ninja@<pinned>`），但这条路径没有回查它；编译器来自外部
路径（`MCPP_TOOLCHAIN=path:…`）时就落到 PATH，PATH 中没有即失败（Windows、MSYS2 的 GCC 复现）。

修正：按序查找——编译器所属 provider 包旁；mcpp home 中按钉住版本安装的 Ninja（`xim_tool_root(env, "ninja")`）；mcpp 的沙箱
`bin`；最后 PATH。都没有时报错列出查过的位置与安装命令，而不是 "cannot start 'ninja'"。`MCPP_NINJA`（`toolchain_env.cpp`，构建
程序看到的）用同一个函数，结果一致。测试：外部工具链 + 清空的 PATH 在三个系统上构建成功；缺 Ninja 时的报错文本。

### 14.3 mcpp#881：`clean --stale` 的报告

`clean_stale()` 对因"比 `--older-than` 新"而保留的目录逐个打印 `kept … not in the record`，没有删任何东西时又打印"每个指纹目录都
记录为当前"——两句矛盾，而后一句是用户据以判断的那句。原因是保留的目录既不计入 `removed` 也不计入 `failed`。

修正：增加 `kept` 计数，摘要在三种情形下都为真（没有可查的、全部当前、保留了 N 个未记录但仍新的目录，并给出 `--older-than`）。
同一 issue 附带的两处一并修正：

- "新"的定义只有一个：`clean --stale` 的 `--older-than` 默认取 `[build] keep_days`（构建后清理用的同一个值，默认 3 天），两个入口
  不再各有默认；文档与 `--help` 同步。
- SubOS 的修复提示写成 `XLINGS_HOME=<registry> xlings self doctor --fix`（与 `src/provider/xlings/xlings.cppm` 已有的说明一致）；
  不带 `XLINGS_HOME` 时，打包的 xlings 会在有系统 xlings home 的机器上修错地方。

补丁变体（§14.1）增加了缓存中的源码目录与构建目录，`clean --stale` 与缓存的回收按变体识别它们：记录中的变体是当前的，不在记录
中且超过 `keep_days` 的变体目录被回收。

### 14.4 交付

一个 PR、一个版本（mcpp 2026.11.x）：§14.1–§14.3、docs（依赖清单的字段、变体与唯一性、`clean`）、测试（补丁的应用与失败、变体
的身份与缓存、同一程序两个变体的报错、工作区两个程序各用一个变体、`embed` + `exclude` 编入使用者且不成环、锁文件、§14.2、
§14.3 的三种摘要）。openkal-musl 0.26 的清单要求该版本（`mcpp = ">=2026.11.x"`），因此它在交付链中排在 openkal-musl 之前（§12）。

## 15. 自我评审

### 15.1 第一轮（rev 1）

| # | 视角 | 发现 | 处理 |
|---|---|---|---|
| 1 | 架构 | `ModuleCachePlatform` 若写在 openkal series 里，主 series 仍以 OS 选择，映像与原生分叉 | 接口与 POSIX 实现在主 series，openkal series 只有映射（§9） |
| 2 | 边界 | 用量的计量三系统不同，若规范写"常驻集"会让 Windows 的 commit 计量不合规 | 计量由实现陈述，规范只要求单位、单调与一致（§5.2） |
| 3 | 边界 | `SUPERVISED` 的超出量若不陈述，使用者无法判断是否足够 | 实现陈述采样间隔，上界写入规范（§5.2） |
| 4 | 跨平台 | Windows 同步句柄的带偏移 `ReadFile` 会移动位置 | openkal-windows 自存位置，全部传输带偏移（§5.1） |
| 5 | 性能 | 分配器保留区域后若不释放驻留页，长时间运行的服务端内存只增不减 | 内存区域的 `discard`/`decommit`（§5.3）与空闲阈值 |
| 6 | 性能 | 自有缓存要求 worker，映像每个 worker 重新接纳 146 MB，冷构建 176 个模块会多出数十秒 | okrun 按文件身份记忆接纳（§8），验收报告 worker 启动时间 |
| 7 | 安全 | 接纳记忆可能被同机其他用户伪造 | 记录在 0700 的用户目录，以清单摘要为键，身份含 `ctime`（§8） |
| 8 | 稳定性 | 能力的编译期判断在挂载的网络文件系统上可能不成立 | 启动时在缓存目录实测，失败退到可移植路径并报告（§9.1） |
| 9 | 兼容与升级 | 旧 mcppls 不给映像额度，新引擎默认值由能力决定 | 默认值在引擎内由能力决定，mcppls 的参数只是覆盖；旧 mcppls 也得到快路径 |
| 10 | 兼容 | `kal_program_info` 增字段 | `self_size` 与 `present` 已为此设计，旧调用方不受影响 |
| 11 | 简洁 | 剖析若做成单独的 okrun 构建，会多一个分发物 | 一个 okrun，剖析关闭时零代价；嵌入式可在编译期去掉 |
| 12 | 测试覆盖 | 缺陷 E2 的单元测试绕过了真实请求路径 | 测试经 `ModulesBuilder` 的请求路径（§9.1） |
| 13 | 测试覆盖 | ux 只在 Linux | nightly 增加 macOS、Windows（§10） |
| 14 | 交付 | 分配器替换的风险最大，若在 L3 才暴露问题会再走一次引擎周期 | L1 用 LLVM 尺寸分布回放与 8 线程压力，L2 跑 LLVM 的数据结构；两层通过才出引擎候选 |
| 15 | 性能 | 分配器的线程堆经"当前线程"取得；映像中当前线程是一次跨映像调用加一次散列查表（R6），在每次分配上约 2–3 ns，与 glibc 的 4 ns 同量级，R6 的"约 1 %"在新分配器下不再成立 | L1 的单线程 malloc+free 门禁直接量它；不达标时 openkal-musl 把线程记录改为按栈对齐定位（线程栈由 C 库按 2 的幂对齐分配，记录在栈底，当前线程为栈指针按位与；主线程与不在 C 库栈上的上下文退回查表）。这是 C 库内部的做法，不改规范，在 L1 内决定，不进入引擎周期 |
| 16 | 性能 | 0017 每构建一个模块起一个 worker（`ModulesBuilder.cpp` 对 `buildModuleInWorker` 每个模块调用一次）；映像的 worker 除接纳外还要把约 60 MB 的段写入 `kal_exec` 区域，冷构建 176 个模块约多 5 s CPU | warm-prime 验收同时以冷阶段为门禁（映像 / 原生 ≤ 1.15）；D 组报告 worker 的启动时间。超出时在 openkal-image 内把只读段改为从映像文件按页映射（`kal_exec` 的文件映射），属于加载器内部，在 L1 内决定 |
| 17 | 兼容 | 分配器替换影响 `malloc_usable_size`、`fork` 的加锁顺序、C 库内部的早期分配 | openkal-musl 的 malloc-threads、fork 示例与 musl 的 libc-test 分配部分作为 L1 的正确性门禁 |
| 18 | 归因的完备 | B2 表明就绪由 worker 策略决定，但进程内路径慢在哪里未定位；若不定位，原生 Windows 与任何实测失败、退回进程内的环境仍慢 | §9.3：候选之前以符号化的原生三组定位，主 series 的缺陷在同一候选中修正 |

### 15.2 第二轮（rev 2）

| # | 视角 | 发现 | 处理 |
|---|---|---|---|
| 19 | 架构 | 只加 `discard` 不够：`kal_alloc` 是带锁的通用分配器，Windows 上建在进程堆上，32 MiB 对齐的请求会多占一倍提交 | 内存区域五个操作（§5.3），`kal_alloc` 不变 |
| 20 | 依赖 | openkal-musl 依赖 mimalloc-m，而映像形态的 mimalloc 需要 C 库，形成环 | `libc` 形态为源码包，只用编译器的独立头文件，经嵌入接口取得 C 库的东西（§7.1）；依赖链为 openkal → mimalloc-m → openkal-musl |
| 21 | 一致性 | 一个映像中若 C 库嵌入一份 mimalloc、程序又经 mimalloc-m 链接一份，会有两个分配器 | 实现的拥有者在 openkal 目标上是 C 库，mimalloc-m 只提供接口（§7.1，符合 mwrap 的"一份实现一个拥有者"） |
| 22 | 一致性 | openkal-musl 的 `none` 目标（openkal-opensbi）没有虚拟内存 | prim 在不声明 `REGIONS` 时以 `kal_alloc` 取已提交内存；所有目标一个分配器，mallocng 整体删除 |
| 23 | 边界 | mimalloc 的 OS 层若经 C 库的 `mmap`/`madvise` 走，会经过描述符表与 POSIX 语义，且 `madvise` 在 openkal-musl 中是空操作 | prim 只调用 `kal_*`（§7.1），不经 POSIX |
| 24 | 性能 | 线程堆指针存在线程记录中，取它要经 `__pthread_self`（R6 的查表） | §15 第 15 条的门禁与后备做法覆盖；L1 的单线程门禁直接量 `mi_malloc`+`mi_free` |
| 25 | 兼容 | openkal-musl 0.25.0 对 mallocng 的改动（`__malloc_release_slot`、`free.c` 的拆分、`pthread_create` 的 flush）随 mallocng 一起删除，musl/PATCHES.md 对应条目改写 | musl 的 PATCHES.md 记录删除及原因；malloc-threads 示例保留为回归测试 |
| 26 | 规范完备 | `kal_memory_props` 是新函数，旧实现没有 | 0.20 起 `openkal.memory` 必须提供它（可以回答 0）；四个实现同时实现 |
| 27 | 交付 | 新仓库首发要过 mwrap 的发布审计（模板、两份 README、许可、installed consumer 测试），比普通依赖多一轮 | mimalloc-m 与 openkal 同时开始，按 mwrap 的检查点提交，不在关键路径的末端 |

### 15.3 第三轮（rev 3）

| # | 视角 | 发现 | 处理 |
|---|---|---|---|
| 28 | 架构 | rev 2 的 mimalloc-m 带 openkal 的 prim 与嵌入接口，接口包承担了实现与平台适配 | 三处归属（§7.1）：compat 只有上游，mwrap 只有接口，适配在使用它的 C 库 |
| 29 | 边界 | rev 2 的 §5.3 以 mimalloc 说明理由，规范像是为一个分配器写的 | 只陈述虚拟内存语义；以 C 库的 POSIX 面建在它上面作为通用性的检验（§5.3） |
| 30 | 语义 | `MADV_DONTNEED` 在 POSIX 下之后可直接使用，规范的 `decommit` 之后要先 `commit` | 不带 OVERCOMMIT 时 C 库在 `decommit` 后随即 `commit`（§5.3 的表）；规范的两个操作各自语义清楚 |
| 31 | 一致性 | 唯一性若按工作区判定，会禁止一个工作区中两个程序合法地使用两个变体 | 变体是新的身份：工作区"一个身份一份检出"的已有规则不受影响；约束是任意两个库都有的"一个程序中模块名与符号唯一"（§14.1） |
| 32 | 一致性 | 打过补丁的库若沿用原包的缓存身份，两个变体会互相覆盖构建与 BMI | 变体是身份的一部分，缓存、BMI、锁按变体（§14.1） |
| 33 | 可复现 | 补丁若允许模糊匹配或依赖系统的 `patch`，不同机器结果不同 | 内置应用器、精确匹配、字节精确，失败即报错（§14.1） |
| 34 | 依赖环 | C 库以普通依赖使用 `compat.mimalloc` 时，后者为目标构建需要 C 库 | `embed`：以使用者的参数编入使用者，依赖不为目标单独构建（§14.1） |
| 35 | 稳定性 | 两个分配器 feature 在依赖边之间合并，可能两个都开 | 恰好一个，编译期报错并给出写法（§7） |
| 36 | 简洁 | `exclude` 若普遍可用，会成为不写补丁而改变依赖行为的后门 | 只在 `embed` 下允许（§14.1） |
| 37 | 兼容与升级 | openkal-musl 0.26 需要新的 mcpp；旧 mcpp 读到 `embed`/`patches` 的行为 | 清单写 `mcpp = ">=2026.11.x"`，旧版本按已有的版本门槛拒绝并提示升级；不认识的成员不会被静默忽略（mcpp 拒绝已知表中的未知成员） |
| 38 | 缓存 | 变体增加源码与构建目录，`clean --stale` 若不识别会误删或漏删 | 与 #881 同一 PR，按变体识别（§14.3） |
| 39 | 用户体验 | #875 的失败信息"cannot start 'ninja'"不指出 mcpp 已经装了 Ninja | 按序查找并在都失败时列出查过的位置与安装命令（§14.2） |
| 40 | 安全 | 依赖边上的补丁改动第三方源码，是供应链上的一个新入口 | 补丁文件属于声明它的包，受该包的校验和覆盖；锁文件记录每个补丁的 sha256；`mcpp sbom` 记录变体、各补丁的路径与 sha256（SBOM 中的 pedigree），供审计 |
| 41 | IDE | 变体与嵌入的源文件在缓存中，mcppls 若不把它们当作受管输入，会对每个文件做完整的输入检查 | 变体源码放在 mcpp 的受管存储之下（与 xpkgs 同一根），mcppls 已有的 `--managed-input-root` 覆盖它；编译数据库中嵌入源文件归属使用者 |
| 42 | 跨平台 | Windows 上补丁与源码的换行、长路径 | 字节精确，不转换换行；变体目录名用短摘要（12 个十六进制字符） |
| 43 | 兼容 | #875 的查找顺序使"外部工具链 + PATH 上另有 Ninja"的用户改用 mcpp 钉住的 Ninja | 这是一致性的修正（同一项目在每台机器上用同一个 Ninja）；CHANGELOG 写明；PATH 仍是最后一项 |
| 44 | 兼容 | #881 附带的统一使 `clean --stale` 的默认窗口由 1 天变为 3 天 | 方向是保留更多，不会多删；CHANGELOG 写明；`--older-than` 照旧可显式给出 |

## 16. 评审

### 16.1 rev 1 待评审项的决定（2026-11-02）

1. 分配器：mimalloc；按 mwrap 规范在 mcpplibs 下新建 mimalloc-m（§7.1）。
2. job 限额的两种执行方式与"超出即结束整个单元"：采纳（§5.2）。
3. 释放驻留页：进入本轮；随第 1 项扩为内存区域（§5.3）。
4. `openkal.prof` 与 `openkal.debug`：都进入本轮（§5.4）。
5. okrun 的接纳记忆与其信任模型：采纳（§8）。
6. 线程指针本轮不改规范：采纳（§5.5）。
7. 引擎不做 LTO/PGO：采纳。
8. 门禁阈值 1.15、目标 ≤ 1.0：采纳。
9. 两项 L1 内的后备做法：采纳（§15 第 15、16 条）。

### 16.2 rev 2 待评审项

1. 内存区域五个操作的签名与语义（§5.3），尤其"`commit` 后读到零、`discard` 后内容未指明"的区分。
2. mimalloc-m 的两种形态与嵌入接口（§7.1）：`libc` 形态以源码包编入 C 库、上游源码以补丁改为调用嵌入钩子。
3. mallocng 整体删除、所有目标一个分配器（§15 第 22、25 条）。
4. mimalloc-m 的版本取上游最新 3.6.1（2026-10-30 发布），而不是 2.x。

### 16.3 rev 2 待评审项的决定（2026-11-02）

1. 内存区域：采纳，但必须是通用的接口与语义——rev 3 改写 §5.3。
2. mimalloc-m 只映射上游或 `compat.mimalloc` 的接口；具体改动在 C 库中——rev 3 改写 §7.1；补丁由 mcpp 的依赖补丁机制承载（§14）。
3. mallocng 不删除，做成 feature——§7。
4. mimalloc 3.6.1——采纳。

### 16.4 rev 3 待评审项

1. 变体的身份与"唯一性按程序判定"的规则（§14.1 的表），尤其"已发布的库在公开依赖边上带补丁"为允许。
2. `embed` 的语义：以使用者的参数编入、嵌入者即供给者、`exclude` 只在 `embed` 下允许。
3. mcpp 的这一版本（§14）进入本轮并排在 openkal-musl 之前；musl 本身仍留在 openkal-musl 中（它是移植而不是补丁）。
4. #881 附带的"新"的定义统一为 `[build] keep_days`（`clean --stale` 的默认因此由 1 天变为 3 天）。

### 16.5 rev 3 待评审项的决定（2026-11-02）

1. 变体的身份与唯一性按程序判定：采纳（§14.1）。
2. `embed` 的语义：采纳（§14.1）。
3. mcpp 的这一版本进入本轮、排在 openkal-musl 之前；musl 留在 openkal-musl：采纳（§12、§14）。
4. "新"的定义统一为 `[build] keep_days`：采纳（§14.3）。

### 16.6 实施中的待决项与决定（2026-11-02）

1. warm-prime 的冷阶段（176 个模块，每个模块一个 worker）映像就绪为原生的 1.63 倍（7.82 s 对 4.80 s），每个 worker 多约 26 ms，
   与模块大小无关：okld 把映像的全部段复制进 `kal_exec_alloc` 的区域，每次启动写入并驻留 54 MB、约 14 000 次缺页。决定：
   openkal 0.21 增加可选操作 `kal_exec_map_part`，实现提供，加载器从文件映射不可写的段（§17.1）。
2. L1 在 Linux x64 上 69 行中 hosted 仍有 5 行、映像 16 行超过 1.15（§17.4），多为纳秒级的字符串函数；L2、L3 达标。决定：L1 在
   CI 中照常运行并报告比值，不作门禁；门禁在 L2、L3。

## 17. 实现中的修订

### 17.1 openkal 0.21 与三个实现

| 项 | 内容 |
|---|---|
| 规范 | `openkal.exec` 增加 `kal_exec_map_part(region, offset, size, file, at, how)`：把区域的一部分换成文件的字节，只读或只读且可执行（`KAL_EXEC_MAP_EXECUTE`），永不可写，随区域由 `kal_exec_free` 释放；文件偏移是 `KAL_MAP_ALIGNMENT` 的倍数，区域内的偏移与长度是 `kal_exec_granularity` 的倍数；失败时区域不变。属性位 `KAL_EXEC_PROP_FILE`（11 条 46、12.2、12.3）。结束的程序的 `cpu_ns` 在句柄释放前可读，与峰值内存相同。设计见 openkal 仓库 `.agents/docs/2026-11-02-openkal-0.21-design.md` |
| openkal-linux 0.22 | `mmap(MAP_FIXED)` 换入文件页；结束的程序的处理器时间取自 `waitid(WNOWAIT)` 的记录。另有三项实现修正：程序以 `CLONE_VM \| CLONE_VFORK` 在独立的 64 KiB 栈上启动，不复制启动者的地址空间，父进程在此期间屏蔽全部信号，子进程把已捕获的信号恢复为默认后再 `execve`；`kal_alloc` 按发出的块切分大块，对齐宽于页时直接映射；名字的校验与终止一次完成，卷属性缓存以序列锁无锁读取 |
| openkal-macos 0.19 | 文件映射在可执行内存可用时声明；处理器时间取自 `proc_pid_rusage`；`kal_alloc` 同 Linux。启动程序保持 `fork`：`vfork` 在 CI 的 macOS 上以 SIGSYS 结束子进程 |
| openkal-windows 0.17 | 回答 `kal_err_not_supported`，不声明该位：保留区中拆出的部分只有在整个保留以占位方式建立时才能换成文件视图，区域会成为需逐个释放的片段表（7.1）。加载器照旧复制 |
| openkal-image 0.3 | okld 从文件映射满足四个条件的段：不可写、文件大小等于内存大小、文件偏移与地址模 64 KiB 同余、独占所在粒度；其余段复制。每个重定位只写入可写段。okrun 在映像运行期间保持文件打开 |

§5.2 的单元按 worker 建立：每个 worker 以 `kal_process_job_enter` 形成自己的单元，以 `kal_process_job_limit` 的内存限额取代
`RLIMIT_AS`（openkal 不陈述地址空间上限）；worker 组的预算仍按 `kal_process_props` 的回答选择。

### 17.2 openkal-musl 0.26 与 openkal-llvm-runtime 0.17

- **互斥锁。** 普通互斥锁按 glibc 的形式等待：`a_swap` 置为"已占用且有等待者"后 futex 等待，解锁时只在有等待者时唤醒。
- **字符串。** AVX2 的 `strlen` 先单独检查第一个向量，再三个一组，之后每次 128 字节；`strcmp` 的逐向量比较以 `min(a, a == b)`
  合并两项检查，按左操作数对齐，在页尾逐字推进；`memcmp` 一次合并至多 256 字节的比较。
- **stat。** 目录的 `fstat` 经 `okm_fs_info(".")` 回答目录的身份（设备号与节点号），引擎测定缓存目录时以它对照 `stat` 的结果
  （§17.3）。`fill_kstat` 先整体清零。描述符的上限等于 `OKM_MAX_FD`。
- **页大小。** x86_64 不再定义常量 `PAGESIZE`：Windows 的分配粒度是 64 KiB，常量 4096 使"映射两页、释放第二页"失败；页大小
  取自运行时。
- **统计。** 计数器的名字表在共用文件中；hosted 的程序不引用 `openkal.prof` 的任何名字（Mach-O 不允许未定义符号），映像在
  启动时注册。
- **分配器的两个 fork 名字。** `__malloc_atfork` 与 `__bump_lockptr` 在 musl 中由 fork.c 以弱别名给出默认定义；本移植在
  Mach-O 与 PE 上不从占位定义生成名字，由提供分配器的源文件定义。mallocng 与 lite_malloc.c 定义了它们，mimalloc 的粘合层
  原先没有，macOS 上链接 fork 的程序失败；现由粘合层按 musl 的默认定义给出。
- **`thread_local` 的析构读到自己的变量。** PE 与 Mach-O 上变量是 compiler-rt emutls 的块，释放它们的 key 早于运行
  `thread_local` 析构的 key 创建，musl 按 key 的顺序调用析构，析构读的是已释放的内存：mallocng 不改已释放块的字节，mimalloc
  在其中写入空闲链表。openkal-llvm-runtime 0.17 的 emutls 在 musl 的最后一轮才释放块（ok 映像的 emutls 在 openkal-musl 中，
  同样处理）。
- **裸机的分配器。** `os = "none"` 上 openkal-llvm-runtime 选 `malloc-mallocng`：mimalloc 按 arena 预留地址空间，在 OpenSBI 的
  内存上返回 `ENOMEM`。目标依赖替换通用依赖，两条目标依赖各自列出 `posix`。
- **嵌入的依赖的链接要求。** `compat.mimalloc` 为 Windows 声明 `psapi`、`user32`、`advapi32` 等库；嵌入 openkal-musl 并以
  `exclude` 去掉 `prim.c` 之后，这些库描述的是不再编译的操作系统层，Linux 上交叉构建 Windows 程序时链接失败。mcpp 2026.11.2.2
  规定嵌入的依赖的链接参数与库属于嵌入者（§14.1 增补；docs/05）。

### 17.3 引擎

| 项 | 修订 |
|---|---|
| 能力的测定（§9.1、§9.2） | 不读 `kal_fs_props`，而在缓存目录经 C 库实测一次：创建、`flock`、按打开文件的锁、相对目录操作、打开时删除。映像中这些调用由 openkal-musl 落到 openkal，测到的就是 openkal 的回答；原生与映像共用一条代码路径。目录的身份以 `fstat(目录句柄)` 对照 `stat(路径)` |
| 根标记的发布 | 以不替换的改名（`renameat2(RENAME_NOREPLACE)`）发布，不建立硬链接：openkal 不为文件建立第二个名字 |
| 打开的缓存 | 同一个根的租约共用一个已打开的缓存（每租约一个缓存在 macOS 上超过 256 个打开文件，在映像上超过 64 个打开目录）；进程可打开的文件数提高到系统上限 |
| 0040 | 自有缓存中的模块在其发布处、凭自身的租约读取，不再为每个读者复制 |
| 0041 | worker 结束即被看到：所有者监视以条件变量等待，worker 退出时通知；父进程查看运行中的 worker 前 50 次间隔 1 ms，此后 10 ms。冷阶段 176 个模块的就绪由 4.9 s 降为 3.4 s，worker 的中位时长由 20 ms 降为 9 ms |
| okgen | 工具链文件变化时删除 `CMakeCache.txt` 与 `CMakeFiles` 后重新配置 |

### 17.4 验收的形态与结果

- **warm-prime 的工作区**（§9.4）：由 ux-mcpp 的 `mcpp emit build-database` 生成，304 个单元、176 个模块（另有 std）。处理器时间
  与缺页在 Linux 上取 `/proc/<pid>/stat` 的 `utime`、`stime`、`cutime`、`cstime` 与对应的缺页字段，其他系统为引擎自身的时间
  加 worker 日志中各 worker 的时间。门禁在 Linux 与 macOS（两个引擎都走自有缓存与 worker）；Windows 原生走可移植路径，只记录。
- **L3（Linux x64）**：映像对原生，冷阶段就绪 1.04、CPU 1.08、缺页 1.12；已有缓存的阶段 1.04、1.09、1.12。
- **L2**：Linux x64 的 hosted 与映像全部 ≤ 1.15。CI 的映像对比交替运行两侧各五次、逐行取最小值：同一侧连续运行时，共享
  机器的一段干扰只落在一侧（macOS arm64 的 options/command 一次为 1.22，重跑为 0.85）。
- **L1（报告）**：Linux x64，对 glibc。hosted（GCC 编译）69 行中 5 行超过 1.15：线程私有数据 1.20、stat 1.16、clock 1.21（多一层
  openkal 调用，约 2 ns）、4 线程的 open-close 1.16、memcmp/64/7 1.19。映像（Clang 编译）16 行超过，集中在字符串函数
  （memchr/4096 1.46、memcpy/4096 1.34、strcmp/64 1.29），另有 clock、stat 与线程私有数据；同一份源码以 Clang 编成 hosted 时为
  9 行，映像多出的部分大半来自编译器而不是映像。这些差距在 L2 的工作负载中不显现。

