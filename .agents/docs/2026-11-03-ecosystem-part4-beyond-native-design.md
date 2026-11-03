---
subject: openkal-performance
status: active
---

# 生态设计 part 4：超越原生——openkal 之上与之下的优化、可观测性与安全

日期：2026-11-03（rev 3，定稿）。基线：openkal 0.23 分支（`openkal-0.23`：`kal_exec_alloc_at`、placement note、
`kal_fs_resolved_name`）、openkal-image 0.4.0 分支、openkal-musl 0.27.0 分支、mcppls-clangd #16（引擎 23.1.0-mcppls.9）。
前文：[part 3](2026-11-02-ecosystem-part3-ok-image-performance-parity-design.md) §16.7、§16.8、§17。

**修订记录**

- rev 1：综合 2026-11-03 的讨论与测量。已定：放置之外的五个方向（A 最小授予、A' 不变目录、B 精确输入集、C 按触页顺序排布、
  D 初始化后快照）全部实现并入本轮；Windows 的文件页映射与 hosted 启动成本两项对等缺口并入本轮；调试、性能分析、可观测性与
  安全同轮完成。分层原则（§3）：openkal ABI 是内核接口，实现层是内核，程序只经由 openkal 请求与使用能力；不以运行器拦截
  `kal_*` 调用的方式实现功能。
- rev 2：§17 的四项按推荐定下（§17.1），本文定稿，按 §15 实施。
- rev 3：资源偏好（§4.8、§16）：程序的资源偏好 balanced（默认，自适应）、performance（以空间换时间）、economy（以时间换空间），
  由 openkal 表达、各层兑现；可用内存的查询。并入本轮。

---

## 1. 结论

1. 放置（part 3 §16.8）之后，clangd 映像一次启动的处理器时间与原生相同（0.77 ms 对 0.76–0.85 ms），内核态少 13%。
   此前看到的 2 倍差距中，一半来自 0.4.0 的一处回退：加载器每次装入都执行 `okld_check`，新增的 placement 内容校验占用户态
   指令的 57%；内容校验移到封装时的 `okld check` 后差距消失（§2.1）。
2. 余下的差距不在处理器时间而在缺页（519 对 284）与 warm 阶段的文件系统查询。缺页的三分之一是静态初始化写满的
   `.data`/`.bss`，三分之一是两层分配器的堆（§2.2）；warm 阶段约 30 万次失败的查找来自 clang 驱动对宿主 GCC 安装的探测，
   原生与映像相同（§2.3）。
3. 这些差距原生程序无法消除，映像体系可以：映像对环境的一切访问都经过 openkal，加载器、C 库与封装都在体系之内。
   本文以 openkal 的能力（§4）、映像格式（§5）与各层的实现（§6–§9）给出 A、A'、B、C、D，并把调试、性能分析、可观测性
   （§10）与安全（§11）放在同一组能力上。
4. 三项安全缺口与原生相比是退步，本轮补齐：重定位后的只读数据（RELRO）仍可写；放置使每个映像的地址全球固定；映像未以
   栈保护等加固选项编译（§11）。

## 2. 测量

### 2.1 启动（本机 x86-64，`taskset -c 6`，P 核，`perf stat -r 100`，`clangd --version`）

| | task-clock | 用户态周期 | 内核态周期 | 缺页 |
| --- | --- | --- | --- | --- |
| 原生（非 PIE） | 0.76–0.85 ms | 1.09 M | 3.00 M | 284 |
| 映像，放置于基址（0.4.0 修正后） | 0.77 ms | 1.53 M | 2.63 M | 532 |
| 映像，任意地址（`OKRUN_PLACE=0`） | 1.54 ms | 3.40 M | 4.97 M | 1 103 |
| 映像，放置于基址（修正前） | 1.52 ms | 4.90 M | 3.37 M | 594 |

修正前的用户态指令为 1 740 万条，其中 57% 在 `okld_check`：`okld_load` 每次装入都执行完整的格式检查，0.4.0 在其中加入了
对存储段逐字的校验（16.4 万条 RELR 的展开与 2 MB 的比较）。修正：`okld_check` 只检查 placement 描述映像，内容校验成为
`okld_check_placement`，由封装时的 `okld check` 与 `okld bundle check` 调用。存储段与映像的指令同样可信，运行时的完整性由
签名或 okb 的摘要负责。

### 2.2 缺页的构成（同一启动，`perf record -e page-faults -d`，519 次）

| 位置 | 缺页 | 说明 |
| --- | --- | --- |
| 映像 `.data`/`.bss` | 179 | 静态初始化写入（LLVM 的 `cl::opt` 对象）；加载器另将 `.bss` 全部填零 |
| 堆与匿名映射 | 176 | okrun 的分配器、映像的 mimalloc、openkal-linux 的 `kal_alloc` |
| 映像指令（文件页） | 84 | |
| 映像只读数据（文件页） | 31 | |
| RELRO（存储段与复制的首尾块） | 28 | |
| okrun 与栈 | 21 | |

hosted 形态的 okrun（openkal-musl，静态）单独启动 185 次缺页，glibc 动态链接的同一 okrun 为 73 次；task-clock 相同（0.18 ms）。
其中 85 次来自 `okm_table_init` 对 1 024 个描述记录的逐个写入（340 KiB 的 `.bss`）。

### 2.3 warm 阶段的文件系统查询（`strace -f -c`，3 个会话，CI probe 的 L3 数据）

| | 原生 | 映像（0.3.1） |
| --- | --- | --- |
| stat 类 | 35.3 万（失败 18.9 万） | 87.3 万（失败 19.1 万） |
| `openat` | 18.2 万（失败 10.5 万） | 18.8 万（失败 10.5 万） |
| `readlink` | 9.9 万 | — |

映像多出的 50 万次 stat 来自 realpath 的逐段询问，0.23 的 `kal_fs_resolved_name` 已去掉。两者共有的约 30 万次失败查找落在
`/usr/lib/gcc/x86_64-linux-gnu/{9,11,12,13}/include`、`/usr/include/c++`、`/usr/lib32`、`/etc` 等：编译命令以
`--sysroot=~/.mcpp/registry/subos/default` 使用 mcpp 的 gcc 16，clang 驱动仍在每个 worker、每条命令上探测宿主的 GCC 安装。
探测的结果还使 LSP 的结论依赖宿主装了什么，与构建不一致。

### 2.4 静态初始化的份额

映像自身的代码占 okrun 进程用户态周期的约 35%（约 0.11 ms），其中约三分之一是 musl 与 mimalloc 的初始化（描述表、逐项
`getenv` 的选项解析），其余是 LLVM 的静态初始化（约 0.07 ms）。D 的收益因此主要在缺页与内存：初始化写满的 `.data`/`.bss`
与初始化期间的堆成为文件页，按需读入、在进程间共享（每个 worker 约 1.5 MB 的私有页）。

### 2.5 现有缺口

| 缺口 | 现状 |
| --- | --- |
| RELRO | 加载器重定位后不改变可写段的保护，GOT 与 `.data.rel.ro` 在运行时可写；原生的 `-z now` + RELRO 使其只读 |
| 地址 | 放置使一个映像的地址在所有机器上相同，等同非 PIE；原生发行版的程序多为 PIE + ASLR |
| 加固 | 平台的编译选项有栈冲突保护，无栈保护、无自动变量初始化、无 CET/BTI 标记、无 libc++ 加固 |
| 回溯 | 映像不保留帧指针；perf 的 DWARF 回溯止于映像内的叶子帧，okrun 没有崩溃报告 |
| Windows 文件页 | openkal-windows 不声明 `KAL_EXEC_PROP_FILE`，每次启动复制全部 37 MB 指令 |
| Windows stat | 一次成功的 `kal_fs_info`：`NtCreateFile`、四次 `NtQueryInformationFile`、`NtClose` |
| 观测 | okprof 计 `kal_*` 调用与计数器；没有采样、触页、阶段与跨进程的时间线；macOS、Windows 上系统的采样器不认识映像 |

## 3. 分层与原则

1. **openkal ABI 是内核接口。** 实现层（openkal-linux、-macos、-windows）对上即内核：今天是进程内的库，将来可以是直接实现
   openkal 的内核。一项需要环境配合的能力表达为 openkal 的操作、标志或属性，由实现层完成；同一映像与同一组请求在两种内核上
   含义相同。
2. **映像格式是加载器的契约。** 加载器今天是 okrun/libokld，将来可以是内核的程序装入。放置、触页集合、快照以第 12 条的
   note 表达。
3. **程序与 C 库内部的优化不需要下层。** 描述表的惰性初始化、realpath 等属于此类。
4. **不以运行器拦截 `kal_*` 实现功能。** 记录、缓存、只读都是能力的属性，由实现层在每次调用中完成。okprof 对 `kal_*` 调用的
   计数是观测 ABI 边界本身，属于边界的绑定者，保留。
5. **系统调用合并的判据。** openkal 的边界今天是函数调用，将来可能是真实的系统调用；一个常见操作应一次跨越。合并只在减少
   内核切换、句柄创建或往返时采用（§12）。
6. **能力以属性位声明，缺席有确定的语义。** 每项新能力：实现声明则可用；未声明时请求被拒绝或作为不生效的提示，规范写明是哪
   一种。
7. **证明而不是假设。** 快照、放置缓存、记录等以可检查的条件成立；条件不成立时回到普通路径，而不是报错或冒险。

## 4. openkal 0.23 的增补（ABI）

0.23 已有：`kal_exec_alloc_at`、`KAL_EXEC_PROP_PLACE`（11 条 48）、`kal_fs_resolved_name`、`KAL_FS_PROP_RESOLVED_NAME`（49）。
本文增补如下，均进入 0.23.0，条文为 11 条 50–57。

### 4.1 exec 区域为零（陈述，无新名字，11 条 50）

`kal_exec_alloc` 与 `kal_exec_alloc_at` 返回的区域，字节为零。三个实现都以新映射的页满足（`mmap` 匿名、`VirtualAlloc` 提交）。
加载器据此不再写 `.bss`：未触及的 `.bss` 页不驻留，与原生相同。

### 4.2 只读封存：`kal_exec_seal_part`（安全，11 条 51）

```c
/* Makes a part of a region read-only and not executable: a loader's
 * read-only-after-relocation data once relocated. KAL_EXEC_SEAL_FOREVER asks
 * that neither the protection nor the mapping change again for the program's
 * life, where the environment can hold that (KAL_EXEC_PROP_SEAL_FOREVER);
 * elsewhere the part is read-only as asked. Version 0.23. */
int kal_exec_seal_part(void* region, kal_uintptr offset, kal_uintptr size, kal_uintptr flags);
#define KAL_EXEC_SEAL_FOREVER      ((kal_uintptr)1u << 0)
#define KAL_EXEC_PROP_SEAL_FOREVER ((kal_uintptr)1u << 4)
```

| 实现 | 只读 | `FOREVER` |
| --- | --- | --- |
| Linux | `mprotect(PROT_READ)` | `mseal`（6.10 起；属性按内核在运行时判定） |
| macOS | `mprotect(PROT_READ)` | `mach_vm_protect(set_maximum=TRUE, VM_PROT_READ)` |
| Windows | `VirtualProtect(PAGE_READONLY)` | 不声明 |

加载器对入口映像（不卸载）请求 `FOREVER`，对 `dlopen` 的映像只请求只读（卸载要释放区域）。

### 4.3 预取与 BTI：`KAL_EXEC_MAP_PREFETCH`、`KAL_EXEC_MAP_BTI`（C、安全，11 条 52）

`kal_exec_map_part` 的 `how` 增加两位：

```c
#define KAL_EXEC_MAP_PREFETCH ((kal_uintptr)1u << 1) /* the part's bytes are read in now */
#define KAL_EXEC_MAP_BTI      ((kal_uintptr)1u << 2) /* executed with branch-target enforcement */
#define KAL_EXEC_PROP_BTI     ((kal_uintptr)1u << 5) /* KAL_EXEC_MAP_BTI is enforced */
```

`PREFETCH` 把逐页的缺页合并为一次请求：Linux `MAP_POPULATE`，macOS 映射后 `madvise(MADV_WILLNEED)`，Windows
`PrefetchVirtualMemory`。它是提示，不改变语义。`BTI` 只对带 `EXECUTE` 的部分有意义：Linux arm64 内核支持时以 `PROT_BTI`
映射并声明 `KAL_EXEC_PROP_BTI`，其余环境忽略该位。

### 4.4 能力衰减与隔离：`kal_fs_dir_attenuate`、`KAL_SPAWN_CONFINE`（A、A'、B、安全，11 条 53）

```c
struct kal_dir_rights {
    kal_u32         self_size;   /* set by the caller (clause 5.3) */
    kal_u32         reserved;    /* zero */
    kal_uintptr     flags;       /* KAL_DIR_* */
    struct kal_file record;      /* KAL_DIR_RECORDED: where reached names are appended */
};
#define KAL_DIR_READ_ONLY  ((kal_uintptr)1u << 0)  /* nothing beneath is created, written, removed or renamed */
#define KAL_DIR_UNCHANGING ((kal_uintptr)1u << 1)  /* the caller states nothing beneath changes while the handle lives */
#define KAL_DIR_RECORDED   ((kal_uintptr)1u << 2)  /* each name reached beneath is appended to `record' */

int kal_fs_dir_attenuate(struct kal_dir d, const struct kal_dir_rights* rights, struct kal_dir* out);

#define KAL_FS_PROP_ATTENUATE     ((kal_uintptr)1u << 12)  /* kal_fs_dir_attenuate is answered */
#define KAL_SPAWN_CONFINE         ((kal_uintptr)1u << 5)   /* the environment holds the started program
                                                            * to its grants and their rights */
#define KAL_PROCESS_PROP_CONFINE  ((kal_uintptr)1u << 14)  /* KAL_SPAWN_CONFINE is honoured */
```

- 派生的句柄只能更少：属性只增不减，`READ_ONLY` 不可撤销；由它打开的目录继承属性。
- 经 `kal_spawn.grants` 授予时属性随之传递，启动的程序在 `kal_fs_props` 中读到 `KAL_FS_PROP_READ_ONLY`、
  `KAL_FS_PROP_UNCHANGING`（新位 13、14）。
- **只读**由实现层在每个修改操作中强制（`kal_err_permission`）。
- **隔离**是启动时的请求：带 `KAL_SPAWN_CONFINE` 启动的程序，由环境把它限制在授予的目录与各自的权限之内，即使它不经由 openkal
  访问文件；声明 `KAL_PROCESS_PROP_CONFINE` 的实现才接受该标志，其余实现拒绝（`kal_err_not_supported`），不静默降级。
  Linux 以 Landlock（5.13 起）在启动的程序执行之前设定规则；macOS 以 `sandbox_init_with_parameters` 的 SBPL 规则；Windows 不声明。
  隔离由调用者请求而不是随授予自动生效：一个动态链接的原生程序需要授予之外的运行时目录，只有知道这一点的调用者才请求它。
  mcppls 启动引擎（静态的 okrun 与 payload 中的映像）时请求隔离：被恶意仓库利用的语言服务器只能触及项目、payload 与缓存，
  读不到 `~/.ssh`。
- **不变**是授予者的陈述。实现可以从已读到的答案回答此后的查询：目录列表作答不存在的名字，已取得的属性作答 stat。缓存在
  实现层，对上透明；原生 openkal 内核以同一陈述省去重新验证。
- **记录**：每个经由该句柄（及其下打开的目录）到达的名字，以 `<种类><名字>\0` 追加到 `record`，种类为 `r`（为读打开）、
  `w`（写、创建、删除、改名）、`s`（询问，存在）、`a`（询问或打开，不存在）、`l`（列举）；名字相对于被衰减的目录。实现
  在进程内去重并成批写入，程序结束时写 `e\0`；没有 `e` 的记录是不完整的，使用者按"未知"处理。多个进程向同一文件追加
  （`O_APPEND`、`FILE_APPEND_DATA`），每条不超过一次原子写。

### 4.5 带属性的列举：`kal_fs_list_next_info`（A'、合并，11 条 54）

```c
/* kal_fs_list_next, and what the listing told of the entry: the fields
 * `wanted' asks for that the environment gave with the entry are filled and
 * stated in `info->present'; a field it did not give is asked of
 * kal_fs_info. KAL_FS_PROP_LIST_INFO: the listing gives size, modified time
 * and identity without a further enquiry. Version 0.23. */
int kal_fs_list_next_info(struct kal_dir, kal_uintptr* iter, char* name_out, kal_uintptr name_cap,
                          kal_uintptr* name_len, kal_u32 wanted, struct kal_node_info* info);
#define KAL_FS_PROP_LIST_INFO ((kal_uintptr)1u << 15)
```

macOS 以 `getattrlistbulk`、Windows 以 `NtQueryDirectoryFile(FileIdBothDirectoryInformation)` 一次给出一批条目的全部属性；
Linux 的 `getdents64` 只有类型，`wanted` 要求其余字段时逐项 `statx`（不声明该位，但仍是一次 ABI 跨越）。

### 4.6 页面用量：`KAL_PINFO_FAULTS`（观测，11 条 55）

`kal_program_info` 增加缺页的两个计数（`faults_minor`、`faults_major`，追加在结构末尾，`self_size` 判定）。Linux
`getrusage`/`/proc/self/stat`，macOS `task_events_info.faults`，Windows `PROCESS_MEMORY_COUNTERS.PageFaultCount`（只有总数，
`major` 不声明）。okprof 以它给出每个阶段的缺页。

### 4.7 故障报告与采样（调试、观测，11 条 56）

`openkal.debug` 与 `openkal.prof` 至今由加载器提供给映像（12.3）。故障与采样需要环境的机制（信号或异常处理、
处理器时间定时器、挂起线程），属于实现层：0.23 起三个实现也向本环境的程序提供这两个接口，okrun 即其中之一；okrun 向映像
提供它们时，计数与阶段由自己记录，采样转交给它的实现（okrun 绑定给映像的是 `own_kal_*`，与实现导出的名字不冲突）。

```c
/* openkal.debug */
#define KAL_DEBUG_PROP_FAULT_REPORT ((kal_uintptr)1u << 0)
kal_uintptr kal_debug_props(void);

/* openkal.prof */
#define KAL_PROF_PROP_SAMPLES ((kal_uintptr)1u << 0)
#define KAL_PROF_SAMPLE_DEPTH 64u
struct kal_prof_sample {
    kal_u64     time_ns;      /* the monotonic clock */
    kal_u64     context;      /* kal_task_current() of the sampled context */
    kal_u32     depth;        /* frames in `pc', innermost first */
    kal_u32     reserved;
    kal_uintptr pc[KAL_PROF_SAMPLE_DEPTH];
};
kal_uintptr kal_prof_props(void);
int        kal_prof_sample_begin(kal_u32 per_second);   /* every context of this program */
kal_intptr kal_prof_sample_take(struct kal_prof_sample* out, kal_uintptr count); /* drains; returns how many */
int        kal_prof_sample_end(void);
```

- **故障报告**：声明 `KAL_DEBUG_PROP_FAULT_REPORT` 的实现，以故障结束程序时（非法访问、非法指令、栈溢出）向程序的错误流写出
  故障的种类与地址，以及帧指针链给出的每一帧：地址、持有它的文件与文件内偏移（环境映射文件之处）、该文件为 ok 映像
  （第 12 条）或程序自身时的符号名与偏移。写完后以环境的默认处理结束程序，core 照常产生。Linux/macOS 在备用栈上处理
  `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE`（`/proc/self/maps`、`mach_vm_region` + `proc_regionfilename` 给出文件），Windows 以向量化
  异常处理（`GetMappedFileNameW`）。原生内核对应于内核的故障报告。
- **采样**：`kal_prof_sample_begin` 后，实现按频率采样本程序的全部上下文，记录 PC 与帧指针回溯；`take` 取走已采的样本，
  `end` 停止。Linux 与 macOS 以整个程序的处理器时间定时器（`setitimer(ITIMER_PROF)`）与 `SIGPROF`：信号送到正在使用
  处理器时间的上下文，其上下文中即有 PC 与帧指针（`perf_event_open` 受 `perf_event_paranoid` 限制，CI 与许多机器上不可用）；
  Windows 以采样线程 `SuspendThread` + `GetThreadContext`。环满时新的样本不记录。符号化在采样之外（okprof 按映像与程序的符号表），
  同一份火焰图在三个系统上得出，不依赖系统的采样器是否认识映像。

### 4.8 资源偏好与可用内存（11 条 57）

```c
/* openkal.process: in kal_spawn.flags; none of the three inherits the caller's. */
#define KAL_SPAWN_RESOURCES_BALANCED    ((kal_uintptr)1u << 6)
#define KAL_SPAWN_RESOURCES_PERFORMANCE ((kal_uintptr)1u << 7)
#define KAL_SPAWN_RESOURCES_ECONOMY     ((kal_uintptr)1u << 8)

/* openkal.env: the preference this program was started with. */
#define KAL_RESOURCES_BALANCED    0
#define KAL_RESOURCES_PERFORMANCE 1
#define KAL_RESOURCES_ECONOMY     2
int kal_env_resources(void);

/* openkal.memory: what the environment can give this program now. */
struct kal_memory_status {
    kal_u32 self_size;        /* set by the caller (clause 5.3) */
    kal_u32 present;          /* KAL_MSTATUS_* */
    kal_u64 total_bytes;      /* the most this program may use: the machine's, or a limit set on it */
    kal_u64 available_bytes;  /* what can be given without taking it from another program */
};
#define KAL_MSTATUS_TOTAL     ((kal_u32)1u << 0)
#define KAL_MSTATUS_AVAILABLE ((kal_u32)1u << 1)
int kal_memory_info(struct kal_memory_status* out);
```

- 偏好是程序与环境之间的请求，与 `kal_task_set_priority` 同属一类（各系统的 QoS 类别）。启动时设定，被启动的程序继承；
  多于一位为 `kal_err_invalid`。实现以自己的方式传递（Linux、macOS 为 `KAL_RESOURCES` 环境变量，与 `KAL_PREOPENS` 相同；
  手工运行时用户也以它设定）。
- 实现层在程序开始时按偏好调整环境的机制：economy 时 Linux `prctl(PR_SET_THP_DISABLE)`、Windows
  `SetProcessInformation(ProcessMemoryPriority, MEMORY_PRIORITY_LOW)`；performance 时不限制。实现自己的缓存上限按偏好取值。
- `kal_memory_info` 给出本程序可用的上限与当前可得：Linux `MemAvailable` 与所在 cgroup 的 `memory.max` 取小，macOS
  `hw.memsize` 与 `host_statistics64`，Windows `GlobalMemoryStatusEx` 与作业对象的限额，emscripten 线性内存的上限与余量。
  均衡档以它自适应（§16）。

### 4.9 一致性套件

| 条 | 观察 |
| --- | --- |
| 4.1 | 新区域每页的首字节为零 |
| 4.2 | 封存后读到原值；声明 `FOREVER` 的实现：再次 `kal_exec_publish_part` 该部分被拒绝 |
| 4.3 | 带 `PREFETCH` 的映射成功，内容与不带时相同 |
| 4.4 | 只读句柄上的创建、写打开、删除、改名为 `kal_err_permission`，读成功；属性只增不减；由其打开的目录继承；记录中出现
      `r`、`a`、`l`、`e` 且去重；`UNCHANGING` 的句柄上已列举目录中不存在的名字为 `kal_err_not_found` |
| 4.5 | 列举给出的字段与 `kal_fs_info` 相同；`present` 只含给出的位 |
| 4.6 | 触及 N 页后 `faults_minor` 至少增加 N/2（考虑预读与大页） |
| 4.7 | 能力字只含分配的位 |
| 4.8 | 未设定时为 balanced；以三个标志之一启动的副本读回该值；不带标志时继承；两个标志为 `kal_err_invalid`；`available_bytes` ≤ `total_bytes` |

隔离（Landlock、SBPL）由各实现自己的测试观察：带 `KAL_SPAWN_CONFINE` 启动的子进程以原始系统调用越过 openkal 写只读授予中的文件、读授予之外的文件，均被环境拒绝；未声明该属性的实现拒绝该标志。

## 5. 映像格式（第 12 条，版本 1.1）

| 类型 | note | 内容 |
| --- | --- | --- |
| 3 | placement（已有） | 封装时的基址与存储段；本机缓存不可用时的后备 |
| 4 | touch（C） | 启动触及的页：若干 `(偏移, 页数)`，以 64 KiB 为单位，按首次触及的顺序；加载器以 `KAL_EXEC_MAP_PREFETCH` 预取 |
| 5 | snapshot（D） | 只出现在本机缓存的文件中，不出现在发布的映像中（§6.4） |

签名（类型 2）覆盖整个文件，不变。type 4 由封装写入，前于签名。

## 6. 加载器与运行器（openkal-image 0.4.0）

### 6.1 已在 0.4.0 分支

`okld place`、placement note、加载器按基址放置、`okld_check_placement`（只在封装时）、`okrun` 的 `reserve_at`。

### 6.2 装入路径

1. 不写 `.bss`（§4.1）。
2. 符号重定位之后，以 `kal_exec_seal_part` 封存 `PT_GNU_RELRO`：入口映像请求 `FOREVER`。映射自文件的 RELRO 块本来只读。
3. 有 type 4 note 时，映射指令与只读数据的那些部分带 `KAL_EXEC_MAP_PREFETCH`。
4. `okrun_resolve` 以完美散列查找（构建时由 SURFACE 生成），代替逐项比较。

### 6.3 本机放置缓存（安全与共享）

参照 macOS dyld 共享缓存按开机滑动的做法，默认策略：

- 每个用户、每次开机一个随机滑动 `S`（64 KiB 的倍数，在 [16 TiB, 32 TiB) 中）；映像的基址为 `P(digest) + S`（模 16 TiB）。
- 缓存目录由启动者授予（mcppls 授予 `<cache>/okrun`），或取 `OKRUN_CACHE`、`XDG_RUNTIME_DIR`/`TMPDIR` 下以 0700 新建的
  `okrun-<user>`。滑动记在目录中，附带创建时的墙上时间与单调时间：二者之差变化超过 1 s 即为新的一次开机（单调时钟随开机
  归零），重新选择。
- 当次开机第一个装入该映像的进程，以文件中的存储段为起点按 `S` 重新计算相对重定位（或从文件字节计算），写入
  `<digest>-<base>.okseg`（先写临时文件，再改名），然后照常运行。此后的进程映射这个文件的页。
- 缓存文件以内容摘要命名，创建时由创建者校验（`okld_check_placement` 的同一算法），读取时校验头部的映像摘要与基址。同一用户
  的其他进程能改写缓存，但它们本来就能调试该用户的进程，不增加能力。
- 缓存不可用（只读环境、没有私有目录）时使用封装时的基址；`OKRUN_PLACE=sealed` 固定使用它，`OKRUN_PLACE=0` 放到任意地址。

### 6.4 初始化后快照（D）

快照与放置缓存在一起，由本机生成：

1. **生成**：缓存中尚无快照时，运行器在当次开机第一次装入后，以 `okrun --seal-local` 另起一个进程生成，原进程照常运行。
2. **封装运行**：在基址处装入映像；以一个只提供内存的 openkal 实现运行 C 库的第一阶段初始化与全部静态初始化：
   - `kal_alloc`、`kal_region_*` 由紧接映像跨度之后的初始化区供给（封装进程在 `基址 + 跨度` 处预留）；映像本身不带这段地址，
     平时的装入不预留它，Windows 的提交额度不受影响；
   - `kal_task_current` 给出固定的主上下文；
   - 其余任何 `kal_*` 调用都使封装放弃。
3. **证明**：初始化结束后扫描可写段与初始化区，若有字指向封装进程的栈或 `kal_*` 桩所在的地址段，封装放弃。符号重定位在
   装入时重新施加，静态数据中对 `kal_*` 的引用因此总是正确。
4. **写出**：可写段与初始化区已用部分中非零的 64 KiB 块，加 type 5 note（基址、块表、主上下文的线程记录位置、第二阶段入口
   `__okm_libc_start`）。
5. **装入**：缓存有快照时，加载器在 `基址 + 跨度` 处只预留初始化区实际用到的部分，映射快照块（私有、写时复制），施加符号
   重定位，不运行 `DT_INIT_ARRAY`，把初始化区的范围写入映像导出的 `__okm_snapshot_arena`，先调用第二阶段入口（C 库读取参数、
   环境、preopen、标准流，把当前上下文绑定为快照中的主上下文，以 `kal_random_fill` 设定栈保护的守卫值），然后进入 `ok_main`。
   C 库的分配器不把初始化区内的地址交还环境（它们不是本进程 `kal_region_reserve` 的结果）。
6. **语义差异**：mimalloc 的 `MIMALLOC_*` 环境变量在快照中已按无环境读取，不再生效；文档写明，需要它们时以
   `OKRUN_SNAPSHOT=0` 运行。

### 6.5 运行器的观测

| 模式（`OKRUN_PROF`） | 内容 |
| --- | --- |
| `calls`、`counters`（已有） | `kal_*` 调用与时间；程序的计数器与阶段 |
| `phases` | 准入、装入、重定位、初始化、`main`、退出各阶段的时间与缺页（§4.6） |
| `samples[=hz]` | §4.7 的采样，帧指针回溯，按映像符号化 |
| `touch` | 触页顺序：指令与只读数据先以无访问保护映射，首次访问时由运行器记录并恢复保护（Linux/macOS 信号、Windows 向量化异常），用于 C |
| `names` | 程序经由 `RECORDED` 授予到达的名字（§4.4）的汇总：按目录统计不存在的查找 |
| `perfmap`（已有） | Linux 的 `/tmp/perf-<pid>.map` |

`okprof report --format=trace` 输出 Chrome/Perfetto 的时间线：每个进程（worker）一行，阶段、标记、采样与缺页在同一时间轴上。
`okprof report --flame` 输出折叠栈。

### 6.6 调试

1. **默认可调试，零成本**：Linux、macOS 上指令与只读数据是映像文件的页，`/proc/<pid>/maps` 与 core 的 `NT_FILE` 写明映像文件
   与基址。`okgdb.py` 与 `oklldb.py`（随 openkal-tools 发布）读这些映射，以 `add-symbol-file <image> -o <base>`、
   `target modules load --slide` 载入符号；对 core 同样有效。
2. **复制的映像**（Windows 在 §7.3 之前、任意地址）：GDB JIT 接口（已有，`--debugger`）。
3. **调试信息分离**：平台以 `--build-id` 链接；`okld debug-split <image>` 写出 `<build-id>.debug` 与精简后的映像，布局与
   debuginfod 相同（`.build-id/xx/rest.debug`），okgdb 按 build-id 查找。
4. **故障报告**：§4.7，帧指针回溯，`kal_debug_symbolize` 命名。

## 7. 实现层（openkal-linux 0.24.0、openkal-macos 0.21.0、openkal-windows 0.19.0、openkal-emscripten 0.12.0）

### 7.1 共同

`kal_exec_seal_part`、`PREFETCH`、`kal_fs_dir_attenuate`（属性随 `KAL_PREOPENS` 一类的授予传递，在每个修改操作与路径解析中
检查）、`UNCHANGING` 缓存（目录列表与已取属性，以句柄与相对名字为键）、`RECORDED` 记录（去重集合、64 KiB 缓冲、退出时写
`e`）、`kal_fs_list_next_info`、`KAL_PINFO_FAULTS`、故障报告、采样。

### 7.2 Linux

`mseal`（`ENOSYS` 时不声明 `FOREVER`）、`MAP_POPULATE`、Landlock（ABI 版本按内核探测；规则：授予的层级按读写权限，另加
`/proc`、`/dev/null` 与被启动的程序文件本身的执行权限）、`statx`、`SIGPROF` 采样。

### 7.3 Windows

1. **文件页映射**（`KAL_EXEC_PROP_FILE`）：`kal_exec_alloc` 以 `VirtualAlloc2(MEM_RESERVE | MEM_RESERVE_PLACEHOLDER)` 保留占位，
   `kal_exec_map_part` 拆分占位后以 `NtMapViewOfSectionEx(MEM_REPLACE_PLACEHOLDER)` 映射文件的节（以执行权限重开文件，节为
   `SEC_COMMIT` + `PAGE_EXECUTE_READ`/`PAGE_READONLY`），其余部分以 `VirtualAlloc2(MEM_REPLACE_PLACEHOLDER | MEM_COMMIT)` 提交；
   `kal_exec_free` 逐段释放。需要 Windows 10 1803 起，函数动态解析，缺失时不声明该位。
2. **按名字查属性**：`NtQueryInformationByName(FileStatInformation)` 一次给出文件 ID、时间、大小、属性、链接数（Windows 10 1709
   起）；卷序列号取自基目录的句柄（缓存在句柄上）；Windows 11 24H2 的 `FileStatBasicInformation` 直接给出卷序列号。
   `KAL_FS_NO_RESOLVE` 与函数缺失时回到打开句柄的路径。
3. **带属性的列举**：`NtQueryDirectoryFile(FileIdBothDirectoryInformation)`，声明 `KAL_FS_PROP_LIST_INFO`。
4. `PrefetchVirtualMemory`、`VirtualProtect`、`PROCESS_MEMORY_COUNTERS`、向量化异常的故障报告、采样线程。

### 7.4 macOS

`mach_vm_protect(set_maximum)`、`MADV_WILLNEED`、`getattrlistbulk`（声明 `LIST_INFO`）、SBPL 沙箱（`sandbox_init_with_parameters`，
不可用时不声明 `GRANT_CONFINED`）、`task_events_info`、采样线程。

### 7.5 emscripten

只提供 `openkal.fs` 的新增：`kal_fs_dir_attenuate`（只读、记录在实现内；不变的缓存）、`kal_fs_list_next_info`（逐项 `fstatat`）；
`openkal.exec`、`openkal.process` 仍不提供。

## 8. C 库（openkal-musl 0.27.0）

1. 描述表惰性初始化（已改）；realpath（已改）；`dlopen` 的 `reserve_at`（已改）。
2. **两阶段初始化**：`__okm_libc_init` 拆为与环境无关的第一阶段（表、分配器、主上下文的线程记录）与第二阶段
   `__okm_libc_start`（参数、环境、preopen、标准流、栈保护守卫）。普通启动两阶段相继执行；快照装入只执行第二阶段。
3. **初始化区**：封装运行中，mimalloc 的操作系统层经由封装者的 `kal_region_*` 自然得到初始化区的地址；快照装入后，
   落在 `__okm_snapshot_arena` 范围内的地址不交还环境，也不丢弃：这段内存是加载器的映射，不是本进程 `kal_region_reserve`
   的结果，规范对它没有释放操作；它的大小是初始化期间的堆（clangd 约 1–2 MB）。
4. **栈保护**：`__stack_chk_guard` 为全局（`-mstack-protector-guard=global`），第二阶段以随机值设定；设定它的函数自身不受保护。
5. mimalloc 选项：一次扫描环境，没有 `MIMALLOC_` 前缀的变量时直接返回。
6. `posix_spawn` 传递的授予：父进程的 preopen 原样授予（含其属性）；`O_PATH` 等不变。

## 9. 平台与消费者

### 9.1 openkal-platform 0.23.0

| 组 | 内容 |
| --- | --- |
| build | 加 `-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`、`-fstack-protector-strong -mstack-protector-guard=global`、`-ftrivial-auto-var-init=zero`、`-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST`；x86_64 `-fcf-protection=full`，aarch64 `-mbranch-protection=standard` |
| link | `--build-id=sha1`；`-z pack-relative-relocs`（已有）；包声明的触页顺序文件（`--symbol-ordering-file`）|
| seal | `okld note`、`okld place`、`okld touch`（有触页数据时写 type 4）、`okld check` |

BTI 与 CET 只发出标记；Linux arm64 上加载器以 `KAL_EXEC_MAP_BTI`（§4.3 同一 `how` 的位 2，内核支持时 `PROT_BTI`）映射指令，
其余环境不强制。帧指针与加固的处理器时间成本以 L2 门禁衡量（§13）。

### 9.2 mcpplibs.os 0.2.8

`spawn_options` 增加 `grants`：`{ name, path, rights }` 的列表（`rights` 为只读、不变、记录的组合与记录文件），映射到
`kal_fs_dir_attenuate` 与 `kal_spawn.grants`；未给出时保持实现的默认授予。

### 9.3 mcppls 0.0.20

1. **最小授予**：启动引擎时授予项目根（第一项、读写）、编译数据库中出现的每个目录（`-I`、`-isystem`、`-iquote`、`--sysroot`、
   `-B`、`-resource-dir`、编译器的安装前缀；只读、不变）、引擎 payload（只读、不变）、模块缓存与临时目录（读写）、okrun 的
   缓存目录（读写）。宿主的 `/usr` 只在编译命令使用宿主编译器时出现在其中。
2. **精确输入集**：模块 worker 的授予带 `RECORDED`，记录写在模块载荷旁；复用载荷时检查记录中 `a` 的名字现在是否存在、`r` 的
   文件是否变化，任一成立即重建。这覆盖了 clang 的 PCM 校验不覆盖的情形：搜索路径靠前的目录中新增的同名头文件。
3. 引擎崩溃时，mcppls 的日志包含实现的故障报告。

### 9.4 引擎（mcppls-clangd 0.0.20）

CI 增加触页训练：以 `OKRUN_PROF=touch` 运行 warm-prime 的冷阶段（Linux x64、arm64），合并各进程的首次触及顺序得到函数顺序
文件，重新链接（目标文件复用），封装时写 type 4。顺序文件是 CI 的产物，不提交。

### 9.5 mcpp

`docs/` 中 openkal 平台一节说明加固、帧指针、本机放置缓存与授予；`CHANGELOG`；内置平台钉住 0.23.0。

## 10. 调试、性能分析与可观测性（汇总）

| 需求 | 手段 | 所在层 |
| --- | --- | --- |
| 断点、单步、变量 | okgdb/oklldb 从映射载入符号；复制的映像用 JIT 接口；分离的调试信息按 build-id | 工具 |
| 崩溃 | 故障报告（帧指针回溯 + 符号化）；core 加 okgdb | 实现 |
| 热点 | `samples`：三系统一致的采样与火焰图；Linux 另有 perf（文件页可符号化，帧指针可回溯） | 实现 + okprof |
| 启动 | `phases`：各阶段时间与缺页；`touch`：触页顺序 | 加载器 + okprof |
| ABI 边界 | `calls`：每个 `kal_*` 的次数与时间 | 绑定者 |
| 文件系统 | `names`：到达的名字、不存在的查找按目录汇总 | 实现（`RECORDED`） |
| 多进程 | `--format=trace`：一个时间线上的全部 worker | okprof |

## 11. 安全（汇总）

| 项 | 原生（发行版常见） | 本轮之后的映像 |
| --- | --- | --- |
| 地址随机化 | PIE + ASLR，每进程 | 每用户每次开机的滑动，同一次开机内共享（§6.3） |
| RELRO | 只读 | 只读，入口映像永久（§4.2） |
| W^X | 有 | 有（12.2，已有） |
| 栈保护 | `-fstack-protector-strong` | 同，全局守卫，第二阶段随机设定 |
| 自动变量初始化 | 部分发行版 | `-ftrivial-auto-var-init=zero` |
| 标准库加固 | `_GLIBCXX_ASSERTIONS`（部分） | libc++ 加固 fast 模式 |
| CET/BTI | 标记，部分强制 | 标记；Linux arm64 强制 BTI |
| 文件系统范围 | 用户的全部文件 | 授予的目录；只读由实现强制，请求隔离时由环境强制（§4.4） |
| 完整性 | 包管理器 | 映像与 okb 的签名（已有）；本机缓存由创建者校验 |

## 12. 系统调用合并的判断

| 候选 | 判断 |
| --- | --- |
| 按名字查属性 | 已是一次 ABI 调用；Windows 实现由 6 次系统调用改为 1 次（§7.3） |
| 带属性的列举 | 采纳（§4.5）：macOS、Windows 一次给出一批；为原生内核保留一次跨越 |
| 预取 | 采纳（§4.3）：把启动时数十次缺页合并为一次请求 |
| 进程内作答 | 采纳：最小授予之外的名字由 C 库以无 preopen 作答；不变授予由实现缓存作答 |
| 通用批量提交（io_uring、IoRing） | 不采纳：LLVM 的文件访问同步逐个进行，调用者无法成批；io_uring 在容器的默认 seccomp 与部分发行版被禁用，IoRing 只支持读 |
| 整文件读取 | 不采纳：Linux 上内部仍是同样的系统调用，收益只在 Windows 的一次查询 |

## 13. 验证与门禁

1. **一致性套件**：§4.9；各实现的本地一致性与 CI。
2. **openkal-image CI**：放置缓存（第一个进程写、第二个进程映射、换滑动后重建）；快照（生成、装入、输出与普通启动相同、
   证明失败的映像回到普通路径）；RELRO 封存后写入被拒绝；预取；`touch`、`samples`、`phases` 的输出；okgdb 从 core 载入符号。
3. **openkal-musl CI**：两阶段初始化；初始化区；栈保护的守卫在第二阶段设定；授予属性经 `posix_spawn` 传递。
4. **安全测试**：Linux 上以原始系统调用越过 openkal 写只读授予中的文件被拒绝（Landlock）；macOS 同（SBPL）；栈溢出被栈保护
   终止并有故障报告。
5. **L2（引擎 warm-prime 门禁）**：Linux x64/arm64、macOS、Windows 四个平台；加固与帧指针的成本计入；预期见 §14。
6. **L3**：warm 阶段 `strace -c`：失败查找的数量与来源；缺页。

## 14. 预期

| 指标 | 现在（本机） | 预期 |
| --- | --- | --- |
| 一次启动的缺页 | 532（原生 284） | 约 250（快照 −150，`.bss` 惰性，预取合并指令页） |
| warm 阶段失败查找 | 约 30 万（原生同） | 约 0：最小授予之外进程内作答，授予之内由不变缓存作答 |
| 每个 worker 的私有页 | 约 1.5 MB 初始化数据 | 文件页，跨进程共享 |
| Windows 每次启动 | 复制 37 MB 指令 | 文件页 |
| Windows 一次 stat | 6 次系统调用 | 1 次 |
| 帧指针与加固的成本 | — | 处理器时间 ≤ 2%（L2 计入） |

## 15. 交付

| 仓库 | 版本 | PR |
| --- | --- | --- |
| openkal | 0.23.0 | #61（同一 PR 增补） |
| openkal-linux / -macos / -windows / -emscripten | 0.24.0 / 0.21.0 / 0.19.0 / 0.12.0 | #48 / #39 / #43 / #14 |
| openkal-image | 0.4.0 | 新 PR（分支 `openkal-0.23`） |
| openkal-musl | 0.27.0 | 新 PR |
| openkal-llvm-runtime | 0.18.0 | 新 PR（加固与帧指针随平台） |
| openkal-platform | 0.23.0 | 新 PR |
| mcpplibs.os | 0.2.8 | 新 PR |
| mcppls-clangd | 0.0.20 | #16 |
| mcppls | 0.0.20 | #75 |
| mcpp | 2026.11.2.3 | #884 |

依赖顺序与 part 3 的版本组级联相同：openkal 与实现 → 索引 → image → musl → llvm-runtime → platform → os → 引擎 → mcppls →
mcpp-ide → mcpp。openkal 的改动在 CI 通过后做多角度的规范评审（规范、ABI、设计意图、架构）。

## 16. 资源偏好：各层的兑现

默认为 balanced；performance 以空间换时间，economy 以时间换空间。每一项在三档下各有确定的取值，单项设置优先于档位；
balanced 按 `kal_memory_info` 与缓存目录的余量自适应：资源充足时取偏快的值，可得内存低于总量的 15% 或缓存目录余量低于
1 GiB 时取 economy 的值。

| 兑换点 | 层 | performance | balanced | economy | 单项设置 |
| --- | --- | --- | --- | --- | --- |
| 本机放置缓存 | 加载器 | 保留 | 保留，按开机重建 | 不写，每次重定位 | `OKRUN_PLACE` |
| 初始化后快照 | 加载器、C 库 | 保留，上限 2 GiB | 上限 512 MiB，LRU | 不生成 | `OKRUN_SNAPSHOT` |
| 启动页预取 | 加载器 → 实现 | 触页集合与全部 RELRO | 触页集合 | 不预取 | `OKRUN_PREFETCH` |
| 不变目录缓存 | 实现 | 列表与属性 | 列表与属性，64 Ki 条目 | 只缓存不存在 | `KAL_FS_CACHE` |
| 分配器的保留 | C 库 | 归还延迟 1 s，堆区域用大页 | mimalloc 默认（10 ms） | 立即归还 | `MIMALLOC_*`（快照装入时除外，§17.1） |
| 透明大页 | 实现 → 环境 | 不限制 | 环境默认 | 关闭 | — |
| 内存优先级 | 实现 → 环境 | 正常 | 正常 | 低 | — |
| worker 并发 | mcppls | 核数 | 核数的一半 | 2 | `mcppls.workers` |
| 模块缓存 | mcppls | 4 GiB | 1 GiB | 256 MiB，载荷压缩 | `mcppls.moduleCache` |
| okb 形态 | 打包 | 存储 | 压缩传输，安装时展开为存储 | 压缩存放，首次运行展开到缓存 | mcpp.toml 的打包设置 |

- **入口**：构建期的项（触页 note、顺序文件、okb 形态）在 mcpp.toml 的平台节；运行期由启动者请求，mcppls 以
  `mcppls.resources = "balanced" | "performance" | "economy"` 设定，并经 `KAL_SPAWN_RESOURCES_*` 传给引擎及其 worker。
- **观测**：okprof 的 `phases` 报告每个兑换点的命中、占用与节省（时间、缺页、字节）；`okrun cache` 列出与回收本机缓存，
  `mcpp cache gc` 一并回收。
- **衡量**：L2 门禁以 balanced 运行；另跑 performance 与 economy 的对照，报告峰值 RSS、磁盘占用、处理器时间与墙钟时间，
  写入本文。

## 17. 待评审项

1. **默认放置策略**：本文以每用户每次开机的滑动与本机缓存为默认（安全），封装时的基址为后备。另一选择是以封装时的基址为默认、
   滑动为可选（每次开机的第一次启动少一次重定位）。
2. **快照的语义差异**：`MIMALLOC_*` 环境变量对快照装入不生效（§6.4.6）。
3. **加固的默认**：libc++ 加固 fast 模式与 `-ftrivial-auto-var-init=zero` 对引擎的处理器时间影响以 L2 衡量；若超过 2%，是否
   保留为默认。
4. **授予的推导**：mcppls 从编译数据库推导授予（§9.3）。编译命令依赖宿主编译器的隐含路径时，授予包含该编译器的安装前缀。
   是否另给用户一个追加授予的设置（`mcppls.grants`）。

### 17.1 决定（2026-11-03）

1. 默认放置策略为每用户每次开机的滑动与本机缓存；封装时的基址为缓存不可用时的后备。
2. 快照装入时 `MIMALLOC_*` 环境变量不生效，写入 openkal-image 与 openkal-musl 的文档；需要它们时以 `OKRUN_SNAPSHOT=0` 运行。
3. libc++ 加固 fast 模式与 `-ftrivial-auto-var-init=zero` 为平台默认，成本写入 L2 报告；包可以在自己的清单中关闭。
4. mcppls 提供 `mcppls.grants`：用户追加的目录（只读或读写），`"all"` 回到实现的默认授予。

## 18. 实现记录（2026-11-03）

本节记录实现与 §4–§16 的差别及其理由，以及实现中发现并修正的缺陷。§4–§16 的正文保留为设计；凡与本节冲突，以本节为准。

### 18.1 快照（D，openkal-image 0.4，docs/snapshot.md）

1. **生成**：运行器在当次开机第一次以本机放置启动映像、而缓存中没有 `<digest>-<base>.oksnap` 时，独占创建
   `<digest>-<base>.sealing`，以 `OKRUN_SNAPSHOT=start` 重新启动自身（同一参数与环境，标准流为空设备）；该进程以
   `OKRUN_SNAPSHOT=make` 再启动一个进程后立即结束，原进程只等待前者，生成快照的进程不比任何父进程活得久。标记文件记录结果
   （`made` 或 `refused: <原因>`），同一次开机不再尝试。`--seal-local` 未采用。
2. **C 库的入口**：映像的 C 库导出记录 `__okrun_snapshot {arena, size, phase, reserved, context}` 与入口
   `__okrun_snapshot_start`，代替 §6.4 的 `__okm_snapshot_arena` 与 `__okm_libc_start`：名字属于运行器的协议而不属于某个 C 库。
   phase 1 为生成时初始化之后，C 库报告快照不能保存的东西（例如初始化代码设置的环境变量）；phase 2 为从快照启动，C 库取得
   参数、环境、流、目录、随机数与栈保护守卫，并把快照中的上下文记为当前上下文。
3. **生成时的绑定**（run/src/seal.cpp）：内存操作由初始化区供给；实现自身的属性（`*_props`、粒度、版本、接口）作为事实；
   `kal_task_parallelism`、`kal_task_cores`、`kal_env_resources` 与按名读取的环境变量作为记录的回答；其余调用使生成放弃，
   标记写明调用名。事实与被问到的回答折叠为一个字，启动时与本次的值比较，不同则不使用快照（报告中的 runner 一项写明原因）。
   被读取的环境变量以名字与值的摘要保存在快照文件中：LLVM 在静态初始化中读取 `LLVM_OVERRIDE_PRODUCER`，原设计的"其余调用即
   放弃"会使 clangd 永远没有快照。
4. **证明**：只检查两件事：每个导入槽仍是装入时写入的值（`okld_bound_imports`）；其余可写数据与初始化区中没有任何字等于绑定
   的地址。原设计的栈地址检查被删除：短字符串的复制携带其未用字节，常含已返回栈帧的地址，产生误报；这些地址在任何启动之后都
   指向已返回的帧，不构成快照特有的风险。
5. **初始化区**：位于映像跨度之后、按 256 MiB 对齐的边界两侧；区域与 64 KiB 以上对齐的分配在边界之上按地址本身对齐
   （mimalloc 以地址掩码找区域元数据，需要地址对齐而不是偏移对齐），其余小分配在边界之下向下放置；上限 1 GiB。
6. **装入**：只重新施加命名导入的重定位（`applying::imports`）；快照的块以 `KAL_EXEC_MAP_WRITE` 私有映射，不预读（预读快照块
   的内核时间超过其节省，§18.4）。
7. **测量**：clangd `--version`：从快照启动 311 次缺页，不用快照 442 次，处理器时间少 15%。

### 18.2 放置、触页与调试

1. **本机放置缓存的位置**：`OKRUN_CACHE`，否则 `$XDG_RUNTIME_DIR/okrun`（该目录每次开机由系统清空），否则用户缓存目录的
   `okrun/placed`；滑动与开机时刻记在其中的 `boot` 文件。§6.3 的 `TMPDIR` 下 `okrun-<user>` 未采用：用户缓存目录已只属于该用户。
2. **触页**：经由 ABI（`kal_prof_touch_*`）由实现记录，`okprof record --what touch` 取得，`okld touch` 写入 type 4 note，
   `okprof report --order` 输出符号顺序文件；note 由生产者（引擎 CI 的训练步骤）写入，运行器不在用户机器上记录触页。
3. **数据段**：存储段以 `KAL_EXEC_MAP_WRITE`（私有可写映射）映射文件的页，代替复制。
4. **okprof 与调试脚本**：okprof 成为 openkal-tools 的第三个程序，列入平台的 `[platform.tools]`；`okgdb.py`、`oklldb.py` 随
   release-binaries 发布为一个归档。

### 18.3 实现层

1. **Windows（openkal-windows 0.19，§7.3）**：文件页映射以占位实现：区域是页文件节的视图，映射文件的部分拆分占位后换为文件
   视图，其余部分以同一节在原偏移的视图保持内容；部分须按 64 KiB 对齐，否则 `kal_err_not_supported`。`KAL_EXEC_PROP_SEAL_FOREVER`
   不声明：视图的持有者总能改变保护。按名字查属性用 `NtQueryInformationByName`（24H2 的 `FileStatBasicInformation` 带卷序列号，
   1709 的 `FileStatInformation` 对单一分量的名字用基目录的卷序列号），记录不能回答时打开节点。`KAL_SPAWN_CONFINE` 被拒绝：
   该系统只能经由文件的安全描述符限制程序，而一次启动不改变它们。
2. **实现中发现的缺陷**：
   - openkal-macos 0.21.1：`ulock_wait` 读取被等待的字时不处理缺页，对从未写过的零页返回 `EFAULT`；`kal_time_sleep` 等待
     一个静态零字，每次等待立即失败，睡眠成为整段时长的自旋（arm64 与 x86_64 运行机上睡 200 ms 耗费 127–200 ms 处理器时间）。
     由 openkal-musl 的 posix 探针（`CLOCK_PROCESS_CPUTIME_ID` 跨 `nanosleep`）发现，原生探针确认。等待在 `EFAULT` 时读取该字
     后重试；睡眠的字位于栈上。
   - openkal-macos 0.21.2：单调时钟粒度原为每次启动测得（41、42 ns 或更大），快照的事实字因此在两次启动间不同，macOS 上的
     快照被间歇拒绝。粒度改取 `mach_timebase_info` 的计时单位向上取整。
   - openkal-macos 0.21.3：该格式按链接顺序运行初始化函数，平台的 hosted 安排把 C 库链接在实现之前，C 库的初始化函数读取
     程序的目录时实现尚未记录环境，启动者授予的目录（及其权限）被实现的默认目录（整个文件系统、可写）取代（mcpplibs.os 的
     hosted macOS 测试：被授予只读目录的程序在其中创建了文件）。实现在尚未记录时从加载器的记录（`_NSGetArgc`、`_NSGetArgv`、
     `_NSGetEnviron`）取参数与环境；一致性测试从链接在实现之前的初始化函数提问。
   - openkal-musl 0.27.0：Windows 上以卷开头的名字与持有目录的名字逐字节比较，`c:\Users\...` 到达不了名为 `C:` 或
     `C:/Users/...` 的目录。卷下的名字改为不区分大小写、两种分隔符等价（`examples/image-names`）。

### 18.4 资源偏好（§16 的实现值）

| 兑换点 | performance | balanced | economy |
| --- | --- | --- | --- |
| 本机放置缓存 | 写入并保留 | 写入并保留 | 在内存中计算，不写缓存 |
| 快照 | 生成，总量上限 2 GiB，LRU | 生成，上限 512 MiB，LRU | 使用已有的，不生成 |
| 预取（`OKRUN_PREFETCH=0\|touch\|all`） | 级别 2：触页集合与数据 | 级别 1：触页集合 | 0 |
| mimalloc 归还延迟 | 1 s | mimalloc 默认（10 ms） | 0 |
| mcppls worker（`engine.workers=auto`） | 硬件线程数，至多内存 GB 数的一半 | 原有公式：线程数减一，2–8，至多内存 GB 数的一半 | 2 |
| mcppls 缓存预算（`cache.maxBytes` 未设置时） | 8 GiB | 4 GiB（原默认） | 1 GiB |

balanced 在可得内存低于总量 15% 或缓存目录所在设备余量低于 1 GiB 时由运行器按 economy 处理。与 §16 的差别：

1. **balanced 保持原有取值**（worker 公式与 4 GiB 预算）。升级不改变未设置偏好的用户所得的行为；§16 中 balanced 的 1 GiB 与
   "核数的一半"会使现有用户的缓存与并发在升级后缩小。
2. **economy 不压缩模块载荷**。clang 以映射读取 BMI，压缩的 BMI 在每次读取时都要展开为一份副本，这与缓存已有的读时副本重复，
   以时间换得的空间在读取期间又被占用；economy 改为较小的预算。
3. **预取不覆盖快照块**：测得预读快照块与可写数据所费内核时间超过其节省；数据只在 performance 预读。

`mcpp cache gc` 调用最新的 `okrun cache gc` 回收运行器的缓存（其他开机的副本与快照）；`okrun cache [list|gc [--all]]` 列出与回收。
压缩的 okb（okb 2，DEFLATE）由 `mcpp pack --pkg-format okb --compress gzip` 经 `MCPP_PACK_COMPRESS` 请求，运行器首次运行时展开到
用户缓存的 `okrun/expanded`。

### 18.5 授予与精确输入集（§9.3）

1. **授予（mcppls 0.0.20）**：ok 映像引擎得到项目根（可写，第一项）、mcppls 的缓存与数据库目录、临时目录与运行器的缓存（可写）、
   引擎自身的目录（只读、不变）、包仓库与系统根目录（只读）、编译数据库提到的目录（只读），以及 `mcppls.grants` 追加的目录；
   被链接指向的目录以两种名字给出。数据库在引擎启动之后才推送，故启动时的集合覆盖工具链与头文件的常见位置；之后的数据库提到
   未覆盖的目录时，引擎带着扩大的集合重启（重启原因 plan）。`mcppls.grants = ["all"]` 回到运行器的默认（整个文件系统）。
   `mcppls.engine.confine` 默认关闭；系统不能执行时（Windows、无 Landlock 的 Linux），引擎只由 openkal 限制并在日志中说明。
2. **精确输入集（B，mcppls-clangd openkal 0003）**：模块 worker 通过 C 库的 `okm_preopen_attenuate`（openkal-musl 0.27.0）为
   自己持有的每个目录请求 `KAL_DIR_RECORDED`，记录写在 worker 单元的 `inputs-<n>`。worker 结束后，clangd 取完整记录中查找
   未果的名字（`a`），去掉 worker 自身单元之下的名字，连同 worker 开始前的墙上时间写在 BMI 旁（`<bmi>.inputs`），先于 BMI
   发布。复用 BMI 之前，若其中的名字现在存在则重建；目录最后修改早于 worker 开始的，其下没有新增名字，只对修改过的目录逐名
   查询。与 §9.3 的差别：`r` 项不检查，因为读取过的文件就是 BMI 的输入，clang 对它们的大小、时间与内容的校验已覆盖；检查之后
   在 recheck 间隔内不重复（与验证结论的缓存相同）。owned 缓存把 `payload.pcm.inputs` 计为代次的载荷。本地 Linux 上的 ok 映像：
   在搜索路径更靠前的目录新建同名头文件后，日志为 `Rebuilding module good: …/first/lib.h was absent when the module was built,
   and is present now`；四个平台的验收各运行一次（`ci/module_inputs_canary.py --absent-names-only`）。
3. **故障报告**：mcppls 把实现的故障报告（`openkal: fault: ...` 与 `openkal:   #N ...`）作为崩溃上下文保存，不受日志限流。

### 18.6 平台的编译选项

aarch64 用 `-mbranch-protection=pac-ret+bti`，不用 §9.1 的 `standard`：clang 23 的 `standard` 含受保护的控制栈
（定义 `__ARM_FEATURE_GCS_DEFAULT`），libunwind 因此按 GCS 展开并包含 C 库的 `<arm_acle.h>`，映像的构建在 aarch64 上失败；
GCS 还需要系统为程序启用该栈，映像没有这一环节。返回地址签名与 BTI 标记不变。

加固选项在平台的 CI 中暴露了两处下层缺陷，各在其所属的层修正：

1. **compiler-rt 的 outline atomics（openkal-llvm-runtime 0.18.1）**：生成的 125 个 `oa_*.S` 先按自身路径包含 compiler-rt 的
   `assembly.h`，`lse.S` 再以 `"assembly.h"` 查找，包含路径给出 libunwind 的同名头文件，它把 `GNU_PROPERTY` 重定义为数字；
   有分支保护时 compiler-rt 写出的属性 note 无法汇编。各文件在包含 `lse.S` 前声明 libunwind 头文件的守卫；目标文件带有映像
   加载器强制 BTI 前所读的 `aarch64 feature: BTI, PAC`。
2. **std 模块的编译选项（mcpp 2026.11.2.3）**：平台的 `[platform.build]` 选项原先到达每个翻译单元而不到达 std 模块；clang 把
   `-fcf-protection` 记入 BMI，x86_64 上每个 `import std` 的单元都被拒绝。平台的选项现在进入 std 模块的两条命令与其 BMI 的缓存
   键，无论 std 模块由工具链还是图中的包提供（e2e 1010）。openkal-platform 0.23.0 因此声明 `mcpp >= 2026.11.2.3`，其 CI 在 mcpp
   发布前以发布分支验证（`MCPP_SOURCE_REF`）。

### 18.7 交付（实际版本）

| 仓库 | 版本 |
| --- | --- |
| openkal | 0.23.0 |
| openkal-linux / -macos / -windows / -emscripten | 0.24.0 / 0.21.3 / 0.19.0 / 0.12.0 |
| openkal-image（ld、run、tools、bind） | 0.4.2（0.4.0 之后两次以新的 openkal-macos 重建） |
| openkal-musl / openkal-llvm-runtime / openkal-platform | 0.27.1 / 0.18.2 / 0.23.1 |
| mcpplibs.os | 0.2.8 |
| mcppls-clangd / mcppls | 0.0.20 / 0.0.20 |
| mcpp | 2026.11.2.3 |
