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
| mcpp | 2026.11.3.1 | #884 |

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
   `OKRUN_SNAPSHOT=make` 再启动一个进程后立即结束，原进程只等待中间进程。生成进程独立完成，可能在原程序退出后仍运行；
   统计整次启动的资源消耗须等待并计入这部分工作。标记文件记录结果
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
7. **早期热启动样本**：clangd `--version` 在快照已存在时启动，记录到 311 次缺页；不用快照时为 442 次，处理器时间少 15%。
   范围为已有快照的单次启动；首次生成的后台资源和最终交付版本组另行统计。

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
2. **std 模块的编译选项（mcpp 2026.11.3.1）**：平台的 `[platform.build]` 选项原先到达每个翻译单元而不到达 std 模块；clang 把
   `-fcf-protection` 记入 BMI，x86_64 上每个 `import std` 的单元都被拒绝。平台的选项现在进入 std 模块的两条命令与其 BMI 的缓存
   键，无论 std 模块由工具链还是图中的包提供（e2e 1010）。openkal-platform 0.23.0 因此声明 `mcpp >= 2026.11.2.3`，其 CI 在 mcpp
   发布前以发布分支验证（`MCPP_SOURCE_REF`）。

### 18.7 交付（实际版本）

表中列出本轮各源码包的版本。公开依赖组的验证记录见 §19、§20；编译器与编辑器的
完整交付还需其各自的发布验收。

| 仓库 | 版本 |
| --- | --- |
| openkal | 0.23.0 |
| openkal-linux / -macos / -windows / -emscripten | 0.24.1 / 0.21.3 / 0.19.0 / 0.12.0 |
| openkal-image（ld、run、tools、bind） | 0.4.3（Linux 只读隔离修正，见 §20） |
| openkal-musl / openkal-llvm-runtime / openkal-platform | 0.27.3 / 0.18.4 / 0.23.3 |
| mcpplibs.os | 0.2.9 |
| mcppls-clangd / mcppls | 0.0.20 / 0.0.20 |
| mcpp-ide | 0.2.0 |
| mcpp | 2026.11.3.1 |

## 19. 发布版本组的更正（2026-11-03）

os 的公开索引映像验收发现了原定版本组中尚未一致的一条边：openkal-musl 0.27.1 的
`[target.'cfg(os = "openkal")'.dependencies]` 仍固定 `openkal-ld = "0.4.1"`，而
openkal-platform 0.23.1 的 `[platform.versions]` 固定 openkal-ld 0.4.2。mcpp 在编译之前
按版本组拒绝这个组合。证据为 [os CI 41578080022 的映像作业](https://github.com/mcpplibs/os/actions/runs/41578080022/job/125763510224)，
报错同时列出 0.4.1、0.4.2 和请求者 openkal-musl 0.27.1。

hosted 构建未经过这条条件依赖。该组合的 hosted macOS `process_test` 已通过，日志没有
“不能授予目录”的跳过分支；本地 hosted 的七个测试程序也全部通过。这些证据只覆盖
hosted 行为。运行时包的分支图验收以工作树代入依赖，也没有证明公开索引中整组版本的
固定关系一致。

既有发布的标签、归档和摘要保持不变。修正使用以下版本：

| 包 | 修正版本 | 修正内容 |
| --- | --- | --- |
| openkal-musl | 0.27.2 | 映像依赖固定 openkal-ld 0.4.2；已发布（[PR #78](https://github.com/mcpplibs/openkal-musl/pull/78)） |
| openkal-llvm-runtime | 0.18.3 | 固定 openkal-musl 0.27.2 |
| openkal-platform | 0.23.2 | 固定上述两个修正版本，其余实现与 openkal-image 版本不变 |

os 0.2.8 固定平台 0.23.2，其完整 CI 的 20 项检查均已通过，包括四个宿主运行相同映像、
hosted macOS 的只读目录授权正向用例。该版本已发布、镜像并进入公开索引（[PR #692](https://github.com/mcpplibs/mcpp-index/pull/692)）。
mcpp 2026.11.3.1 和三个消费者随之固定平台 0.23.2；平台验收和 mcpp 的完整 CI 仍为
后续交付门禁。

局部回归已验证：在 openkal-musl 0.27.2 候选的 `examples/image-early-constructor` 中执行
`mcpp run --target x86_64-openkal --platform openkal-platform@0.23.1`，解析到索引中的
openkal-ld 0.4.2，程序输出 `early 42 path` 和 `the C library was up before the constructor`。
该结果覆盖修正后的依赖边和初始化顺序；修正版的完整发布验收尚未完成。

musl 候选的原生 Linux CI 还暴露出无链接目标包的默认构建回归。缓存填充单元引入显式
Ninja `default` 后，Ninja 不再自动选择其余终端边；本包的 C 编译规则虽已生成，默认
目标却只有依赖和嵌入包的 69 个对象。本地修正为：没有链接单元时，将全部编译单元的
对象列入默认目标；存在链接单元时仍只额外列入缓存填充目标。相同工作树随后生成了
本包的 1,332 个 C 对象，总数 1,401，原 CI 的符号检查通过：可选接口保持弱引用，
`kal_time_sleep` 保持未定义强引用。该检查未修改，也未将依赖包的对象计作本包对象。


## 20. Linux 只读隔离的修正（2026-11-03）

全局审查发现，Landlock ABI 1、2 不能约束文件截断；只处理 `WRITE_FILE` 不能维持只读
授予的承诺。`TRUNCATE` 从 ABI 3 起才可处理，见
[内核 Landlock 文档](https://docs.kernel.org/userspace-api/landlock.html#truncating-files)。
隔离的可用性必须以这一能力为前提，不能仅以 Landlock 是否存在作答。

受控探针在实际 ABI 4 的内核上分别安装旧的 ABI 1 权限掩码和包含 `TRUNCATE` 的 ABI 3
掩码，操作探针自己创建的 10 字节文件。前者允许 `truncate`，文件变为 0 字节；后者以
`EACCES` 拒绝，仍为 10 字节。这是旧权限掩码的复现，不是旧内核启动测试，也不是当前
ABI 4 完整策略被绕过的证据。

openkal-linux 0.24.1 统一以 ABI 3 作为属性声明、子进程隔离和本进程隔离的门槛；较早
ABI 对隔离请求返回不支持。ABI 查询缓存改为原子读写，并发首次查询可重复取得稳定的
内核能力而不竞争普通静态变量。现有行为测试加入原始 `truncate` 与 `O_RDONLY | O_TRUNC`
的拒绝断言，并验证原文件长度不变。[PR #49](https://github.com/mcpplibs/openkal-linux/pull/49)
的 GCC 16 与 LLVM 22 完整验收均通过。ThreadSanitizer 探针未能在本机启动，因此这里
不把静态审查发现的竞争写作经 ThreadSanitizer 证实的报告。

原生运行器静态链接系统实现，单独前移 musl 不能修正已经发布的 okrun。因此继续以新版本
传播修正，既有标签、归档和摘要保持不变：

| 包 | 修正版本 | 依赖关系 |
| --- | --- | --- |
| openkal-linux | 0.24.1 | 只读隔离要求 ABI 3；ABI 缓存使用原子访问 |
| openkal-image（ld、run、tools、bind） | 0.4.3 | 原生 Linux 运行器使用 Linux 0.24.1 |
| openkal-musl | 0.27.3 | Linux 0.24.1；映像加载器 0.4.3 |
| openkal-llvm-runtime | 0.18.4 | 两种分配器安排均使用 musl 0.27.3 |
| openkal-platform | 0.23.3 | hosted 与映像层、工具和版本表固定同一组 |
| mcpplibs.os | 0.2.9 | Linux 0.24.1；平台验收使用 0.23.3 |

Linux、image 源码、musl 和 C++ 运行时的生产者 CI 已通过；image 在四个宿主上执行同一
映像，musl 的原有符号门禁检查 1,332 个本包对象并通过，运行时的十项检查均通过。
平台 0.23.3 已发布、镜像并进入公开索引；GitHub 与 GitCode 的索引归档摘要一致。

os 0.2.9 的[完整二十项检查](https://github.com/mcpplibs/os/actions/runs/41634299142)
全部通过，覆盖原生、hosted、Emscripten 和四个宿主运行相同映像。其依赖由公开索引
解析，不以生产者工作树代入。hosted macOS 的只读授予与可写授予两条正向断言均实际执行
并通过，七个测试程序全部通过，没有使用“不支持授予”的跳过分支。os 0.2.9 已发布、
镜像并进入公开索引（[PR #698](https://github.com/mcpplibs/mcpp-index/pull/698)）；GitHub
与 GitCode 的索引归档均通过摘要核对。编译器的最终公开组打包回归在此之后执行。

工具 0.4.3 的[四个平台二进制发布检查](https://github.com/mcpplibs/openkal-image/actions/runs/41634324479)
全部通过；十二个程序归档与一个调试脚本归档已下载并计算摘要，十二个程序包的版本与
可执行入口已核对。三个工具仓库中的十五份 GitCode 副本均与 GitHub 下载字节一致。
普通安装索引的前移仍需其独立 CI 与发布。

平台的[第一轮四宿主 CI](https://github.com/mcpplibs/openkal-platform/actions/runs/41632882783)
全部通过；其中的候选 mcpp 仍内置平台 0.23.2。mcpp 的默认平台前移到 0.23.3、Windows
launcher 固定 os 0.2.9 后，平台 CI 还须完整重跑。mcpp 的本地新二进制报告 2026.11.3.1，
两个内置目标固定平台 0.23.3；全新工程不写平台覆盖即可构建并运行 `import std` 映像，
根包与 toolchain-model 共 164 个测试程序通过、0 失败。公开版本组上的 E2E 942、951
完整通过，覆盖两种架构的映像、std、拒绝路径、self-contained、single-file 与工作区。编译器完整 CI、发布、编辑器交付及最终资源实测尚未全部完成；这里不以生产者
或本地结果替代这些门禁。

## 21. 已发布编译器的验收记录（2026-11-03）

本节记录 §20 之后完成的编译器交付及其验证范围。引擎、语言服务端和 IDE 的发布、
最终资源实测仍按各自的门禁推进；本节不将它们计作完成。

### 21.1 发布产物与安装索引

[mcpp PR #884](https://github.com/mcpp-community/mcpp/pull/884) 的最终检查为 78 项通过、
2 项跳过；合入提交为 `6028cced5c961e908a12d275b806c34db9d6ff48`。
[2026.11.3.1 发布流程](https://github.com/mcpp-community/mcpp/actions/runs/41650681477)
的九项检查全部通过。独立下载核对了 21 个发布文件、版本别名的字节一致性、封存清单
及 16 份资源镜像。两个 Linux 载荷中的 mcpp 与 xlings 均为静态链接且已剥离符号。

| 平台 | 压缩包字节数 | SHA-256 |
| --- | ---: | --- |
| Linux x86_64 | 16,521,040 | `898d8447532a0d181e584d69fc286d225210a4ecaf5b4af354cb62773a8ab7ca` |
| Linux aarch64 | 13,772,546 | `d7b4767529d90fa0ac5fe252db9027e8a07413bba0a4c6b0314393ce9d09bc8e` |
| macOS arm64 | 9,804,182 | `75c7ad910a080c9ee75b56b29408879e8e1031745dc67050036d1dadd70b61d2` |
| Windows x86_64 | 10,839,480 | `e38678187fb42e9572104d384163625f7953f104f1d972c54a915b3652b1e81e` |

Linux 两个包比前一版本分别增加 11,665 字节、11,911 字节，约为 0.07%、0.09%。
首次发布作业在镜像上传期间被取消；补齐缺失的镜像后只重跑发布作业。重跑前后 21 个
GitHub 资产的标识、大小、摘要和更新时间全部相同，未重写已发布归档。

普通安装索引的 [PR #1105](https://github.com/openxlings/xim-pkgindex/pull/1105) 已合入为
`7cd7002eba5fb12242310ed48640a0f0d399cc98`。三个系统条目的 latest 均指向
2026.11.3.1，旧版本条目保持不变。索引主分支 CI 的 macOS 作业未取得运行机，
因此该轮 CI 不计作通过；维护者明确要求直接发布，不等待该排队作业。发布使用合入
提交上的同一组脚本，未修改 CI 的常规验证策略。

GitHub 与 GitCode 的公开指针均指向 `xim-index-7cd7002.tar.gz`，大小为 1,756,327 字节，
SHA-256 为 `5f7912c3fc84dd75bcdb82b108cd2a88d440e6918733f8df1fb1b6c5df33bed7`。
两个站点的归档、其中的 mcpp 配方和 GitHub 兼容指针均经匿名下载核对。

### 21.2 已发布二进制的实际执行

在中立工作目录、独立的 xlings/mcpp 与 XDG 状态目录中，用 xlings 的实际可执行文件
刷新公开索引，再分别安装精确版本和 latest。两次均解析到 mcpp 2026.11.3.1。
实际执行的 Linux x86_64 ELF 摘要为
`e1fb3ece147c7511cc447c4b4e2499cc6f51a15c276cf771772f27e4539c1799`，
与发布包内的文件相同；未通过用户已有的 shim 执行。

一个没有平台覆盖、没有已有锁文件的新工程，以全新安装的 `llvm@23.1.3` 构建
`import std` 的 openkal 程序，实际输出 `released default image: 42`。
目标锁文件选择平台 0.23.3、musl 0.27.3、C++ 运行时 0.18.4、加载器及工具 0.4.3。
该锁文件证明映像目标的依赖闭包；原生运行器中的 Linux 实现由工具发布与源码组的
独立验证证明，不能从映像目标锁文件推断。

同一发布 ELF 的两项工具链边界检查实际执行如下：

- `[toolchain] default = "system"` 以状态码 2 拒绝，报出仅支持受管理工具链。
- 默认工具链显式指向已有 LLVM 23.1.3 目录，另写 `bootstrap = "llvm@23.1.3"`，
  能构建并运行默认 openkal `import std` 程序，输出 `released custom bootstrap image: 42`。

不显式指定 bootstrap 的外部路径配置，在本次涉及平台 `build.mcpp` 的构建中被主机
工具链解析拒绝；上面的显式 bootstrap 路径已验证可用。

另一个全新隔离环境显式选择 CN，配置查询实际报告 `mirror CN`，索引更新实际取得
`xim-index-7cd7002.tar.gz`。精确版本和 latest 两次安装均成功，实际 ELF 的版本与摘要
和 GLOBAL 安装相同。此项证明 CN 配置下的真实安装；产品保留镜像后备，普通下载日志
未记录最终资源主机，因此这里不声明每次网络请求均只经过 GitCode。

### 21.3 发布后的生产者验证

四个仓库的临时 `MCPP_SOURCE_REF` 变量已删除，删除前确认它们均为本轮候选分支。
使用已发布 2026.11.3.1 的主分支检查中，
[platform 的四项检查](https://github.com/mcpplibs/openkal-platform/actions/runs/41675003365)、
[os 的二十项检查](https://github.com/mcpplibs/os/actions/runs/41675019377) 和
[运行时的十项检查](https://github.com/mcpplibs/openkal-llvm-runtime/actions/runs/41675052794)
均已通过；对应构建日志实际报告 mcpp 2026.11.3.1。
os 的 hosted macOS 作业实际执行七个测试程序，包括只读拒绝和可写创建断言。

image 的主分支检查 41675067975 虽然五项全部通过，日志却显示安装了 xlings 安装包
自带旧索引中的 mcpp 2026.10.24.1，因此不计作 2026.11.3.1 的验证。
[PR #26](https://github.com/mcpplibs/openkal-image/pull/26) 在安装前刷新索引，精确固定
2026.11.3.1，并在映像构建和四个原生运行器构建入口校验实际版本。修正后的五项检查
全部通过，已合入为 `df212eaf3f56cfd84a4aeb19bb43bb9fcba34c3b`，其
[主分支的五项检查](https://github.com/mcpplibs/openkal-image/actions/runs/41681148927) 也已通过。
musl 的 macOS 映像作业因未取得运行机而单独重跑，随后
[主分支的十项检查](https://github.com/mcpplibs/openkal-musl/actions/runs/41675036253) 全部通过。
五个生产者主分支的 49 项检查至此全部通过；索引 CI 固定版本、自举固定版本和编译器
最终主分支的完整检查分别验收。

### 21.4 索引的验证版本固定

[mcpp-index PR #699](https://github.com/mcpplibs/mcpp-index/pull/699) 的十七项最终检查
全部通过或按工作流条件跳过，包括 Linux 与 Wine 中 Windows 的兼容性测量，已合入为
`0b65b1f18269467e102058925e77930403d55448`。合入提交与审查头的树相同。
三个工作流与测量 pin 统一使用 mcpp 2026.11.3.1，hosted 平台测试成员选择 0.23.3。
客户端版本下界 2026.9.18.3、兼容性运行时基线 0.15.2 和工具链基线 LLVM 22.1.8
保持不变。

## 22. 候选测量有效性与 Windows 分段映射修正（2026-11-03）

### 22.1 布局作业的成功状态与测量结果分开验收

引擎候选 `a5a075b` 的两种架构布局作业均报告成功，但顺序与触页两次测量的
会话 JSON 均为 `ok: false`，在二十分钟超时后仅建成一个模块。这些作业结论不作为
有效测量，也不支持把 openkal 台账项 0002、0003 改为 steady。

原始映像不在安装包的 `bin/../lib/clang` 布局中。相同 x86_64 映像、相同工作区在
系统 clang 18 的工具路径下，未指定资源目录时报告 `stddef.h` 缺失；显式指定本次
构建的 LLVM 23 资源目录后，304 个翻译单元的冷会话实际建成 176 个模块并就绪。
这项对照是资源目录问题的诊断，不是已发布产物的性能测量。

[引擎修正 `994f1be`](https://github.com/Sunrisepeak/mcppls-clangd/commit/994f1be9c56e685b2fd0d7230799df917d349d8a)
为两次原始映像测量显式传递资源目录，要求会话集合非空且每个会话的 `ok` 为真，
并在失败时保留顺序、触页和会话日志。
[ARM 候选的诊断归档](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41689675773/artifacts/13256979913)
实际记录两次就绪，均建成 176 个模块。完整四宿主验收与最终发布测量仍分别执行。

### 22.2 Windows 启动异常的原生取证

旧候选的 Windows 映像验收在 LSP 初始化回复前发生访问异常。
[原生复现](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41691553925)
覆盖原始 `.oke` 和 `.okb`，各自使用默认、关闭快照、关闭放置三种设置；六次均以
`0xC0000005` 退出。该异常不依赖快照或放置，不能解释为缺失名字记录的语义错误。

进程早于调试器附加而结束。随后由 CDB 从创建时启动同一映像，
[六次均捕获首个访问异常](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41692290011)，
保存匹配的运行器、寄存器、指令和六份有效 minidump。指令在把 ELF 头写入已具有该
内容的目标页时被拒绝。调试器报告的实际指令位置是运行器的 `+0x51834`；产品故障
报告所示 `+0x50c34` 不能直接用作 PE 的 RVA。此处未声明具有匹配 PDB 的源码栈。

### 22.3 部分映射成功后的复制范围

加载器按触页信息把一个只读段分成多个映射请求。早先请求成功后，后续请求可能被
环境拒绝，例如 Windows 不接受最后不足 64 KiB 的文件视图。旧实现随即复制整个段，
写入先前已经映为只读的页。

[openkal-image PR #27](https://github.com/mcpplibs/openkal-image/pull/27) 只复制被拒绝的
那一段请求；其中包含指令时，单独发布该复制范围。发布被拒绝则释放本次区域并拒绝
加载，后续不再进入整段复制。成功的文件映射保留。

同一源码提取的受保护映射对照中，旧路径因写入只读页收到 `SIGSEGV`；修正后字节
相同，恰有两次映射请求和一次复制范围的发布。新增真实映像回归验证首个只读段的
长度和未对齐尾段，再由四个原生宿主分别运行其 `.oke` 和通用 `.okb`。
[五项生产者 CI](https://github.com/mcpplibs/openkal-image/actions/runs/41693546622) 已全部通过。
原始 clangd 映像还须由修正后的 Windows 运行器独立复验；新不可变版本、消费者闭包、
缺失名字与 warm-prime 的完整验收尚未由本节宣告完成。已发布标签与归档未被改写。

## 23. IDE 发布渠道与真实索引消费（2026-11-03）

### 23.1 不可变发布文件与编辑器分发

[mcpp-ide 0.2.0](https://github.com/mcpp-community/mcpp-ide/releases/tag/v0.2.0)
来自合入提交 `421613a98e1868fc295262a67f322e49c6f90a99`。发布检查的 23 项构建与验收
全部通过。五个 GitHub 资源与五个 GitCode 镜像分别下载后摘要相同；发布清单内的
四个文件均与 `SHA256SUMS` 一致。通用后端归档为 5,083,938 字节，SHA-256 为
`3d692b433f85588327b7aa06e158e97db61490498870af84ace731a633c1ebe0`。

原发布作业在创建 GitHub release 后因缺少 Open VSX 凭据失败。配置凭据后的
[补发作业](https://github.com/mcpp-community/mcpp-ide/actions/runs/41696583381)
直接使用已有 VSIX，验证摘要后上传，没有重新构建或覆盖 release。
[Open VSX 公开版本 1.1.0](https://open-vsx.org/extension/mcpp-community/mcpp-vscode)
已可下载；实取文件为 6,171,177 字节，SHA-256 为
`7383a9f21565685a0a15cd2ea49dac5dbfa2ed479e5db412986b7c81bb880034`，
与 GitHub VSIX 相同。插件版本 1.1.0 与项目发布版本 0.2.0 分别核验。

[JetBrains 上传作业](https://github.com/mcpp-community/mcpp-ide/actions/runs/41698011098)
提交同一 release 的 `mcpp-clion-0.2.0.zip`，输入文件为 5,574,062 字节，SHA-256 为
`6daccbaa5b52eb30c784da4ebc14fdaa333d6a769e00e4707b9ac74f8b6a3c8b`。
平台返回 HTTP 201，更新 ID 为 1211284，回执的 `approve` 和 `listed` 均为 false。
此项结论是已受理、等待审核；公开上架另行验收。Marketplace 回执中的资源大小
为 5,579,978 字节，不能据此声明平台处理后的下载文件与上传 ZIP 字节相同。

### 23.2 索引启动器适配与双配置安装

新通用包的 Windows 入口是 `bin/mcpp-ide.exe`，旧包使用 `.cmd`。
[xim-pkgindex PR #1106](https://github.com/openxlings/xim-pkgindex/pull/1106)
增加 0.2.0 的三个系统版本条目，并使安装配置优先选择实际存在的 `.exe`，保留旧包的
`.cmd` 回退。旧版本条目保持原文。回归检查覆盖五个场景；CI 的 Linux、macOS 和
Windows 安装作业全部通过，Windows 作业实际执行新入口。

PR 合入为 `114761a4332cb92eff20022c94a5f8d0540c66d7`。按本轮直接发布授权，使用
同一提交的索引构建、发布和指针脚本生成并发布 `xim-index-114761a.tar.gz`。
GitHub 和 GitCode 两个实际公开归档均为 1,756,998 字节，SHA-256 为
`da5c97e8fb1115d1ceb6592fc1c3eb4f84aed919fc7d6e48aefb5b64d5538b43`。
两份归档内的 IDE 描述符逐字等于合入版本，mcpp latest 三个条目仍指向 2026.11.3.1。
GitHub 原始指针和兼容 release 指针已验证；GitCode 原始文件 HTTP 403，改由公开 Git
读取指针，读取方法与对应提交单独记录，不记作原始 URL 下载成功。

GLOBAL 与 CN 两个既有隔离环境分别刷新真实公开索引，精确安装 `mcpp-ide@0.2.0`，
再安装 latest。两边实际安装的 11 个文件逐个等于已验证公开通用归档内的对应文件，
运行安装后的入口均输出 `mcpp-ide 0.2.0 (protocol 2)`。CN 测试允许产品原有镜像
后备，因此证明 CN 配置下的安装，不声明每个下载请求只经过 GitCode。

## 24. 已发布 Windows 运行器的独立复验（2026-11-03）

openkal-image 0.4.4 的不可变标签指向
`fda22ddd9af3c03ebd3996a385bdb0382bbf62c7`；发布版本的五项源码 CI 全部通过。
源码归档在 GitHub 与 GitCode 的摘要相同，mcpp-index PR #700 已合入为 `7e2b542`，
其发布作业成功，两个公开索引归档均验证了四个组件各三个系统条目。
读取时 GitHub 原始文件 CDN 仍返回旧指针，公开仓库 contents API 已返回新指针；
记录保留这一区别，没有把旧 CDN 响应记作新版本可见。

[Windows 原生复验](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41699690007)
直接下载公开的 `okrun-0.4.4-windows-x86_64.zip`，其 SHA-256 为
`1adc1bfcef6a90f9edabf4b265ae547ff8663a522758a949c175d4079aa2c5ca`。
归档中实际 `okrun.exe` 的 SHA-256 为
`22455616bf0907dbd6a17169e4623c941c6e7bcf283f2b945d3dcb17f802076e`，
与取证归档保留的运行器逐字相同。

原始 `.oke` 和 `.okb` 各在默认、关闭快照、关闭放置三种设置下运行。六组版本查询
均报告 `clangd version 23.1.0-mcppls.9`，六组语义会话均完成初始化、查询得到名为
`probe` 的文档符号和正常关闭；没有首个访问异常或调试器强制结束。工作流和独立读取
原始回复的检查均通过。取证 ZIP 为 4,428,615 字节，SHA-256 为
`22320181d885f2eeda26f799a5d8f35cac24308782795cba2ec9aad817243cf0`。

这里仍使用同一旧引擎映像隔离验证已发布加载器的修正。新的 musl、C++ 运行时与平台
依赖闭包、引擎的四宿主缺失名字及 warm-prime 验收、最终发布性能测量分别验收，
不由这次启动与语义复验代替。

## 25. JetBrains 公开版本的验收（2026-11-03）

第 23.1 节提交时尚在审核的 mcpp-ide 0.2.0 已通过审核。
[更新 1211284](https://plugins.jetbrains.com/plugin/35632-mcpp/versions/stable/1211284)
的公开 API 实际报告 `approve: true`、`listed: true`、`hidden: false`。
从 Marketplace 公开地址下载的 ZIP 为 5,579,978 字节，SHA-256 为
`7e3fec81186d897e4185413ffbf3b69b08f9185f0da78842f4826f2408d274ba`；
包内插件 XML 的 ID 为 `io.github.mcpp-community.mcpp-clion`，版本为 0.2.0。
该公开归档与原上传文件分别记录摘要，没有把平台处理后的文件记作原文件字节相同，
也没有声明独立完成 Marketplace 签名验证。

## 26. 0.4.4 工具分发与隔离安装（2026-11-03）

[0.4.4 二进制发布](https://github.com/mcpplibs/openkal-image/actions/runs/41698431841)
的四个宿主全部通过。macOS 首次请求在十五分钟内未分配到运行机，没有执行步骤；
只重跑该未成功作业后通过，其他三宿主的成功结果保留。13 个 GitHub 归档已下载并
校验摘要，12 个程序包内的清单身份与所列文件摘要均通过，15 份 GitCode 镜像分别
下载后与原归档一致，包括三个工具仓库各自的调试脚本副本。

[xim-pkgindex PR #1107](https://github.com/openxlings/xim-pkgindex/pull/1107)
的完整 CI 和 31 项检查全部通过或按条件跳过，包括三个系统的实际安装检查，已合入为
`43781b8389f09662df4d2f81f1f70351798d2460`。三个描述符各增加新版本和四个归档摘要，
更新各自三个系统的 latest 引用；撤销这些新增与 latest 更新后，描述符逐字等于旧版。
发布使用同一合入树的原有构建、上传和指针脚本。两个公开索引归档均为 1,757,443
字节，SHA-256 为 `6e27ff754e6fc2b99db95624feb30519a98d896038e20762632c3468d6278084`。
三份工具描述符逐字等于合入版本，mcpp 2026.11.3.1 和 IDE 0.2.0 的 latest 引用保留。

GLOBAL 与 CN 的既有隔离环境均成功安装三种工具。每个环境中九个安装文件逐个等于
已验证公开 ZIP 内的对应文件；实际 ELF 分别通过 `okld check`、`okrun --prof` 与
`okprof report`。这些是工具安装与功能检查，不作为引擎最终验收或性能数字。

GLOBAL 环境此前显式选择 0.4.3。不带版本的安装保留该选择；`@latest` 实际解析为
0.4.4，执行 `xlings use <tool>@0.4.4` 后，不带版本的安装仍选择 0.4.4。
CN 环境此前未安装这三种工具；本轮精确安装后，不带版本的安装均选择 0.4.4。
这里没有声明裸安装会自动替换已有的显式选择。Linux 0.4.4 的 okprof ELF 与先前实测的 0.4.3 字节相同；
独立草稿 PR #25 的 `record` 修正未包含在本次发布，`record` 不计作通过。

## 27. 最终主分支验证（2026-11-03）

[mcpp 最终主分支运行 41686943730](https://github.com/mcpp-community/mcpp/actions/runs/41686943730)
在提交 `e0aee5cb7dfa5b13d1a5248d447947534d3cfa8b` 上成功结束，64 项成功，2 项按条件
跳过。跳过项为 `openkal` 汇总入口和 `macos-ios / iOS - what this runner actually
provides`。本轮只重跑了未成功的基础设施作业；重跑后的 ARM musl 交叉构建、Android
模拟器和两个架构的 iOS 模拟器作业均通过。

该提交的直接父提交为 `a86d09bc8bff016f14f05c6b9ff8003256d6e094`，包含用户 PR #878
对 glob 的 `**/` 语义修正，祖先关系已验证。自举 PR #886 仅更新 `.xlings.json`，
没有撤回该用户改动。结合第 21.3 节五个生产者的 49 项验证和索引固定版本验证，
T6 已完成；新加载器依赖组与引擎、服务端发布继续分别验收。

## 28. musl 0.27.4 的发布（2026-11-03）

[openkal-musl PR #80](https://github.com/mcpplibs/openkal-musl/pull/80) 的
[十项生产者检查](https://github.com/mcpplibs/openkal-musl/actions/runs/41700027602)
全部通过。原生 macOS 和交叉构建程序的 macOS 运行请求先后因十五分钟内没有取得
运行机而取消，没有执行测试步骤；每次均待运行结束后只重跑未成功项。最终包括
两个 Linux 原生工具链、原生 macOS、交叉链接及其 macOS 执行、一次构建的映像与
四个宿主对这些相同映像的执行，均有实际成功结果。

合入提交为 `657fa1609b9aba10527501e47e98c0bd5ddc347e`，树为
`9e2f7afe49178f555e5ee372b8615e569b15acba`，与已审查并通过 CI 的 `fc64463` 相同。
不可变标签 0.27.4 指向该合入提交，GitHub release 已创建；家族分支通过旧引用的
精确租约由 `91aa89e` 更新到合入提交。同树的主分支重复构建 41706351111 已取消，
本节的十项成功结果来自前述 PR 运行。

[GitHub 源码归档](https://github.com/mcpplibs/openkal-musl/archive/refs/tags/0.27.4.tar.gz)
与 GitCode 镜像均为 1,266,813 字节，SHA-256 为
`6620ec32d8a3058153d392094edae6345c0c47f60e9ca0f871b9a01f6217eb89`。
归档根 manifest 逐字等于合入源码：包版本 0.27.4，映像的加载器依赖为 0.4.4。
C 库实现和各原生内核实现的固定版本保持不变。

[mcpp-index PR #701](https://github.com/mcpplibs/mcpp-index/pull/701)
新增三个宿主条目并更新 Linux 测试成员，旧版本条目保留。其完整索引检查与公开索引
指针验收另行完成，后续运行时 0.18.5 和平台 0.23.4 在此前不启动消费者闭包验收。

## 29. musl 公开索引与运行时接续（2026-11-03）

第 28 节的索引 PR #701 已完成 22 项检查，全部通过或按条件跳过，包括两个 Linux
候选工具链、镜像摘要检查和 macOS 工作区，合入为
`a9615dbda0aabe62330c93253d0dae7f8ecb9e5e`。
[索引发布运行 41708174098](https://github.com/mcpplibs/mcpp-index/actions/runs/41708174098)
成功。普通 GitHub 原始指针已报告该提交；两个公开索引归档均为 664,441 字节，
SHA-256 为 `feff22f2ad123d7d2b48041ec54357d314b8ec60f6b34e08f5fb2fbe2d36237e`。
归档内的 musl 0.27.4 三个宿主条目及其源码摘要均已检查。

在这些公开字节可用后，才推送运行时 0.18.5 的
[PR #62](https://github.com/mcpplibs/openkal-llvm-runtime/pull/62)，完整生产者 CI
41708741056 已启动。另用已发布 mcpp 2026.11.3.1 的实际 ELF 和 LLVM 23.1.3，运行
该候选源码原有的 `cxx` 与 `import-std` 两个 Linux musl 示例。前者报告零个失败观察，
后者输出 `import std above openkal: 2 4 7`。

两个示例的实际锁文件均只含四项索引依赖：musl 0.27.4、Linux 0.24.1、openkal
0.23.0、mimalloc 3.6.1，目标均为 `x86_64-linux-musl`；运行时自身是待测本地源码。
这些依赖没有被改成工作树路径。此项使用已有工具链状态，验证公开依赖的实际消费；
运行时四宿主发布验收、完整平台闭包和最终性能测量分别完成。

## 30. C++ 运行时 0.18.5 的发布（2026-11-03）

[运行时 PR #62](https://github.com/mcpplibs/openkal-llvm-runtime/pull/62) 的
[完整生产者运行 41708741056](https://github.com/mcpplibs/openkal-llvm-runtime/actions/runs/41708741056)
十项检查全部通过，包括原生与交叉目标行为、原生 macOS 的开发和发布程序、交叉
构建程序的实际执行，以及同一组 C++ 映像在四个宿主上的运行。

合入提交为 `e7a7b8ea019dbbcb68bc96480abaae6835f9694d`，树为
`22db06238d26db2490969ce9491e76f61dc5b591`，与审查头 `3e65aa7` 的树相同。
不可变标签 0.18.5 指向该合入提交，GitHub release 已创建，家族分支通过旧引用的
精确租约更新。同树的主分支重复构建 41712011165 已取消，本节的成功验证来自
前述完整 PR 运行。

[GitHub 源码归档](https://github.com/mcpplibs/openkal-llvm-runtime/archive/refs/tags/0.18.5.tar.gz)
和 GitCode 镜像均为 15,643,016 字节，SHA-256 为
`45414755cc65755ca05fb5f21f8862151cad484eb6f6b6ad52ae7a3fcf6e6eb0`。
归档根 manifest 逐字等于合入源码；两种分配器安排均选择 musl 0.27.4，其映像加载器
为 0.4.4。C++ 运行时实现保持不变。

[mcpp-index PR #702](https://github.com/mcpplibs/mcpp-index/pull/702) 增加三个宿主
条目并更新 openkal 测试成员。撤销新增版本块后，描述符逐字等于先前版本。
使用已发布 mcpp 2026.11.3.1 和该候选索引，在 `x86_64-linux-musl` 上构建并运行
原有 `kal` 测试，通过一项测试、零项失败。实际锁文件记录运行时 0.18.5、musl
0.27.4 和 Linux 0.24.1，均为索引来源。此项使用既有工具链缓存，不作为隔离安装或
最终性能数字；公开索引验收和平台 0.23.4 的完整闭包验证分别完成。

## 31. 运行时索引与普通客户端的传播边界（2026-11-03）

第 30 节的索引 PR #702 完成四个工作流和二十项检查：十五项成功，五项按条件
跳过。合入提交为 `ed1348cc6cd9959e026138ca28d3171e3b984446`。
[索引发布 41713458419](https://github.com/mcpplibs/mcpp-index/actions/runs/41713458419)
成功；GitHub 和 GitCode 两个索引归档均为 664,489 字节，SHA-256 为
`eaf45f962593a12a2e3783e3c761ad54c1a1cc97fb4f7ba82c4414d64949de83`。
归档内三个运行时 0.18.5 条目的源码摘要均已检查。

首次验收通过仓库内容 API 的退路确认了新指针与两个归档，但普通原始文件地址仍
由 CDN 返回旧提交 `a9615db`。据此启动平台 CI 过早：两个 Linux 作业和一次本地
检查均因旧索引没有运行时 0.18.5 而失败，尚未进入平台行为验证。初次失败日志保留。
本轮检查脚本已改为默认要求普通原始指针跟进；API 退路须显式选择，只用于观测
仓库发布，不能作为普通客户端已可安装的证明。

20:39 UTC，普通原始指针已返回 `ed1348c`；严格检查再次下载两个归档并验证摘要和
三个版本条目。随后本地实际刷新索引开始下载运行时 0.18.5。平台 PR #17 的同一
源码继续验证；其初次失败项在传播完成后分别重试，成功结果另行记录。

## 32. 平台 0.23.4 的发布与索引候选（2026-11-03）

[平台 PR #17](https://github.com/mcpplibs/openkal-platform/pull/17) 的
[完整四宿主运行 41713909104](https://github.com/mcpplibs/openkal-platform/actions/runs/41713909104)
全部通过。macOS 和 Windows 的初次运行成功；第 31 节中因旧索引而失败的两个
Linux 作业，在普通指针传播完成后单独重跑，通过平台测试、打包、快照、hosted
程序和以映像运行的构建程序。没有更改源码或放宽检查。

合入提交为 `575eb44261f70de6db7e08af3da49f3eccdd5424`，树为
`5dcbc2351eb299e39129bf495d5d2c8506014664`，与审查头 `395ddae` 相同。
不可变标签 0.23.4 指向该合入提交，GitHub release 已创建，家族分支通过旧引用的
精确租约由 `4de4285` 更新。同树的主分支重复构建 41717608555 已取消，本节的
四宿主成功结果来自前述完整 PR 运行。

[GitHub 源码归档](https://github.com/mcpplibs/openkal-platform/archive/refs/tags/0.23.4.tar.gz)
与 GitCode 镜像均为 18,573 字节，SHA-256 为
`dd1ea3ae41fd0607ccb02f1dbf2fe7aced073107b8442a81f33bfe74c96454d7`。
归档根 manifest 逐字等于合入源码。映像和 hosted 两种安排均选择 musl 0.27.4、
运行时 0.18.5 和映像加载器及工具 0.4.4；内核实现固定版本与编译、链接约定保持不变。
已有 mcpp 2026.11.3.1 和 IDE 0.2.0 发布保留其固定版本；新消费者显式选择平台 0.23.4。

[mcpp-index PR #703](https://github.com/mcpplibs/mcpp-index/pull/703) 增加三个宿主
条目并更新 hosted 测试成员，兼容性检查的运行时与工具链基线保持不变。撤销新增
版本块后，描述符逐字等于先前版本。使用已发布编译器和候选索引运行原有 hosted
测试，通过一项、零项失败；实际锁文件记录平台 0.23.4、运行时 0.18.5 和 musl
0.27.4，均为索引来源。此项使用已有工具链缓存，不作为隔离安装或最终性能数字。
索引完整检查、公开指针和归档验收完成后，才启动引擎在新平台组上的完整验证。

## 33. 公开平台闭包与消费者接续（2026-11-03）

索引 PR #703 最终仅改三个文件：版本描述符、hosted 测试成员固定版本和 CHANGELOG。
初次补充的两行基线注释触发旧运行时 0.15.2 的全部 32 个兼容性成员；该注释随后
撤回，固定版本文件逐字恢复为基线，选择器和工作流保持不变。按原有规则，新增版本
不影响该旧的精确固定图，新版本候选及 hosted 检查继续执行。

合入提交为 `586a7e5293e695096eea9b79c3d03447f0b2e53f`，树为
`3beaeccf8d43ad38c34fe6f464f77d9d339cfa52`，等于审查头 `4c93b5d` 的树。
合入时两项重复 Mac 检查仍在排队，依照用户不等待该队列的发布要求，复核了前一头
`b243b18` 两项实际 Mac 成功日志，均运行平台 0.23.4 的 hosted 测试。两个头仅相差
上述两行注释，TOML 解析值相同，其他文件逐字相同；没有将排队作业记作已经执行。
随后 `4c93b5d` 的四个工作流和二十项检查全部结束，十六项成功、四项按条件跳过。
同树的重复主分支验证 41720309336 已取消，没有将其记作绿色主分支验证。

[索引发布 41720309193](https://github.com/mcpplibs/mcpp-index/actions/runs/41720309193)
成功。两个公开归档均为 664,515 字节，SHA-256 为
`32b89034957232a7e301bfc3b5133185f0fb45782dab052f84228b382b5163f5`；归档内
三个平台 0.23.4 条目与源码摘要已检查。21:23:39 UTC，普通原始指针实际跟进该提交，
随后严格检查再次下载并验证两个公开归档。

在原有 GLOBAL 隔离环境中新建显式选择平台 0.23.4 的项目，使用已发布 mcpp
2026.11.3.1 的实际 ELF 和该环境的托管 LLVM 23.1.3，刷新普通公开索引后构建、运行
`import std` 程序，输出 `released explicit 0.23.4 image: 42`。实际锁文件的
平台 0.23.4、运行时 0.18.5、musl 0.27.4、工具 0.4.4 均为索引来源，没有工作树
依赖替换。此项使用既有隔离工具链状态，验证新平台公开消费；编译器内置默认仍为
0.23.3，不作为最终引擎或服务端性能测量。

21:24:27 UTC 才推送引擎修订 `ca208ce`，启动
[完整引擎运行 41721522543](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41721522543)。
台账检查 41721522428 因 0002 仍为 stabilizing 而拒绝发布，符合当前阶段；在实际
四宿主缺失名字验收及 warm-prime 完成前，不提升 0002、0003 的状态。

服务端以相同公开索引配置所有六个既有目标，二十二项依赖均为索引来源。
原生构建通过，授权目录、模板与进程三组分别通过 8/65、7/110、28/131 个用例/断言，
合计四十三个用例、三百零六条断言，零失败。测试后的全部包字段、目标集合及锁文件
逐字等于此前六目标配置，SHA-256 为
`b0de59c62473e2967271657e644a1bd0639d19b588c51a7c1191aa3e44e66b31`。
固定版本与锁文件整理为本地提交 `66078ca1a1514777567a30bb47fd286ba1de8d5d`，
尚未推送；新引擎候选的完整服务端 CI 仍先于引擎正式发布。


## 34. Windows 工作进程路径与连续租约的修正（2026-11-03，Sunrisepeak）

这是 §33 后续验收的实现记录，不将候选测量作为正式发布结果。
[引擎运行 41721522543](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41721522543)
以十三项成功、一项失败、一项跳过结束。四个原生构建、两个兼容性底线包、两种架构
的映像及 bundle 身份检查均成功。Linux x64、Linux arm64、macOS 的映像验收成功；
Windows 的模块用例返回 3，缺失名字 canary 在首次构建模块时失败。warm-prime
因依赖失败而跳过，不能据此提升补丁状态。

原始日志给出两项具体原因：缓存能力探针报告共享锁没有共享，工作进程报告
`bounded worker outputs require its private unit`。Windows 会在同一文件句柄的独占锁
旁增加共享锁，而非将前者替换；两个锁存在时，第一次相同范围的解锁释放独占锁。
此行为见 [Microsoft LockFileEx 文档](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex)。
原有探针错误地将再次申请共享锁视为转换，实际独占锁仍阻止第二个句柄取得共享锁。
工作进程的输出边界、私有缓存路径和隐式缓存清单还包含硬编码的 `/` 判断，
与 Windows 运行时使用的路径风格不一致。

修正提交 `02714d0ee7c7a25dc4102cb49db025e3206636d3` 增加主系列 0043 和 openkal
系列 0004。0043 按路径组成部分检查私有输出，保留外部目录、相邻前缀目录和 `..`
逃逸路径的拒绝，并以同一风格构造私有缓存路径、识别隐式缓存目录。
0004 在 Windows 上先取得同一打开文件的共享锁，再释放独占锁，发布期间始终持有
租约；能力探针检查转换后的独占排斥与共享准入。Windows 的共享锁也禁止持有者
写入，探针因此在测试写入前释放两个共享锁。真实缓存的租约文件和载荷文件仍分开，
没有通过释放后重取租约来放宽发布协议，也没有修改底层已发布版本。

路径回归用例同时覆盖 POSIX 与 Windows 风格。抽取实际新增函数与用例体，链接
本机 LLVM 18 的路径实现后通过；将缓存路径判断换回旧的拼接写法，同一个 Windows
正向用例触发断言。此项不替代 LLVM 23 的完整构建。十项发布门槛和包身份负向
控制通过；八个相关文件从固定上游文件按全部四十三个主补丁、四个 openkal 补丁
重建后逐字一致。

[Windows 原生锁探针 41730623432](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41730623432)
成功，十二项检查、零失败。实际检查旧的替换假设被拒绝、第一次解锁后共享租约仍
排斥另一个独占持有者、两个共享持有者共存，以及共享锁持有期间写入被拒绝、释放后
写入成功。探针使用系统文件锁接口，验证环境语义；完整引擎的实际 openkal 路径仍须
由后续四宿主验收与 warm-prime 验证。

[完整修正运行 41730464895](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41730464895)
已启动。独立的补丁检查 41730464752 已将主系列及 openkal 系列完整应用于固定
LLVM 源码，随后因 0043 为 stabilizing 而在发布台账检查处停止，符合当前阶段。
0043、0002、0003、0004 保持 stabilizing。此处没有将该运行记作完整绿色验证。

修正前的 `ca208ce` 候选还完成了一次匹配版本的 balanced 预检：原生 `.9`、映像
`.9` 与已发布运行器 0.4.4，原有 L2 探针每项五次、含标准库用例，验收结果为通过。
这是修正前候选的预检，不是修正后提交或正式发布的最终数字。执行的双架构 bundle
为 150,706,147 字节，内存额度使用经其 manifest 摘要验证的 x86_64 原始映像
73,307,928 字节，未使用两个架构合计大小。最终实测脚本已要求显式提供该原始映像，
记录执行 bundle 与选中映像各自的摘要和大小；测量场景及验收阈值未改动。


## 35. Windows 账本排斥与定点读写的修正（2026-11-03，Sunrisepeak）

§34 的完整运行 41730464895 最终以十三项成功、一项失败、一项跳过结束。四个
原生构建、两个兼容性底线包、两个映像构建及 bundle 检查均成功，三个其他宿主的
映像验收成功；Windows 映像的模块用例五次返回 3，缺失名字 canary 的首次模块
构建报告 `Resource temporarily unavailable`。warm-prime 因依赖失败而跳过。
此结果保留为失败，不能提升台账状态。

Windows 后端 0.19.0 的定点读写使用第二个文件句柄，以保留原句柄的流位置。
账本原有整文件独占锁是强制排斥，会拒绝第二句柄的数据访问。
[原生 Windows 验证 41738774174](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41738774174)
通过十六项检查，复现整文件锁下读写被拒绝，并验证数据范围之外的单字节锁仍排斥
另一个账本写入者、允许定点读写、不增加文件长度，且新旧两种锁范围相互冲突。
这验证系统接口的行为，不替代完整映像的实际运行。

提交 `82e33ba097eff3d14fd513778a9ed42c52725d88` 增加 openkal 补丁 0005。
Windows 账本锁使用偏移 2,147,483,647 的单字节范围，编译期断言全部有限账本数据
均在其前；POSIX 仍使用既有 flock 协议，载荷租约仍使用既有范围。能力探针在持锁
期间实际执行定点写入及读回。三个修改文件从固定基线经四十三个主补丁、五个
openkal 补丁重建后逐字一致。实际平台适配文件在本机 LLVM 18 下编译和运行通过，
仅将 scope-exit 工具的名字映射为该旧版本的拼写；十项发布门禁与包身份检查通过。
底层已发布后端及平台版本没有变动。

[完整验证 41739179747](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41739179747)
已启动，LLVM 23 的四宿主映像验收和 warm-prime 尚未完成。0043 及 0002 至 0005
保持 stabilizing，服务端提交 66078ca 仍在本地等待合格候选。补充澄清：原有
PrerequisiteModulesTest.cpp 整体受非 Windows 条件保护，新增路径用例在 Linux 与
macOS 实际通过，不能表述为该用例在原生 Windows 执行通过。

## 36. 嵌套构建的进度粒度与缓存诊断（2026-11-03，Sunrisepeak）

用户补充的日志在总用时 6:04 至 30:06 之间保持 `1630/1636`，当前动作为
`clangd: llvm:clangd-libraries`，动作耗时从 1:11 增至 25:13。mcpp 2026.11.3.1
的该计数表示外层 Ninja 已完成和总动作数。这个 prepare 动作调用 okgen，再由
CMake/Ninja 构建 LLVM 与 clangd 静态库；内部编译步骤不增加外层计数，后续链接
等待该动作整体结束。末尾时间是动作已用时间，不是预计剩余时间。非终端输出的
心跳间隔为六十秒；Ninja 在命令完成前缓冲其输出。该映像构建随后实际成功。
因此可确认进度可见性的粒度限制，不能由此日志判定调度死锁。

同一完整运行的 x86 映像日志记录 ccache 4.9.1-1：2,592 次调用中 83 次可缓存，
2,509 次不可缓存。隔离的同版本 ccache 对最小程序及未修改的公开平台编译契约均
取得首次未命中、第二次直接命中；样本不含 LLVM 的完整头文件负载，尚未复现其
不可缓存原因。82e33ba 的必要重建同时保存详细统计原因，未为诊断单独重建 LLVM。
本轮未修改 mcpp 的进度实现。后续显示改进可分别呈现外层动作和嵌套构建活动；
动作数量不能直接换算为剩余墙钟时间。


## 37. 预编译头缓存统计的补充（2026-11-03，Sunrisepeak）

§36 的原因待查状态由本轮 ARM 映像作业的详细统计补充。
运行 41739179747、提交 82e33ba 的 ARM 布局 artifact 13281721622 摘要为
`ccc0aafaa9abd0437a86ea5f344b557112374865947ad202ac10bcbeb2639540`。
该作业最终成功。计数在映像构建前清零，2,592 次调用中 82 次可缓存、均未命中，
2,510 次不可缓存；其中 2,482 次为 `Could not use precompiled header`。
另有十四次编译失败及十四次预处理失败计数，未逐一归因，不能将其解释为整个作业
失败。这组统计来自 ARM，不是当时仍在运行的 x86 映像作业。

[ccache 4.9.1 文档](https://ccache.dev/manual/4.9.1.html#_precompiled_headers)
说明该计数表示 PCH 缓存前提未满足。固定 LLVM 源码的 CMake 配置默认在相应 Clang
版本下启用 PCH，并为 `LLVM_CCACHE_BUILD` 设置配套的缓存参数；映像工作流直接
设置编译器 launcher，没有采用该参数配置。此项是构建缓存配置问题，与外层动作
计数的可见性限制分别记录，不是调度死锁的证据。

本地以相同 ccache 4.9.1、Clang 23.1.3 和公开平台 0.23.4 的 Release 编译契约
复核。最小程序及含 vector、cstdint 的头文件样本均可缓存，第二次为直接命中。
随后生成真实 PCH：相同源码两次编译均成功，但缓存两次报告同一 PCH 拒绝计数；
改为文本包含后，两次均可缓存，第二次直接命中。日志明确给出缺少 PCH 配套参数。
实验未放宽缓存策略，也未修改编译选项或正在运行的 CI；不据小样本推断关闭 PCH
后的完整 LLVM 首次构建或重复构建用时。

## 38. 统一载荷安装适配与临时缓存整理（2026-11-03，Sunrisepeak）

XIM 配方的本地提交 bcc26a6 按载荷版本 5 的平台条目选择运行器和引擎路径，
保留旧布局分支，未添加尚未发布的资源或修改 latest。十项静态及隔离检查通过；
使用实际 xlings 2026.10.26.1 ELF 在既有隔离 GLOBAL 环境完成注册、搜索和移除，
覆盖条目已清除。这些检查只证明注册与静态条件，版本 0.0.20 的公开载荷、安装、
执行、卸载及四宿主 CI 仍分别验收。

整理缓存前核对自有目录、活动进程及永久归档，保留旧诊断日志和原生 Windows
故障转储。随后仅移除六处过期目录：临时 Wine 前缀、旧运行时示例的 target，
以及旧索引验证工作树的工具与构建缓存。观察区间内可用空间增加约 1.759 GiB，
达到约 4.284 GiB。最新运行时及平台索引验证的 target、源文件和全局缓存均保留。
该值是区间内观察值，不声明对整个文件系统活动具有排他控制。


## 39. 联合候选验证的并行推进（2026-11-04，Sunrisepeak）

重新核对服务端发布文档 §“新引擎”第一、二步后，调整先前将两边完整验证串行执行
的推进安排：服务端候选 CI 依赖已打包且身份有效的引擎部件，可以与引擎尚未结束的
映像验收及 warm-prime 并行。两边完整通过后才合入、发布引擎；补丁台账提升、正式
发布归档的两次下载及摘要核对、服务端删除候选文件后的最终 CI 均保持原有要求。
这是验证调度的调整，不将部分成功视为完整引擎验收。

引擎 82e33ba 的四个原生构建、两个兼容性底线包、两种架构的映像及布局探针，
以及 bundle 身份检查均通过。五个部件均已上传，并核对运行号、头提交、大小、
摘要及未过期状态。实际 Linux 底线包的二进制摘要及主补丁系列摘要也已复算。
其 engine.json 的 fork 为 GitHub 合成合并提交
`df31cf255657f958c5128402980dff9718f1999b`；API 记录的父提交含 82e33ba，树
逐字等于该 PR 头的树。此项不将合成提交误写为 PR 头。

随后推送服务端提交 `191dc4d1d82c399771483ff625cb2692251e5eed`，候选文件改为
运行 41739179747 和引擎头 82e33ba。
[服务端联合 CI 41744249054](https://github.com/Sunrisepeak/mcpp-language-server/actions/runs/41744249054)
已触发，与四宿主引擎映像验收并行。公开平台 0.23.4 的六目标锁文件保持原摘要，
载荷锁与 CLANGD_VERSION 仍指向已发布的 .7，待 .9 正式发布后在同一提交中更换
并删除候选文件。PR 描述明确记录完整引擎验收仍在进行，没有宣称候选已完整合格。

PCH 缓存另按 LLVM 原生构建已经采用的参数完成受控验证：原始 PCH 为未命中、命中，
改变头文件并重建 PCH 后为未命中、命中，两组缓存对象分别与独立调用编译器生成的
对象逐字相同。四次调用均可缓存。该配置采用上游对 PCH 和时间宏的假设，不承诺
任意含时间宏源码的输出；未据此推断完整 LLVM 的缓存命中率或用时改善。配置仅在
本地准备，待实际完整引擎验收后随必要的台账提交及其完整 CI 一同验证。


## 40. 编译器预热与独立构建的依赖更正（2026-11-04，Sunrisepeak）

服务端首次联合运行 41744249054 的八项工具及静态检查已通过，macOS 编译器预热
仍排队，所有交叉构建因依赖整个预热矩阵而未开始。核对四个消费者后，它们均自行
调用 setup-mcpp，且不读取预热矩阵的输出。提交 aec2630 仅移除 build、host-platform、
server 和 clion-verify 对整个矩阵的控制依赖；任务名、预热矩阵、消费者矩阵、
步骤及命令逐项保持一致。结构化 YAML 比较通过。

[后继 CI 41744974822](https://github.com/Sunrisepeak/mcpp-language-server/actions/runs/41744974822)
已触发，前一运行由原有 cancel-in-progress 策略取消，不能记作完整候选通过。
macOS 编译器及功能检查仍为整轮验证的一部分，不适用索引发布的排队例外。
新的拓扑使其他宿主的工作可以先进行，完整候选及发布检查仍须全部通过。


## 41. Windows 载荷发布刷盘的访问权限（2026-11-04，Sunrisepeak）

运行 41739179747 最终为十三项成功、一项失败及一项跳过。Windows 映像验收中，
worker 已成功构建模块：墙钟五十二毫秒、处理器四十六毫秒，随后四毫秒内的先决模块
发布报告 Permission denied。五次模块导入检查均返回 3，并行模块检查也返回 3；
warm-prime 因前置失败跳过。此轮不满足完整引擎验收，主补丁 0043 及映像
0002–0006 保持 stabilizing，不能依据其他宿主成功提升台账或发布引擎。

发布代码的两处常规文件刷盘路径以 O_RDONLY 重开载荷；公开 Windows 后端将该操作
映射到 FlushFileBuffers。[Microsoft 接口文档](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
要求句柄具有 GENERIC_WRITE。映像补丁 0006 因此仅按原生 Windows 路径风格将这两处
载荷重开改为 O_RDWR；目录、普通读者及 POSIX 载荷保持原模式。身份校验及实际刷盘
失败传播保留，错误增加操作名。独立原生 Windows 对照运行 41746593503 已提交，
覆盖只读刷盘拒绝、可写刷盘、内容与大小保持；记录时尚在排队，不计作执行成功。

按顺序重建四十三个主补丁及六个映像补丁后，缓存实现及平台适配的三个目标文件与
修正源码逐字相同。本地以 LLVM 18 编译从真实修正源码提取的辅助函数，确认 POSIX
只读刷盘、载荷内容与大小保持，及带操作名的错误保留 EACCES；十项发布门禁与包身份
对照通过。这些小范围检查不能替代完整 LLVM 23 构建及四宿主映像执行。

§39 所述等待现轮通过后合并 PCH 配置的安排随该实际失败调整：已有的 PCH 配套配置
与必要的 0006 修正合为下一轮完整验证，避免为缓存配置单独启动大型重建。完整 LLVM
命中率和耗时改善仍待实际统计。服务端 41744974822 的独立源码、构建及工具验证继续
并行，但其候选仍指向已失败的 82e33ba，不能视为合格的联合发布验证；修正引擎的
完整打包部件及身份检查就绪后，再更新候选。


## 42. 载荷刷盘对照与完整重建（2026-11-04，Sunrisepeak）

原生 Windows 运行 41746593503 的十四项检查全部通过：只读句柄可读，但刷盘返回
ERROR_ACCESS_DENIED；以可读写且不截断的模式重开后刷盘成功，内容及长度保持。
artifact 13283168509 的 ZIP 摘要为
`276dfd0b26c747d97111ad09b672f32bd0351aa8c65aec6c438155cffd67a3af`，
回收文件中的检查数、零失败数及只读拒绝结果均已核对。

引擎提交 d87eea43ccd313e1a0069a4faed999c71a963f8b 已推送，将映像补丁 0006
与 PCH 配套缓存配置合为必要的同次重建。
[完整 CI 41747171954](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41747171954)
已开始；补丁状态仍为 stabilizing。原生内核对照仅确认访问权限条件，最终映像中的
模块导入、缺失头文件失效及四宿主 warm-prime 仍须实际通过。


## 43. 服务端发布脚本登记的修正（2026-11-04，Sunrisepeak）

服务端联合运行 41744974822 的 Linux 开发构建在仓库不变式检查失败：新增
.github/scripts/publish-jetbrains.py 尚未登记到 tools/devtools/scripts.allow。
补充脚本路径及其 CI 发布用途后，使用实际已发布的 mcpp 2026.11.3.1、公开平台
0.23.4 构建并运行原有 devtools check all，全部通过，七个受管脚本均有用途说明；
二十二包、六目标锁文件逐字保持。修正本地提交为 47eb181。

该登记修正待下一组完整引擎部件及身份检查就绪后，与服务端候选更新一并推送，
避免为仍指向已失败引擎的候选单独重建整个矩阵。现轮其余独立检查继续保留结果；
先前失败不改记为成功，候选及最终发布验证仍须重新实际通过。


## 44. PCH 缓存配置的实际完整构建结果（2026-11-04，Sunrisepeak）

运行 41747171954 的两个映像构建及布局检查均成功。ARM 的 2 592 次编译调用中，
2 562 次可缓存，占 98.84%；x86-64 为 2 563 次，占 98.88%。两者均不再出现
PCH 前置条件未满足的不可缓存原因。此前两项构建各有 2 482 次该类拒绝。
本轮两者仍均为冷缓存，所有可缓存调用均未命中，因此只能确认可缓存性改善，
不能据此报告重复构建的命中率、耗时改善或映像启动性能收益。

ARM 布局 artifact 13284439473 的 ZIP 摘要为
`954aa86fa9b66a57d5be976f3634a99de834b2f34b5bb2350e5f5a6d4d9a3127`；
x86-64 artifact 13287260650 的 ZIP 摘要为
`37a0d989fcaf47fa24622e9eb6b83ef83a9d2be85137aa34dbbcdba209debb8a`。
两项仍使用既有的 304 单元、176 模块探针，工作进程受监督、缓存属于测量目录。


## 45. 缺失头文件验收的语义加强（2026-11-04，Sunrisepeak）

运行 41747171954 最终为十三项成功、一项失败及一项跳过。Windows 映像中的模块
导入及缺失头文件检查已成功，表明 §41 的实际发布权限失败在最终映像中得到修正。
本轮失败发生在 macOS：新增搜索路径前部的头文件后，日志记录模块实际重建、
零错误，但验收要求特定的“先前不存在”重建理由，而本次走的是“命令及先决输入无
现存 BMI”的重建路径。warm-prime 随前置失败跳过，本轮不能提升台账或发布。

补充对照保持模块、使用者及编译命令在三次调用中逐字相同。后部头文件导出的
constexpr 值为 42；第三次调用前只新增前部头文件，其值为 7。使用者根据该文件
是否存在对导入模块的值执行 static_assert，因此复用旧 BMI 会实际报错。
在 macOS 运行 41754360840 中，原生引擎五次均复用旧值并在新增后的断言失败；
相同候选的映像五次均完成首次构建、未变复用及新增后重建，断言通过。
Linux 原生的三次同型对照也均拒绝旧值。该负向对照确认探针没有绕过缓存失效问题。

正式验收提交 89d6a50 保留首次实际构建、未变复用及新增后实际重建要求，并用上述
取值断言验证结果，不再要求唯一的诊断措辞。README 同时更正主补丁数为四十三，
并如实说明发布要求两个台账均为 steady。
[完整后继 CI 41755260087](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41755260087)
已开始。主台账 0043 及映像 0002–0006 仍为 stabilizing；小范围对照不代替新一轮
四宿主完整验收。macOS 对照原始 artifact 13287795264 的 ZIP 摘要为
`f1adc50311482cb0f521f9f531af40b1721830e78686f044583a7f885ded22d8`。


## 46. 服务端 Windows 开发测试的峰值磁盘占用（2026-11-04，Sunrisepeak）

服务端候选提交 765ec10 的联合运行 41753047474 最终为二十一项成功、一项失败及
十六项跳过。失败任务是在 Linux 上交叉构建 Windows 开发测试，LLVM 明确报告
No space left on device；四十三个根测试中三十七个已构建，余下六个链接失败。
这不是原生 Windows 的测试执行结果，也不能记作完整候选通过。

本地修正 37281b0 仅调整 Windows 开发行的测试归档顺序。由真实发现结果生成互斥
且完整的子串筛选组，每组至多四个测试；每组完成既有配置的构建及归档后，核对
归档中的目标、测试集合及可执行文件摘要，再回收已经归档的对应可执行文件。
公共对象、模块、程序及其他七项构建矩阵的命令保持。原生测试任务原有的归档循环
会执行所有分组归档；完整覆盖记录及各归档摘要随 artifact 保留。

本地按实际发现的六十二个测试验证归档控制逻辑，覆盖无遗漏、无重复、公共文件
保持，以及遗漏、重复、错误载荷、缺少程序、截断归档、筛选重叠的先拒绝后回收
边界。该检查未执行编译器或 Windows 测试。另在独立诊断分支以生产工作流的原有
步骤执行 Linux 交叉构建与 Windows 原生归档测试；记录时仍在进行。正式联合候选
将随新引擎部件就绪一并更新，仍须完整 CI 通过。


## 47. 构建尾部等待与嵌套构建进度（2026-11-04，用户补充）

用户报告 Building 1630/1636 长时间保持，而 llvm:clangd-libraries 的运行时间持续
增加。源码确认 openkal/clangd/build.mcpp 将 LLVM 静态库构建声明为一个 prepare
动作；okgen 内部再启动 CMake 与 Ninja。因此分子和分母计外层构建步骤，整个内层
LLVM 构建只占一步，并非时间百分比。完成大部分外层任务后，仍可剩余大量实际编译。

进度行中的第一个时间为本轮构建已经消耗的时间，动作后的时间为该动作已经运行的
时间，均不是预计剩余时间。非交互日志每六十秒输出当前状态；定时输出只能确认
状态显示线程仍在工作，不能单独证明编译器仍有处理器或 IO 进展。

上一轮 x86-64 映像任务 126302992405 也在同一外层计数等待，最终构建成功。
构建总结为三十六分三十七秒，其中 llvm:clangd-libraries 为三十四分钟；该轮
2 563 次可缓存编译调用全部未命中。这给出了真实冷编译而非死锁的已完成案例。
用户所贴片段本身仍不足以判定其运行是否出现调度、内存或磁盘异常。

此处确认的局限是嵌套构建的可观测性：外层步骤计数不能表达内层编译的完成量。
后续进度改进应提供真实子进度，或明确显示外部构建动作的阶段及等待；不能把定时
心跳伪装成实际完成量。当前未为该显示局限改动构建调度或另行发布 mcpp 版本。


## 48. warm-prime 运行器输入的一致性更正（2026-11-04，Sunrisepeak）

逐项核对验收输入发现，build-test.yml 中 warm-prime 的辅助项目仍固定平台
0.23.3，而映像、封装与四宿主验收使用 0.23.4。该辅助项目会安装旧工具组；随后
按全局工具缓存排序选取 okrun 也不能证明取到所声明平台的工具。因此即使运行
41755260087 其余步骤成功，也不能据此声称完成了当前工具组的四宿主 warm-prime。

提交 776c4d5 将该辅助项目固定为 0.23.4，并让 warm-prime、封装及四宿主验收均从
mcpp self env 的结构化结果读取 runner 或 okld 路径。矩阵、探针工作量、会话次数
及原验收阈值保持。新增发布输入回归从映像项目读取平台版本，检查两个工作流中的
固定值一致，并拒绝按全局缓存通配符选取工具；原 89d6a50 工作流的负向对照失败，
修正后的六项发布门禁对照通过。

本地使用已发布的 mcpp 2026.11.3.1 和公共平台 0.23.4 构建最小映像，self env
返回工具组 0.4.4 的准确路径；所选运行器实际执行及所选 okld 的映像检查均成功。
这是公共源包构建工具的本地控制，不证明这些工具与发布 ZIP 中的二进制逐字相同。
最终发布性能测量仍使用独立验证的已发布二进制。

[新完整 CI 41758340462](https://github.com/Sunrisepeak/mcppls-clangd/actions/runs/41758340462)
已触发。资格收集器同时保存语义探针、warm-prime 源码及固定图，核对未缩减的
304 单元图的摘要，并检查四宿主 warm-prime 的实际运行器来自 0.4.4 工具组。
Windows 路径仅在核对版本时统一分隔符。主台账 0043 及映像 0002–0006 保持
stabilizing，待准确输入的新完整运行提供证据后提升，再对齐服务端联合候选。


## 49. 编译缓存清理与原始证据归档（2026-11-04，Sunrisepeak）

按用户限定的相关项目范围，已清理引擎 PR16 的七个旧映像编译缓存快照，共
187 134 137 字节。保留计划生成时两个架构各自最新的快照，删除后通过 GitHub API
回读核对；这两个快照均已被正在运行的后继构建读取。未将 sccache 的逐对象条目
误当成独立整轮快照，也未删除其他引用的缓存。

第十四份永久检查点为 mcpp-885-semantic-canary-checkpoint-evidence.tar.gz，
大小 13 452 018 字节，包含 1 074 个逐项摘要核对的文件，压缩包摘要为
`274d5f64621e13ad84d27fa6099e0252a4490c4f9c6789fa1b7c911f116e02dc`。
它保存 macOS 原始对照 ZIP 及源码、两个映像布局原始 ZIP、引擎 d87 与服务端 765
完整运行日志、语义对照、归档控制及本地运行器选择证据。首次引擎日志传输发生
HTTP/2 流错误，其不完整 ZIP 未被用作证据；替代传输及全部 ZIP 的 CRC 已核对。
该检查点不包含尚未完成的 CI 结论或最终发布性能数字，前十三份永久检查点保持。


## 50. 分组归档校验的单遍读取及容器校验（2026-11-04，Sunrisepeak）

服务端本地后继提交 5ab2909 将分组归档校验改为顺序读取：遍历时计算载荷摘要，
并读完 gzip 尾部后才回收已经匹配的可执行文件。此前先遍历成员、再按成员回读，
会在压缩流中回退，重复解压大型调试程序。六十二个真实发现名称、四十八个归档的
固定对照中，回退次数由四十八次变为零；该数字描述对照的读取模式，不是 CI 用时。

原校验的负向对照还表明，损坏 gzip 校验尾部而不改变解压载荷时，程序仍可能
接受归档并回收可执行文件。新读取器在任何回收前拒绝此输入。完整覆盖及七类
负向输入对照通过，公共对象、模块与程序保持。独立运行 41756189512 仍验证原有
分组构建与原生 Windows 执行流程；新读取器待随正式联合候选的完整 CI 实际运行，
本地控制不代替该门禁。

后继本地提交 5bf5919 为已经压缩的测试归档设置上传层 compression-level 为零。
该输入已按 actions/upload-artifact v7 的定义核对，调整的是外层 ZIP 的存储方式，
测试归档内容、编译配置及测试命令保持。此调整同样待正式联合候选 CI 验证。


## 51. 恢复编译缓存后的实际命中与身份对照（2026-11-04，Sunrisepeak）

后继运行 41755260087 的 ARM 映像步骤明确恢复了前轮的 298 902 136 字节缓存。
本次 2 562 / 2 592 次调用仍可缓存，但命中为零。构建用时二十三分五十九秒，
其中 LLVM 库动作二十二分二十五秒；宿主与负载未受控，不能将与前轮的用时差
解释为缓存收益。布局原始 artifact 13289540925 的 ZIP 摘要为
`900c48534a373652cbf9db543446a28429d787a650d467729087a26aebd57cde`。
此前“重复命中尚未测量”的状态据此更新为“已观察到一次恢复后仍全部未命中”。

独立对照使用真实 Clang 23.1.3 的自有副本及 ccache 4.9.1：编译器字节不变、
仅修改时间变化时，mtime 身份模式两次均未命中，content 模式为未命中、命中。
随后仅改变 ELF 末尾不参与执行的一个字节，保持大小与修改时间，content 模式
重新未命中，下次未变调用命中。六个对象均与独立调用原编译器的对象逐字相同，
原安装编译器的内容及修改时间保持。

该对照证明身份模式的行为，尚不能证明实际完整 CI 的全部未命中由何处造成，
也未证明完整 LLVM 或 PCH 的跨安装复用。不同安装前缀的编译器还可能因 ELF
运行时路径修正而具有不同字节；不能仅凭版本号假定编译器相同。生产缓存参数
未因此更改，后续调查应保留实际编译器身份及 PCH 输入证据，不为推测另行启动
完整 LLVM 重建。最终发布引擎与服务端的性能数字仍待 T10 的准确发布输入测量。


## 52. 准确运行器输入下的性能门禁失败（2026-11-04，Sunrisepeak）

运行 41758340462、引擎提交 776c4d5 最终为十五项成功、三项失败。四个原生构建、
两个 Ubuntu 20.04 下限、两个映像及布局、封装身份、四宿主功能与缺失名字语义验收
均通过；Windows warm-prime 按原有路径条件通过。其余三个宿主均在 warm-prime
的 Acceptance 步骤失败，而非模块无法构建：这三个宿主的每个首会话均实际构建
一百七十六个模块，后续三个会话均复用三百四十一个模块且不再构建，语义就绪
检查均成功。原生 Windows 使用进程内编译及非 owned 路径，其原有门禁记录比值，
但不将该比值作为同路径的性能通过条件。

Linux x64 的冷会话处理器时间为映像 15.144 秒、原生 11.696 秒，比值 1.295；
warm 中位处理器时间为 5.49 秒对 4.55 秒，比值 1.207，均超过 1.15。
Linux arm64 冷会话为 13.726 秒对 11.319 秒，比值 1.213，超过 1.15。
macOS 冷就绪时间为 14.922 秒对 11.619 秒，比值 1.284；缺页为
320 382 对 117 855，比值 2.718，分别超过 1.15 与 2.0。

这些是实际未发布候选及当前源工具组的门禁结果，不是最终发布测量。旧输入运行
41755260087 也已结束，十四项成功、四个 warm-prime 失败，不能作为替代资格。
两个观察进程均已终止，不能将观察终止误当成仍在运行的完整验证。
主台账 0043 与映像 0002–0006 继续保持 stabilizing，服务端候选仍未更新或发布。
下一步使用准确候选的原始数据分析 Linux 处理器开销及 macOS 缺页来源；保留原
探针、会话和阈值，不通过缩减工作量或改变标准使门禁通过。


## 53. Windows 开发归档的实际覆盖与产物体积（2026-11-04，Sunrisepeak）

独立诊断 41756189512 的 Linux 构建任务 126332740455 已成功。逐项解析原始日志中
四十八条归档回执，与真实发现列表比较，六十二个测试均恰好归档一次，所有归档
摘要保留，归档后回收的可执行文件逻辑大小合计 12 702 464 000 字节。
原生 Windows 任务已执行完归档，三个测试程序失败，尚不能计为通过。清理后磁盘
采样的最小空闲量为
76 940 771 328 字节；这是清理后的采样，不是峰值，也未复现原失败机器的容量。

实际 artifact 13290692289 为 9 355 073 547 字节，摘要为
`b0e7e89807d6fffec071b3cbc264b03601d5b82b3dd0b73c8d664649a46a01a0`。
每个分组重复携带包的程序使体积过大，当前原型尚不作为最终联合候选推送。
后继应将已验证的分组流式合并，每个包只保留一份公共程序及一个完整测试清单，
再核对最终容器的测试覆盖、文件摘要及元数据。中间采用 mcpp 支持的未压缩 tar，
避免对公共程序反复执行 gzip；同时保持一次只持有一个分组的可执行文件及中间包。
该改进尚未实现，后续实际 Linux 构建及 Windows 执行仍为必须的验证。


## 54. 原生 Windows 归档执行发现的失败（2026-11-04，Sunrisepeak）

任务 126351796514 已结束。devtools 的 test_bench_review、根的 test_grants、
pack 的 test_payload 失败。前者的两个快照案例及后者的十个载荷案例在文件系统
weakly_canonical 中收到 Invalid argument；实际调用点使用 filesystem::relative，
不是测试代码直接调用 weakly_canonical。test_grants 的两项新安全案例有三条
断言失败，分别涉及合并后父目录的 immutable 属性及过多可写授予的保留。

执行日志明确显示 Windows GNU 对象 ABI 下的实际 C 库为 musl 0.27.4、C++ 库为
openkal-llvm-runtime 0.18.5，平台为 hosted 0.23.4。因此需核对 hosted 路径语义及
具体调用目的，不能假定这是普通 MinGW 标准库，也不能仅以跳过测试处理。
当前尚未确定根因或修改这些产品调用点；后继修正须保持符号链接、目录范围及
授予边界，并在实际 Windows 上验证。此诊断不构成服务端完整候选通过。


## 55. Windows 开发归档的合并实现与路径修正（2026-11-04，Sunrisepeak）

服务端本地提交 723d633 将九处目录走查所得路径的相对名称改为词法计算，保留
目录项在原树中的名称，避免为映射复制目标或快照键而解析符号链接的物理目标。
授予数量测试以已解析的根目录构造，原断言保留；另增案例核对过多可写根目录的
每个输入拼写及物理别名均被保留，未改为无限制授予。Windows 原失败的原因与
修正效果仍需原生执行核实，尚未声明底层标准库路径处理已修复。

Linux 本机复测通过：test_grants 九个案例、一百零三条断言；test_payload
十五个案例、七十八条断言；test_bench_review 十一个案例、二十六条断言。
这些结果仅证明本机对应程序的行为，不替代 Windows 执行。

提交 625a101 将分组运输改为每包一个最终归档。mcpp 先产生未压缩 tar，脚本
前向校验并将载荷写入该包的 gzip 容器。同路径公共程序的摘要、大小及权限均
一致时才去重；不同测试的路径不合并。各分组的元数据保持一致，测试清单取完整
并集，重命名时去除旧 PAX path 与 linkpath，避免旧前缀覆盖新名称。

每组的可执行文件先与归档内容逐一比较，整组有效才回收匹配的文件；对象、模块
及公共程序保留。最终容器关闭后重读全部载荷、清单与 gzip 尾部，覆盖与摘要
一致后原子改名。控制测试覆盖真实发现的六十二个测试，并拒绝十三类分组或
最终容器错误。仓库不变式检查通过。

专项分支 9ed9564 的运行 41770334975 复制生产构建和执行任务，仅选取 Windows
dev 矩阵行，全部测试及调试选项保留。运行已启动；此处尚无实际归档体积、构建
耗时或 Windows 通过结论。服务端正式分支尚未推送这些本地提交，最终联合
候选仍需完整 CI。


## 56. 准确候选的本地性能归因记录（2026-11-04，Sunrisepeak）

776c4d5 的原生 Linux 包与映像包均已下载并核对 artifact 摘要。原生包的三百一十
个文件通过 SHA256SUMS 校验，二进制摘要为
`ccaa8f510abb8a54eebd10381957413cb1a3a93dc6d4dc776ccff634bd9d2b68`。
原生补丁序列、构建提交的合并父节点及树均已核对。映像束的两架构成员也与
okb.json 中的大小及摘要一致；这些检查不是完整性能资格。

同一本地 Linux 环境分别使用隔离目录运行原探针的三百零四个单元。四次会话均
就绪，冷会话构建一百七十六个模块，三个暖会话均未重建。原生暖会话处理器
时间中位数为 2.74 秒，映像为 4.43 秒；就绪时间分别为 1.493 与 2.132 秒。
这是候选的本地复现，不能用作已发布版本的最终数字或替代失败的 CI 门禁。

另行跟踪三个暖会话的文件系统调用，原生失败路径查询为 288848、289014、
288930 次，映像为 289097、289029、289128 次。原始跟踪均已无损压缩并记录
解压后摘要。两边的查询数量接近，额外处理器时间的原因仍未确定；带跟踪会话
的耗时不参与验收，也尚未据此声明第三级性能通过。

映像的独立调用、阶段及处理器采样报告已保留。涉及调用计时本身的样本需与
产品开销区分，阻塞等待的调用耗时不能累加为处理器时间。macOS 专项运行
41770916075 复用相同候选并按平台 0.23.4 构建工具，尚待报告。原阈值、工作量
及 stabilizing 台账状态均保持不变。


## 57. 嵌套构建末尾长任务的进度诊断（2026-11-04，Sunrisepeak）

llvm:clangd-libraries 在 mcpp 中是一项 prepare 动作，内部运行 CMake 与 Ninja
构建 clangd 的静态库。Building 1630/1636 计算外层已完成任务，不按预期耗时
加权。任务右侧的时钟仅表示该动作已运行时间，其刷新不证明编译器仍有进展。

外层 Ninja 缓冲该命令的输出，mcpp 默认又不展示成功步骤的普通输出。使用真实
Ninja 1.12.1 的三步嵌套控制，每步运行一秒，默认及 verbose 两种方式均在外层
动作结束后才输出内层三行进度。历史 x86 映像任务曾保持同一外层计数超过
三十三分钟，最终成功，该动作共三十四分钟。

另一次实际 ARM 构建恢复 298902136 字节的 ccache，但 2562 次可缓存调用均未
命中，库动作耗时二十二分二十五秒。恢复缓存不等于复用了编译结果，未命中的
具体原因尚待核实。已确认的进度可见性不足与缓存效率问题分别记录；现有日志
不足以认定调度死锁。后继可独立显示嵌套动作的阶段及子进度，不把外层任务数
表述为时间完成率。


## 58. 原生归档构建及最终运输体积（2026-11-04，Sunrisepeak）

运行 41770334975 的 Linux 交叉构建任务 126378300821 已成功。逐项比较实际日志中
四十八个分组和四个最终回执，六十二个测试均恰好保留一次。每包最终清单的测试
集合与发现列表一致；公共程序去重后，根包为四十三个测试、四十八个载荷文件，
其余三包分别为八个、十个、一个测试。

最终 artifact 13296016507 为 2 463 309 786 字节，摘要为
`83c0c19a1f2f73ac49c8758bef1b917dae79d8b89fdd993f42ae32a9f6cd06cf`。
相较此前 9 355 073 547 字节的分组产物，体积减少约 73.67%。本次归档步骤耗时
1535 秒；机器与负载未控制，不据此声明性能加速比例。整轮回收可执行文件逻辑
大小 12 702 354 944 字节，清理后最小空闲量 84 196 241 408 字节，仍不作为峰值。

原生 Windows 任务 126387978453 已开始执行四份归档，结果尚待核对。这份实际
构建证明运输方案保留完整覆盖并减少重复，尚不能证明此前三个 Windows 程序的
行为修正通过，也不替代服务端正式联合候选 CI。


## 59. 缺页采样边界及 Linux 处理器计数修正（2026-11-04，Sunrisepeak）

macOS 专项 41770916075 成功，未采样会话中，原生冷会话缺页为 126640，映像为
309248。专项 41772175101 在 LSP initialize 之前取得两边的实际 vmmap。映像指令
仍为映像束文件的映射，准备数据为 okseg 的写时复制映射；因此不能将差距归为
装入器复制了全部指令。首次冷启动、预取及采样自身的开销仍需分别核对。

后继专项 41772570686 比较周期采样与内核完整进程树总账，并以短生命期孙进程
触及六十四 MiB 的控制确认内核会累计被回收后代。原生就绪时采样缺页 135516，
内核完整运行总账为 508628；映像分别为 295915 与 699903。两种算法所得比值
分别为 2.184 与 1.376。完整总账包含 Python 探针及关闭阶段，不能直接替换就绪
时的统计；该证据说明采样漏计的程度在两条路径上不一致，尚未修改原缺页门禁。

源码审查另发现 Linux 冷会话报告在 stop 之后再读取 proc；此时进程已消失，
导致先前取得的内核处理器时间被采样及 worker 日志的回退值覆盖。本地提交
f3ee948 按宿主选择原有算法，Linux 保留就绪时的内核值；另增加明确标注边界的
waited_usage，记录引擎完整生命期及已回收后代的内核统计，不含 Python 探针。
异步重挂至其他父进程的后代不计入该字段。原缺页指标、阈值与工作量均保持。

五项回归通过，包括直接驱动实际 session 报告函数，以及真实被回收孙进程。
旧 776 报告函数将给定的内核值 12.345 改写为 2.01，修正后保持 12.345。
复用原候选完成同一三百零四单元图的四次会话，两边均语义就绪且暖阶段无重建。
修正后本机冷处理器比值为 7.83 / 6.61 = 1.185，暖阶段为 2.54 / 2.13 = 1.192，
仍超过 1.15；不得将计数修正写作引擎性能已通过。提交尚未推送到正式 PR，
原完整运行的失败状态及 stabilizing 台账保留。

第十五份永久检查点 mcpp-885-consumer-diagnostics-checkpoint-evidence.tar.gz 保存
2193 个逐项核对文件，大小 101119999 字节，摘要为
`396f81ae860bcb1f1dba551bdf730e6287fdefa51bc1ce614f6f8f97e74b86fa`。
其中保存准确候选的原始 ZIP、本地双路径跟踪及独立 macOS 采样报告；后取得的
映射、总账对照、计数修正及 Windows 构建终态另行保存，不追写既有检查点。

## 60. Windows 完整执行及归档回归终态（2026-11-04，Sunrisepeak）

专项运行 41770334975 已成功结束。原生 Windows job 126387978453 实际执行
六十二个程序，名称与发现清单逐项对应且各出现一次；四个包均无失败。
先前失败的 test_bench_review、test_grants、test_payload 均通过。生产源码与
625a101 相同，专项提交仅多一个诊断工作流；不得据此宣称底层标准库的
weakly_canonical 已被修正。

归档构建的四个最终包共 2463309786 字节，对先前 9355073547 字节减少约
73.67%。实际归档步骤为 1535 秒；它与旧原型不是受控速度对照。构建过程中
回收 12702354944 字节可执行文件；清理后可用空间不是峰值占用证据。
原生执行日志摘要为
`69c05125001e5b53e3316a4ab96c2db04b302ca317aef807bdbbd6bffc345251`。

生产提交 625a101 已推入 server PR 75，完整回归 41775013004 正在运行。
该回归仍使用先前冻结的 d87 引擎候选；其结果只能证明当前 server 源码回归，
最终合格引擎的联合资格及发布后的精确依赖仍需另行完成。

## 61. 准确映像的处理器采样与临时区初始化（2026-11-04，Sunrisepeak）

宿主 perf 权限现已可用。对冻结 776 构件各执行一次冷阶段及三个暖阶段，
两边均保持三百零四单元图、四次会话、暖阶段零构建及语义就绪；采样只在暖
阶段开启，使用 499 Hz cpu-clock 与帧指针，两边均限定处理器 0–7。
该限定及采样开销使这组数据仅用于归因，不代替最终验收。

映像束内准确 ELF 成员偏移为 0x49d0000。解析器依据程序头与符号表转换
文件偏移，调用栈另以准确进程的 perf map 校正加载基址。三个映像会话的
4812 个叶样本中，214 个位于 memset，约 4.45%；其中 79 个来自
__okm_syscall，24 个来自 okm_unlock。内核符号仍受限制，未解析的内核样本
不能继续归到具体函数。

准确构件反汇编确认：__okm_syscall 在分派前清零 8224 字节，okm_unlock 在
检查待关闭数量前清零 10752 字节。发布 0.27.4 源码中，这些自动临时区位于
公共入口；映像启用的自动变量初始化使原本不用该区域的调用也承担清零。
修正方向为限定临时区生命范围，保留自动清零、栈探测及其他保护。

## 62. 路由临时区的源码修正候选（2026-11-04，Sunrisepeak）

musl PR 82 的 d583f24 将五组使用临时区的原有 case 原样移动到不内联的
共用辅助函数，六个参数保持原次序传递。解锁在持锁状态检查空队列；非空
队列仍按当前上下文取出资源，并在解锁后关闭。空队列不再初始化大数组。
null 名称检查对普通末尾分量直接拒绝；null、点分量及歧义形式仍完整解析，
扫描长度不超过原解析器的路径上限。版本准备为 0.27.5，尚未发布。

本地使用实际映像编译命令重编两处源码，核对保留自动变量清零。x86_64 的
分派入口栈帧在 O0/O2 为 2712/1112 字节，ARM 为 2704/1104；慢路径继续
共用一个有界临时区。这是编译器报告的局部栈帧，不是完整调用链用量。

对照发布 0.27.4 与修正源码的实际映像，二十六项观测一致且全部通过，涵盖
2916 种路径、十二个路径长度边界、复制内容与偏移、拒绝旗标、名称修改、
时间与容量，以及四百次并发管道生命期。相同编译器、保护参数、运行器及
处理器集合下，交替执行三次完整的六十八项原语测试。修正对旧源码的中位
耗时比为 stat 0.840，单线程 open/close 0.845，四线程 0.852，单线程 pread
0.874，四线程 0.945。源码控制仅早于最终提交的包版本变更；最终提交另由
完整 CI 41777022375 验证。这些数值不代表 clangd 的性能门禁已通过。

## 63. 此轮版本组的最短依赖路线（2026-11-04，Sunrisepeak）

此轮修改仅在 musl，不改变后端、装入器或工具。所需发布依赖为 musl
0.27.5、runtime 0.18.6、platform 0.23.5，再构建合格 clangd 并完成 server
联合资格。os 直接依赖的后端保持原版本，故此轮不追加 os 发布。

runtime PR 63 使用现有同名分支图测试未发布的 musl，允许与上游 CI 同时
验证。platform 0.23.5 的版本组已在独立工作树准备，待上游版本发布并进入
公开索引后再启动其完整 CI。原有映像保护策略与验收阈值均保持。

## 64. musl 发布及 runtime 验证中的性能失败（2026-11-04，Sunrisepeak）

musl 完整运行 41777022375 的十项检查全部通过。PR 82 以 c0430c0 合入，
合入树与受测 d583f24 完全一致；0.27.5 标签及 GitHub release 已建立，家族
分支同步。GitHub 与 GitCode 归档均为 1268814 字节，摘要一致：
`458c9ef1f46446f27d0199118357ced065598f8aadf653a5a4c498a6d811b295`。
索引 PR 704 保留既有版本行，仅前移直接 C 库测试成员；本地实际运行该成员
通过。索引候选的 Linux default 与 LLVM 两项及兼容性工作流均通过。

runtime 0.18.6 的完整运行 41777349050 在 macOS 映像 L2 的 index/symbol
失败：281.13 对 212.69，比值约 1.32，超过 1.15；其功能观测通过。
此失败尚未被判定为源码修正的性能回归。基准以散列表遍历次序选择查询，
并在若干表达式中多次调用可变随机数生成器，跨编译器的输入与查询需核对。
本地 GCC 与 Clang 的原始符号集合摘要不同；本地 Clang 与公开映像的摘要
相同，故这些观测还不能解释 macOS 的失败。专项 41779143805 在同一 macOS
宿主比较冻结的已发布与候选映像，并记录编译器及工作量摘要。原基准与阈值
尚未修改，runtime 与 platform 均未发布。

## 65. Windows 标准库卷路径的独立复现（2026-11-04，Sunrisepeak）

专项 41777888621 在原生 Windows 上，使用公开 platform 0.23.4、runtime
0.18.5、musl 0.27.4 及 runner 0.4.4，对同一源码分别使用宿主 MSVC 运行时
与映像运行时。MSVC 的 current_path 为 D:/ 前缀路径且被识别为绝对路径，
canonical、weakly_canonical 及已遍历条目的 relative 均成功。映像返回同类
路径却将其识别为相对路径，absolute 重复拼接当前目录，上述三项均返回
EINVAL 22；文件的创建、遍历及删除完成，lexically_relative 返回 leaf。

该公开运行时缺陷登记为 openkal-llvm-runtime issue 64。专项成功表示两边
的观测已取得，不表示错误的映像结果通过了功能断言。server 的词法遍历
映射修正仅覆盖相应调用者，底层标准库在此轮尚无修正。

## 66. 不含探针的完整引擎内核总账对照（2026-11-04，Sunrisepeak）

专项 41777772516 的 Linux 与 macOS 均成功，五项计数控制通过。两条引擎
路径各完成原三百零四单元图的四次会话，均语义就绪且暖阶段无构建。
与第 59 节包含 Python 的外层总账不同，这次的 waited_usage 直接在每个
引擎启动前及其 wait 完成后取差，不含探针自身，包含关闭及已回收后代。

macOS 冷阶段原生就绪采样为 126581，完整引擎总账为 506753；映像分别为
294066 与 692680。比值为 2.323 与 1.367；暖阶段分别为 1.109 与 1.107。
Linux 冷阶段两种缺页比值均约 1.076，暖阶段为 1.028 与 0.878，显示关闭
阶段的额外用量在两边也不相同。因此两种统计不能在同一名称下静默互换。

原有缺页门禁仍保持。若调整测量边界，应先明确指标的阶段、回收范围及旧
记录的可比性，再统一所有支持该内核计数的宿主。此专项使用冻结的失败
776 构件，仅验证计数：Linux 冷、暖处理器比值仍为 1.286 与 1.190，不能
作为最终引擎资格，后续仍需用新 musl 版本组重建。

## 67. runtime 零点十八点六资格与发布（2026-11-04，Sunrisepeak）

musl 零点二十七点五已完成全部十项生产检查、双端源码摘要及 GLOBAL、CN
普通客户端测试。runtime 的第一次 macOS L2 index 比值为 1.32，失败保留。
专项 41780521076 在同一宿主交替执行准确冻结的旧、新映像及两个原生编译器，
每组五次，核对 bitstream、symbols 与 queries 输入指纹相同。五项新映像对原生
比值均低于原阈值，index 为 0.948；具体调度或资源原因未获证明。

据此对相同提交及原阈值仅重新验证原失败 job 一次，生产 CI 41777349050 的
十项检查均通过。PR 63 合入 f450b4d418a84a6b2b18184bc12663111fffce9f，
发布零点十八点六。GitHub、GitCode 源码均为 15642891 字节，SHA256 为
811fbb8b59344b319ecf70880708fd7c97c3325b0e47f5a8f8ec3b2f2bb4a898。
索引 PR 705 全部适用检查通过后合入 500054cd5100fdf7c40cd97b568551f033b64c83；
普通客户端指针及后继 platform 仍待核对，以上不构成最终引擎验收。

## 68. 模块 worker 的装入器只读授权（2026-11-04，Sunrisepeak）

服务端候选 625 的 Neovim 三宿主及 Linux 清洁容器出现模块 worker 启动失败，
日志为 posix_spawn 的 ENOENT。实际授权包含引擎映像目录，未包含 payload 的
libexec 装入器目录。worker 经 kal_env_loader 取得装入器，再经受授权限制的
路径解析启动；该缺项使工作区位于 payload 外的编辑器路径失效。

专项 41782955938 保持准确候选 payload 与第一次原导航断言，仅向客户端初始化
加入装入器父目录的只读授权。Linux、macOS、Windows 均在原九十秒截止之前取得
非空跳转目标，报告中不再出现该启动失败。Linux 独立 token 对照没有增加授权，
不能据导航恢复宣称暖 token 已恢复。本地生产修正将装入器目录作为默认不可变
payload 授权，九个单元用例的一百零五项断言通过；尚待最终候选的完整 CI。

## 69. L3 缺页口径版本二（2026-11-04，Sunrisepeak）

依据第 59、66 节的冻结构件审计，采用第三部分末尾具名更正块的完整引擎生命期
内核总账。page_faults 保留就绪诊断，complete_page_faults 包含关闭及等待回收
后代；版本和范围显式记录。此更正没有调高二倍缺页上限，没有修改 1.15 倍就绪
时间与处理器上限，也没有缩减三百零四单元及四次会话。

本地真实验收 CLI 的控制验证旧版本、错误范围、缺失或不一致计数、伪造暖中位数、
缩短工作量及原有时间和 CPU 超标均遭拒绝。短生命期孙进程触及十六 MiB 的内核
计数控制通过。最终新引擎仍需在所有宿主重新构建和测量，旧候选结果不提升状态。

## 70. 分离目录的冷暖 token 对照与公开 runtime 客户端（2026-11-04，Sunrisepeak）

准确 625 服务端、stage 与语义 kit 的三个原始 artifact 均核对完整 SHA256、ZIP
CRC 并解包。最初将 payload 和工作区一起放入临时目录时，两条路径均通过，因为
临时目录授权已覆盖装入器；该控制不检验目录缺项。分离 payload 至工作区目录、
保留 fixture 于临时目录后，原默认授权的冷、暖会话均缺少 completion，日志复现
worker 的 ENOENT。仅补充装入器父目录的只读授权，两次均通过原万行文件用例，
暖 compiler tokens 为 0.0378 秒，completion 为 0.1005 秒，编辑及 range 断言通过。
这组数字用于根因对照，不作为最终发布性能数据。

runtime 索引发布 41784367275 成功，普通 raw 指针已观察到合入提交。GitHub 与
GitCode 索引归档均为 664572 字节，摘要为
37a902d184744ee8909d72e2036b2c457d9f8bdfb6e84564af8cc1e8a49bec7b。
两个隔离客户端分别选择 GLOBAL、CN，均以真实 mcpp 三点一执行不改源码的 cxx
及 import-std 示例，版本选择来自普通公开索引，锁文件固定 runtime 零点十八点六
及 musl 零点二十七点五。CN 所选客户端本次源码日志实际使用 GitHub，因此此项
证明客户端可用，不宣称本次经 CN 源码链路下载；双端源码字节另已核验。

测量版本二还在冻结 776 原生及映像上实际运行三百零四单元、三个文件、四次会话，
均就绪，完整缺页与原始内核计数相符。原阈值验收仍拒绝该旧映像的冷时间、冷暖
CPU；该结果验证报告及拒绝路径，没有提升旧候选的资格。

## 71. platform 零点二十三点五发布与默认授权实际验证（2026-11-04，Sunrisepeak）

platform PR 18 的生产 CI 41784967446 四项均成功，合入提交为
b9cc96d17a9595c341bd9e9b7029d86766f22456。零点二十三点五的 GitHub 与
GitCode 源码均为 18575 字节，摘要为
e533ce5c9c68ff10f8b8748312a7e25180ae691713498aa5378e9a38ff27b01e。
版本组及映像层一致选择 musl 零点二十七点五与 runtime 零点十八点六；装入器、
工具、后端及加固参数保持。索引 PR 706 的所有适用检查通过后合入
b00684acb8e2784e86a4712ada9393b6e8f75f70，发布专项 41786984354 仍待核对。
本地索引测试已用发布源码执行 hosted 程序通过，303 个既有描述符的保留检查通过。

服务端本地生产修正 4d591a6 的实际 hosted 可执行文件完成冷、暖 token 检查，
fixture 没有附加授权，仍采用原参数及预算。该控制使用冻结的旧引擎、装入器与
语义 kit，证明默认授权代码可用，不构成新映像组的完整资格。旧服务端 CI
41775013004 在记录 49 项成功、十项实际失败及一项跳过后取消剩余任务，保留
原失败。最终服务端将与新引擎构件一起重新验收。

第十八份永久证据归档为 mcpp-885-runtime-loader-checkpoint-evidence.tar.gz，
87541419 字节，1589 个文件逐项核对摘要，归档摘要为
7fe7bf86327ac90e63249bc1f9329c048da4e18f4e778f444a0244590dce7611。
该归档覆盖本轮冻结载荷、作用域控制及 runtime 发布；platform 发布与后续
默认代码实际验证产生于其后，将由后继归档保留。

## 72. 新版本组实际消费与引擎重建（2026-11-04，Sunrisepeak）

platform 索引发布 41786984354 成功，普通 raw 指针已到合入提交。两个端点的
索引归档均为 664633 字节，摘要为
7e82b3766b44738ea6670ccda8e3629f63932f20de4d32945f4c88a7d8a3076a。
隔离 GLOBAL 客户端用真实 mcpp 三点一、显式 x86_64-openkal 目标构建并执行
程序，输出四十二，锁文件确认 platform 零点二十三点五、runtime 零点十八点六、
musl 零点二十七点五及 tools 零点四点四，均来自普通公开索引。

两项辅助验证曾并发共用归档输出路径，其中一次摘要断言失败；该次未作为镜像
损坏的结论。辅助脚本现改为进程独立下载临时文件，验证后原子替换，后续双端
摘要和实际消费均通过。未显式指定 openkal 目标的早期原生执行只作普通命令
控制，不计入上述平台消费证明。

服务端本地提交 9e0c1d4 将 manifest、host graph CI 及原六目标依赖锁切换至
新版本组。仍为二十二个索引包，仅 platform、musl、runtime 三个记录变化，
锁摘要为 fa8c751d8e119d8e79dad7a6829727945a8ad93d3b3dca65ad60b0a28021b939。
本机构建及 grants、templates、process 共四十四个用例、三百四十六项断言均通过，
构建临时增加的 xxhash 单列保留后恢复上述规范锁文件。尚未推送，等待新引擎构件。

引擎 cfd2243 已推送，完整 build-test 为 41788130200。初步 Linux 原生构建通过，
其余宿主、映像及后继性能检查仍在进行。独立 verify-series 41788130169 已应用
主系列及映像系列，随后发布账本门禁拒绝仍为 stabilizing 的 0043；普通账本
一致性检查通过。该条目及映像条目二至六仍需在完整引擎、服务端联合资格满足后
提升并完成最终检查，不能在此阶段发布。Windows 历史崩溃采集专项已通过。

## 73. macOS 规则容量与既存目录名缓存截断（2026-11-04，Sunrisepeak）

专项 41790529996 的短路径可写、只读拒绝及十六条长路径对照均达到预期。
混有反斜杠的转义用例在未隔离时已无法解析，因此不能证明规则容量。改为
只含引号的路径后，41791186105 的六项隔离行为均达到预期，但 std::filesystem
清理目录返回目录非空，整体作业失败。两份原失败及原始 artifact 均保留。

专项 41792589035 在同一 macOS 宿主比较准确公开版本组零点二十三点四及五。
两组的六项隔离结果一致：短路径可写成功、只读拒绝写入；十六条最长八百二十四
字节的可写物理路径在未隔离时有效，隔离时返回 invalid，目标未执行；最长
八百二十七字节、含引号的只读物理路径亦在未隔离时有效、隔离时因规则容量拒绝。
实际后端均为 macOS 零点二十一点三，锁文件不存在工作树代入。该结果确认原
十六 KiB 规则区不足时为拒绝启动，错误分类仍不准确；没有观察到扩大权限。

两个版本组的独立文件系统程序均能清理浅层目录，十层深目录及引号目录均返回
可移植错误三十九，系统运行时对相同结构成功。Linux 同机对照复现相同差异，
且独立程序未使用进程授权。因此该清理问题在本轮路由优化之前已存在；此结论
只覆盖已测用例，不扩展为所有文件系统行为无回归。专项成功只代表证据采集完成，
两个版本组的完整隔离程序仍保留因清理失败而返回的一，不能称为整体通过。

Linux 实际内核跟踪及两版公开源码进一步定位至 musl 的目录名缓存。目录名区
为五百一十二字节，保存时未保留溢出状态，之后把最多五百一十一字节的部分名称
当作完整绝对路径。实际 openat 把七十二字节的相对分量接在三十一字节的截断
祖先分量后，所请求路径不存在。独立问题记录及原始跟踪已保留；尚未修改该实现，
不将其归为本轮新回归，也不宣称已经证明越过授予边界或覆写其他文件。

## 74. 新引擎构件准入与服务端联合验收（2026-11-04，Sunrisepeak）

引擎 41788130200 的四个原生构建、两个 Ubuntu 20.04 兼容包、两架构映像及
通用载荷身份检查均通过，五份构件的 run、head 与 artifact 摘要匹配 cfd2243。
准入记录仅允许联合候选测试，不代替后继四宿主功能与性能门禁，也不允许发布。

服务端提交 99a27fe 将候选文件切换至该引擎构建，连同 4d591a6 的默认只读装入器
授权及 9e0c1d4 的公开版本组、六目标依赖锁一起推送。完整 CI 为 41793927769。
已发布载荷锁及编译时引擎常量保持原发布值，待双方完整资格通过、引擎发布字节
独立下载校验后，再与删除候选文件在同一提交中切换，并执行最终完整服务端 CI。

## 75. Windows 归档执行与服务端首次计划时序（2026-11-04，Sunrisepeak）

服务端完整 CI 41793927769 结束于六十一项成功、四项失败及两项跳过。
Windows 开发归档执行残留上一个解包目录，后续两个包无法解包；暖缓存观察器的
路径匹配另遗漏盘符，实际引擎已复用三个模块且没有重建。修正宿主目录清理及
盘符、反斜杠识别后，41803299158 的 Windows 开发和发布归档、实际暖缓存
一致性检查均通过。该轮整体为七十七项成功、四项失败及两项跳过，仍未合格。

该轮剩余问题包括 mcpp 冷启动首次正确作答超过一百五十秒、xlings 暖启动一次
额外重启，以及两项真实 Zed 的进程观察器失败。xlings 的异步目标选择比首次
引擎启动晚约二百毫秒，首份计划需要新增只读目录。服务端 4497dca 将受限映像
首次启动延后至首份计划或显式无计划回退；单纯运行计时器不再提前启动，也不
产生忙轮询。原生及无限制启动保持即时。既有模型回退仍能解除无计划等待。

本地服务端七十个用例、七百二十五项断言通过，准确旧引擎载荷与默认授权下，
原万行文件的冷、暖 token 用例均通过。原目标选择、切换、删除及重建场景的
十九项检查在七点九秒通过。新增两个 Python 控制脚本最初未登记，完整构建被
仓库脚本清单拒绝；5bdeddb 补充用途登记后，全部本地 devtools 不变量及二进制
检查通过。该修正保留清单门禁，没有删去脚本检查。

## 76. 物理目录视图与冷启动 CPU 定位（2026-11-04，Sunrisepeak）

引擎通用补丁 0044 直接在编译命令的绝对目录建立独立物理文件系统视图，避免
查询随后立即被替换的进程工作目录；自定义视图及无效目录的回退保持。二十二项
LLVM VFS 测试、原生与公开平台映像组件控制通过。组件构造测量不能代替完整
引擎性能验收。完整候选 6fc 的 41807943117 仍为十五项成功、三项失败：
Linux x64 冷时间比一点一七九、CPU 比一点二五七，ARM 冷 CPU 比一点一七四，
macOS 冷时间比一点一九五，均存在超过一点一五的指标。POSIX 暖 CPU 和完整
生命期缺页门禁通过，未据此提升该候选。

冻结 6fc 的 macOS 专项 41840386025 完成三百零四单元、一百七十六 worker 的
同一图，采用命令行配置只采样父进程，实际仅生成一份父进程报告。八千四百
四十七个 CPU 样本中，1491 个文件信息查询及 659 个打开样本经过拥有缓存的
后备租约查询，约占父进程样本四分之一。缓存发布样本为八十三个，不能将此前
观察到的发布日志间隙直接解释为主要 CPU 成本；并行区间之和也不是整体耗时。

Linux 同图 syscall 控制中，原生和映像的全部 stat 类调用分别为 614198 和
1448442 次，不能只比较某一种 stat 拼法。准确映像栈解析保留进程、线程及
映射事件，3063 个样本涉及 179 个映射进程，映像栈未解析帧为零。采样和跟踪
用于定位，不作为带跟踪开销的性能资格数字。

## 77. 有界账本扫描与旧编译器兼容修正（2026-11-04，Sunrisepeak）

通用补丁 0045 在持有账本锁时，每次读取十六个相邻槽位的双份记录，保留原
校验和、序列、类型及字段检查。查询先筛出稳定且键匹配的槽位，释放账本锁后
才申请候选租约；后续仍重读记录并检查命名空间、租约身份、代、载荷及完整清单。
未增加持久缺失缓存，记录布局、配额、加固参数及持久化协议保持。准入复用同一
有界读取，继续核算全部占用、溢出及独占租约。截断和不明确记录仍为错误。

原生与公开 platform 零点二十三点五映像的组件控制通过跨读取块、其他实例
发布、提示、活跃租约、配额、载荷变化、截断及损坏记录检查。新增三个测试体在
最小真实文件系统组件夹具通过；该组件仅排除无关的 CompilerInvocation 参数
检查及两个 Clang 头文件，完整 ClangdTests 仍由 CI 验证。四十次交替组件调用
中，缺失查询 CPU 中位数分别为原生基线的约百分之四十八、映像基线的约百分之
十九。十六次缺失查询的映像 openat 从 65567 次降为 31 次，newfstatat 从
65595 次降为 59 次。这些数字不表示完整引擎已达到性能门禁。

首轮 75b 的完整构建 41846781069 在四项 Linux 原生或兼容构建中失败。
旧 GCC 及 Ubuntu 20.04 的 Clang 12 不接受依赖模板返回值的隐式移动；本地
较新编译器此前接受该代码。77e4e2f 显式移动仅能移动的错误对象，本地 GCC 13、
Clang 18 组件编译、原生控制及三个新增测试体通过。主补丁先于全部映像补丁的
应用次序仍得到完全相同的受跟踪源码树。新完整 CI 为 41849712864；两项
Ubuntu 20.04 兼容打包已经通过，整体功能及性能资格尚待完成。旧失败轮次取消
剩余构建以释放队列，主账本 0043 至 0045 与映像二至六保持 stabilizing。

## 78. 首次计划修正的完整结果与真实 Zed 对照（2026-11-04，Sunrisepeak）

服务端 5bdeddb 的完整 CI 41839033098 结束于六十二项成功、两项失败、
一项取消及两项跳过。两项失败均为 U4 冷启动补全的引擎作答占比：mcpp 与
xlings 各八次请求中七次来自引擎或其缓存，一次来自回退，占比零点八七五，
低于原零点九要求。两组各二十五项场景检查中的二十四项通过；暖启动、编辑、
故障恢复阶段均通过，xlings 先前的一次暖启动额外重启降为零。mcpp 暖阶段
连续五分钟零重启、一百七十六个模块零重建。mcpp 冷阶段 U1 为一百一十七点六
秒，首次正确补全为一百二十一点五秒，仍不能代替占比检查的失败。

旧轮次各有一次冷补全回退，但冷阶段持续五或六轮，观测占比约百分之九十四或
九十五；本轮各持续两轮。该原始分母差异不能直接证明产品退化，也不能作为
降低门禁的理由。早期请求日志已被既有轮转淘汰，尚不能定位单个回退的精确
原因。当前服务器仍采用冻结 cfd 引擎，新引擎构件的联合验收仍然必要。

取消项为 macOS 洁净安装，GitHub 记录十五分钟内多次未获得 hosted runner，
没有执行测试步骤。另一 macOS 一致性作业通过。该容量问题与两项真实冷占比
失败分别保留，未合并为同一种原因，也不将缺失结果计为通过。

Zed 原观察器要求进程名以 clangd 开头，而实际映像使用 okrun。新观察器按安装
清单核对可执行文件、准确映像参数及实际服务器的直接子进程，排除死亡进程并
区分共用装入器的服务器与引擎。专项 41851760562 使用 41839033098 的准确
payload、stage 构件，源码与 5bdeddb 相同，仅调度包装不同。两项真实 GUI
作业及进程控制均通过：推荐配置十一秒、默认配置八秒。协议初始化和默认配置
下独立 clangd 的原断言保留；该专项不代替完整联合资格。

相关项目缓存清理保留每个宿主最近目录，删除已确认无活动进程使用的两个旧
服务端目录，回收 1800843264 字节。无关项目和仍供验证使用的构建目录未删除。
第二十五份永久证据归档为 mcpp-885-owned-ledger-scan-evidence.tar.gz，
12067999 字节，1827 个文件逐项核验，摘要为
d8eeeac6aeb4f7541ad8348189111263d51194558d73475b4ca859c185e1888e。
它覆盖首次账本扫描诊断，不含其后产生的显式移动修正及真实 Zed 专项结果。

## 79. 准确候选的冷 CPU 结果与后续优化（2026-11-05，Sunrisepeak）

77e4e2f 的冻结构件在专项 41862659565 完成与正式门禁相同的三百零四单元、
三个文件、每组一次冷启动及三次暖启动。两份原始构件均核对下载摘要、ZIP CRC
及准确源码，并以未修改的验收程序重算。Linux x64 冷 CPU 为八点四六秒，
原生七点零七秒，比值一点一九七，超过一点一五。该宿主的就绪时间、暖 CPU、
完整生命期缺页指标通过；ARM 的全部原门禁通过。macOS 未获得 runner，
没有测量结果。这一候选不能进入联合载荷，不能提升发布账本。

准确 x64 原始日志的一百七十六个 worker 累计 CPU 为原生二四九八毫秒、
映像三一五三毫秒；这是逐项日志之和，不是中位数乘以数量。本地同图冷采样
同样显示 worker 成本差额，并将位流读取识别为热点。采样及 syscall 跟踪的
CPU 和时间均带观察开销，不作为资格数字。

规范路径解析的属性预查询产生约六点七万次 fstatfs。七组交替局部对照表明，
省去查询可将同量成功符号链接解析的 CPU 从零点一二一零六二秒降为零点
零八七八三六秒，节省约零点零三三秒；该收益不足以单独解决完整门禁，暂未
增加 musl 发布链。账本读取块从十六槽扩大到六十四槽的映像对照没有收益，
同样未进入产品补丁。

新通用补丁 0046 仅要求 Read 与 ReadVBR64 在调用处内联，位提取、补充输入、
溢出及错误处理的函数体保持。以准确候选产生的全部一百七十六个 PCM 为输入，
每次完整解码四遍，每组原生和映像各七对交替调用，所有解码累计值一致。
映像 CPU 中位数由零点二四七七一一秒降为零点一六六八九九秒，比值约零点
六七四；原生比值约零点九九六。该组件结果不等于完整冷启动降低同一比例。
上游原有位流读写十八项测试通过，候选头文件与被测组件仅有注释及换行差异。

引擎新候选 6e3749f 的完整构建为 41876171347，新增补丁保持 stabilizing。
此前仅在非活动构建脚本准备的 VFS 覆盖已移至实际 ci/ci_build.sh，构建并运行
SupportTests 的 VFS 过滤、ClangdTests 的 FSTests，以及本轮 BitstreamTests。
本地完整应用次序的准确源码树相同，仍须由这一轮实际 CI 验证集成与性能。

服务端 161d178 的 41860882938 已终止，三十项成功、一项准入失败、四项
macOS 取消、十四项跳过。新的十二项准入对抗控制在实际 CI 通过；准入作业
正确拒绝上游 macOS 取消，未组装任何联合载荷。原生构建可以先行，载荷仍以
完整上游身份和结果为依赖；下一次准入必须改用修正后的准确引擎及构建轮次，
不能仅重试旧候选的取消项。

## 80. 位流候选的完整结果与冷启动采样（2026-11-05，Sunrisepeak）

6e3749f 的完整构建 41876171347 已结束，十三项成功，一项 macOS 原生构建因
runner 容量取消，依赖它的 warm-prime 矩阵跳过。Linux 两架构原生构建、
Windows 原生构建、两架构映像、通用载荷及四宿主基础映像验收通过。准确 Linux
原生日志确认执行二百九十八项选定 ClangdTests、二十二项 VFS 测试及十八项
位流测试，全部通过；这不是全部 ClangdTests 的覆盖声明。

同一候选的冻结专项 41884872035 完成十六次语义会话。Linux x64 冷 CPU 为
映像十五点零七秒、原生十二点零八秒，比值一点二四八；ARM 为十一点五七秒、
九点七零秒，比值一点一九三，均超过一点一五。其他就绪、暖 CPU 和缺页门禁
通过。两份原始报告经摘要、ZIP CRC 和准确输入身份核对，并由原验收程序重算。
完整生命期 CPU 仅供诊断，不能代替原就绪 CPU 边界；跨轮原生绝对时间不同，
也不足以据此断言源码退化。本候选保持不合格，不重跑抽取较好结果。

服务器 80e7c70 的 41876772794 为三十项成功、一项上游准入拒绝、三项 macOS
取消及十四项跳过。准入正确拒绝缺失上游资格，未组装联合载荷。引擎账本仍为
stabilizing，编译常量与正式载荷锁没有提前切至未发布构件。

本地以准确构件重做三百零四单元、三个文件、一百七十六个 worker 的完整冷
采样。所有映像帧均按准确 ZIP 成员偏移及 ELF 符号解析，无未解析映像帧；
位流 Read 和 ReadVBR64 自身已不再占据主要热点。输入重放在包含调用者的
采样中占一百九十六个样本，分类及路径规范化分别占六十八和六十个，存在重叠，
不能相加为独立成本。采样与 syscall 跟踪均改变时间和 CPU，仅用于定位。

最新 x64 整体构建缓存 10014569779 已保存并核验后，删除旧快照 10006490473，
记录大小一三三九六七五六五五字节。同一 PR 的各架构保留最近快照，Windows
分散编译对象及其他项目、分支的缓存保持。该大小不等于已刷新的物理用量统计。

## 81. 操作内重放复用的修正与验证范围（2026-11-05，Sunrisepeak）

新通用补丁 0047 将缓存存入时已经完成的全量输入重放记入当前操作 epoch，
同一次操作首次查询不再重复审计；下一次操作及构建后的 preamble 审计仍重新
验证。命中快速路径前明确检查取消状态。对照在准确旧代码的原生及映像中均
复现同一 epoch 的已取消查询仍返回命中，新代码均拒绝。

连续相同路径的观测共用一次规范化及来源分类，逐项文件系统操作、别名关系、
包标记检查和计数保持。重复事实在重新分类前按原完整相等关系去重。最终被测
两个头文件与补丁源码逐字节相同；四十七个通用补丁先于六个映像补丁的应用树，
与本地映像补丁先行的源码树相同。

局部测试使用真实记录器、重放器、缓存、Context、Cancellation 及 LLVM Support，
以五百个内存文件、三千条观测隔离元数据成本，不等于完整编译器或真实磁盘。
原生和公开平台映像各七组交替对照，最终映像重放 CPU 比值零点九零九，
存入后首次查询零点七零四；原生分别零点八九一及零点七二六。单纯重放的
三十六万项 project 计数、六万次 status、六万次 open 均相同。内容、负依赖、
工作目录和取消控制通过，编译安全选项没有变更。

引擎提交 31e3513 的完整构建为 41891364456。服务端 605a532 的 41891564035
仅更新这一准确候选引用，继续将独立编译与载荷准入分开。新增集成测试验证
首次存入的操作范围及命中后取消；实际入口同时执行既有来源分类测试。
完整性能及联合资格仍待当前源码 CI 验证，局部收益不提升发布账本。

## 82. 80e 服务器构建计数更正（2026-11-05，Sunrisepeak）

逐项统计保留的 41876772794 终态快照，共四十九个作业，其中三十一项成功、
一项准入失败、三项取消、十四项跳过。第八十节的“三十项成功”为计数错误，
更正为“三十一项成功”。上游准入仍被拒绝，未组装载荷，资格判断不变。
第七十九节所述 161d178 的另一轮三十项成功不在本次更正范围内。

## 83. 重放修正的实际集成与链接优化后备（2026-11-05，Sunrisepeak）

31e3513 的准确 Linux x64 原生作业 126789382644 通过三百项选定 ClangdTests，
其中新增的首次存入重放范围、命中后取消及既有来源分类用例均在原始日志确认。
二十二项 VFS、十八项位流、五项命名空间及两项正向 memo 测试通过。
Linux ARM 原生与两个 Ubuntu 20.04 兼容底线作业亦成功。准确 x64 包三百一十个
文件校验、ELF 摘要、四十七补丁及 overlay 身份、PR 合成提交的父节点与源码树
均核对。完整映像性能仍待构件齐备；macOS 原生仍为无 runner 的容量取消。

独立后备实验仅增加 ThinLTO，保留 O3、自动清零、栈保护、栈冲突保护、帧指针、
libc++ hardening 及控制流保护。原始源码逐字相同，编译参数按顺序核对后仅多
一项 -flto=thin。一百七十六个准确 PCM 每次完整解码四遍，九组交替对照的
两个翻译单元 LTO CPU 比值为零点八四六；同时将真实 LLVM Support 以 LTO
编译后为零点八三五，累计解码值一致。实际 LLVM 库交叉构建一百八十九步完成，
归档成员确认为 bitcode，最终映像封装和执行通过。该结果不是完整 clangd 的
LTO 性能或资格，当前运行的 31e3513 没有因此改变。

后备本地提交 03a3a8c 为映像 LLVM 启用 ThinLTO，链接限制两个后端作业；
优化模式同时写入生成工具链的身份，旧构建树因此重新配置。准确重建的 okgen
通过使用记录型 CMake、Ninja 适配器的模式切换控制，确认旧状态被清除、同模式
不重复配置、归档次序保留。该控制验证生成器流程，不代替完整 LLVM 配置。
实际 LLVM Support 的另一项交叉构建独立验证 LTO 参数及原安全选项。
该提交仅在本地保存，没有推送，也没有提升任何发布账本。

第二十九份永久证据归档为 mcpp-885-replay-audit-evidence.tar.gz，
60510575 字节，3312 个文件逐项核验，摘要为
3fa9a31dafd751abe63064c942982bc392d10a5f71ad734cbde6940dd56986e4。
它保留 6e 冷 CPU 失败的原始 ZIP、准确 bundle、完整 perf 数据，以及重放修正
的源码与组件构件；早于第八十二节的计数更正及其后的真实集成和 LTO 实验。
核验第二十八、二十九份归档中的准确可执行文件后，删除两个过期组件 target，
记录逻辑大小 205366817 字节，保留最新重放候选构建目录及所有源码、控制结果。

## 84. 重放候选的性能边界与冷补全回退证据（2026-11-05，Sunrisepeak）

31e3513 的完整构建 41891364456 已结束，十三项成功、一项 macOS 原生容量
取消、一项依赖矩阵跳过；基础四宿主映像验收均通过。其冻结对照 41900280136
的两份 ZIP 均校验摘要、CRC 和源码身份，并以原程序重算。x64 冷 CPU 为
十六点二四秒对十三点六四秒，比值一点一九一。ARM 冷 CPU 为十一点六三秒
对九点八五秒，比值一点一八一，暖 CPU 同为一点一八一；冷、暖就绪时间
分别一点一五五、一点一五四，亦超过一点一五。十六次语义会话及缺页门禁通过。
这一候选不合格；原生绝对时间随宿主变化，不据此声称源码性能退化。

前轮服务端 605a532 的 41891564035 逐项统计为三十三项成功、一项准入拒绝、
一项 macOS 取消、十四项跳过，没有载荷组装。进入后备优化前，旧性能采集器
与容量观察器均已关闭；因真实性能失败，没有重试 macOS 容量取消。

冻结日志专项 41897714970 使用准确 5bdeddb 服务器与 cfd2243 引擎的原载荷、
测试工具及 kit，三个构件均核对准确源码、轮次、摘要及 ZIP CRC。只增加
Python 日志观察：运行时以只读、不跟随链接的描述符保留有界日志 inode，
子进程退出后才复制内容。五项真实文件控制通过，命令、请求、预算和原
conformance 可执行文件保持。mcpp 保留八个文件 40950189 字节，xlings
九个文件 45296797 字节，均无上限触发、错误或文件截断。早期请求因此可见。

该专项直接确认 execute.cppm 的首次补全：clangd 请求编号六在十九时三十四分
五十七点三七零秒收到，三一四二毫秒后答复。请求内部计时三一二一毫秒，其中
输入检查一六零八毫秒，一一五八项逐项检查、九十五次包标记检查覆盖三二一项
观测，三二一项系统输入已检查，等待模块为零。服务器明确记录权威引擎结果在
三一五一毫秒替换 native 回退。源码的常规补全预算为一千毫秒；这确认了先
回退、后以引擎答案替换的路径，不是仅按请求总数作出的猜测。

这不是找回已轮转丢失的旧轮次日志，也不是新引擎资格。新专项共十四项冷阶段
检查，十三项通过。mcpp 七轮的补全占比为二十二比二十三，xlings 三轮为十比
十一，均通过；mcpp 首次跳转一五二点四六秒超过一百五十秒，仍失败。原完整
轮次两轮的七比八占比失败保留，未按新分母改判，也未修改门禁。

映像 ThinLTO 后备提交以相同源码树进入主候选 39f8589，完整构建为
41901404153。原生基线、四十七通用补丁、六映像补丁、安全选项及门槛保持。
服务端 5392412 的 41901990586 仅更新准确候选引用，仍先完整准入再组装载荷。
当前完整性能和联合资格待验证；没有发布、账本提升或正式常量、载荷锁提前迁移。
