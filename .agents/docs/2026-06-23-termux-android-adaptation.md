# Termux / Android (aarch64) 适配分析报告

**日期**: 2026-06-23
**范围**: mcpp / xlings / tinyhttps / musl-gcc 生态在 Termux(Android 13, aarch64)上的
`curl quick_install.sh | bash` → `xlings install mcpp` → `mcpp build` 全链路适配。
**目标**: 用户无感体验 —— 零手动步骤(无需 `--mirror CN`、无需手动 PATH)。

本报告记录本次会话中在 Termux 上逐层暴露并修复的**全部**问题:网络下载、
工具链解压(hardlink / `ld` / `as`)、连接慢、终端"需要回车"、以及最终的
`set_robust_list` / musl 版本兼容问题。

---

## 0. 环境与症状总览

- 设备: Android 13 (API 33), kernel 5.15.74-android13, aarch64, Termux(bionic libc)。
- 关键约束: Termux 运行的是 **musl-static** 的 xlings/mcpp/工具链二进制(非 bionic)。
- 表现链(修复顺序即暴露顺序):
  1. 所有 HTTPS 资源下载挂(github + gitcode 同时超时)。
  2. 下载通了但 musl-gcc **解压失败**在 `aarch64-linux-musl/bin/ld`。
  3. 解压通了但**连接慢 / 卡几分钟 / 需要回车**。
  4. 工具链装上了但 `aarch64-linux-musl-g++ --version` **SIGSYS**(Bad system call)。

每一层都是独立根因。下面逐个记录。

---

## 1. musl-static 在 Android 上 DNS 解析失败(所有下载挂)

### 症状
`curl quick_install.sh | bash` 装好 xlings 后,任何资源下载失败:`xlings self install`
探测 `https://github.com: timeout` **且** `https://gitcode.com: timeout`(两个都超时),
patchelf 卡 0.0%。而系统 `curl`/`git`、`ping github.com` 都正常。

### 误判(记录以免重蹈)
先后排查 CA bundle、TLS 1.2、熵源,**全错**:
- mbedtls 用 `MBEDTLS_SSL_VERIFY_OPTIONAL` → CA 缺失非致命(排除 CA)。
- gitcode 接受 TLS 1.2(排除 TLS 版本)。
- `/dev/urandom` 可读(排除熵源)。

### 根因
musl-static 二进制在 Android 上 `getaddrinfo()` 解析不了主机名:
- musl 读 `/etc/resolv.conf`,但 Android 的 `/etc` 只读且**无该文件**。
- Termux 把 nameserver 放在 **`$PREFIX/etc/resolv.conf`**(实测 `nameserver 8.8.8.8` / `8.8.4.4`),
  libc 从不看这里 → getaddrinfo 卡死在 `127.0.0.1:53` → 所有下载 "Connection failed"。
- 系统 curl/git 能用是因为它们 **bionic 编译、走 Android DNS**;musl-static 走不了。
- **判据**: github 和 gitcode **同时**超时 = 解析层挂,不是某站点。

### 修复(tinyhttps 0.2.5)
新建 `mcpplibs.tinyhttps:platform` 分区(对齐 xlings/mcpp 的 platform 模块布局):
`getaddrinfo` 失败时读 `$PREFIX/etc/resolv.conf`(候选 `/data/data/com.termux/.../resolv.conf`、
`/etc/resolv.conf`)拿 nameserver,**手写最小 UDP DNS A 查询**拿 IP,再按 IP 连接。
纯 musl-static 自包含,**不兜 curl/git**。调用点用 `if constexpr (platform::is_windows)`,
平台分歧集中在 platform 模块。新增 `test_resolver` 做真实 UDP DNS 查询验证。

- 文件: `mcpplibs/tinyhttps/src/platform.cppm`, `src/socket.cppm`
- 实测生效: `xlings self install` 里 tinyhttps `gitcode.com: 235 ms`,patchelf 装上。

### 经验
- 接口分区(`export module x:platform;`)必须被主模块单元 **`export import`**(GCC 强制,
  否则 "interface partition is not exported")。
- 非模板 `if constexpr` 仍会**编译被弃分支** → Windows 分支引用 `#ifndef _WIN32` 的
  POSIX-only helper 会编译失败 → platform 模块**实现体用 `#ifdef`**,调用点才用 `if constexpr`。

---

## 2. 连接慢 / "connecting…" 卡几十秒

### 症状
patchelf(14KB)装了 85 秒;musl-gcc 下载前 "connecting…" 卡 19s+。

### 根因
1. tinyhttps 的 `connectTimeoutSec = 30`,而手写 DNS 解析器把 **DNS 查询超时也设成了
   connect 超时(30s)**。8.8.8.8 从国内偶尔丢 UDP 包时,单个 nameserver 干等 30s,
   两个最坏 60s —— **每次连接**都付这个代价。DNS 本该 <1s。
2. **无缓存**:一次下载有多次连接(HEAD 探测 + GET + 重定向),每次都从头解析。

### 修复(tinyhttps 0.2.7 → 0.2.8)
- DNS 查询超时**硬上限 2.5s**(与 connect 超时解耦):
  `int dnsTimeout = (timeoutMs > 0 && timeoutMs < 2500) ? timeoutMs : 2500;`
  **注意不能用 `std::min`** —— `<winsock2.h>` 定义 `min` 宏,会 mangle Windows 分支
  (0.2.7 因此挂 Windows 构建,0.2.8 改三元表达式修)。
- `resolve_fallback` 加**进程内缓存**(`std::map<host, ips>` + `std::mutex`),成功解析后复用。

- 文件: `mcpplibs/tinyhttps/src/socket.cppm`, `src/platform.cppm`

---

## 3. `xlings update` 卡 5 分钟(github-only 子索引)

### 症状
`mcpp build` 首次在 `[VERBOSE] fetcher: resolve` 后卡 ~5 分钟无输出。

### 根因
`rm -rf .mcpp` 清掉 mcpp 的索引新鲜标记 → mcpp 调 `ensure_official_package_index_fresh`
→ `xlings update`(quiet,无输出 → 像死 hang)。`xlings update` 用 `git clone --depth 1`
同步索引仓:主索引 `xim-pkgindex` 有 gitee CN 镜像(通),但**子索引
`xim-pkgindex-{awesome,scode,d2x}` 的 CN 也配成 `github.com`**(无真 CN 镜像)。
git **没有连接超时**,连被墙的 github 干等 OS TCP 超时 ~127s/个 × 3 ≈ 5 分钟。

### 修复(xlings 0.4.58)
`src/core/xim/repo.cppm` 的 `sync_repo`:clone/pull 前用
`tinyhttps::probe_latency(url, 3000)`(返回 `inf` = 3s 内不可达)**探主机可达性,
不可达就跳过**。github 子索引在被墙网络上 3s 跳过,不再冻结 5 分钟;主索引走 gitee
CN 镜像正常。本地索引仍可用(非致命跳过)。

---

## 4. "需要回车" + `^[]11;rgb:...` 泄漏

### 症状
`mcpp build` 时终端偶尔卡住、需要按回车才继续,日志里出现 `^[]11;rgb:0000/0000/0000^[\`。

### 根因(**不是 stdout 解析 bug**)
是 xlings 的**终端明暗主题探测**:`platform::query_terminal_is_light()` 往 `/dev/tty`
发 OSC-11 查询(`\e]11;?`),`select` 等 50ms 读回复。当 xlings 在 mcpp 下运行时
(stdout 是**管道**,被 mcpp 的 NDJSON 解析捕获),它**仍然**去戳 `/dev/tty`。Termux 上
终端回复慢(>50ms)→ select 超时放弃 → 回复晚到、落进终端输入缓冲没人读 → 泄漏成
`^[]11;rgb:...` 污染输入行 → 用户得按回车清掉。

数据流分工(供参考,排除"解析 bug"):
```
mcpp → 起子进程 `xlings interface install_packages`(NDJSON,带 "yes":true)
xlings → tinyhttps 真连接/下载/解压,吐 NDJSON 进度
mcpp ui.cppm → 解析 NDJSON → 画 "Downloading … connecting…"
```
"connect 慢"是 xlings 的 tinyhttps 连接(§2),不是 mcpp 解析。mcpp 设计上 install
失败会 `interface → install_direct` **跑两遍**(下载两次);extract 修好不再失败即不再双下载。

### 修复(xlings 0.4.58)
`src/platform/unix.cppm` 的 `query_terminal_is_light()` 开头:
```cpp
// 自己的 stdout/stderr 都不是终端(被管道捕获)→ 不戳 /dev/tty,回退 env/默认主题
if (!::isatty(STDOUT_FILENO) && !::isatty(STDERR_FILENO)) return std::nullopt;
```

---

## 5. musl-gcc 解压失败:strip(体积)+ dereference(hardlink)

### 症状
`mcpp build` 装 musl-gcc 时:
```
[error] extract failed for musl-gcc:
  write_header(.../aarch64-linux-musl/bin/ld): Can't create '.../bin/ld'
```

### 误判 → 真因(两个问题叠加)
原生 `musl-gcc-15.1.0-linux-aarch64` 当初**未 strip** = 828MB 压缩 / 1.86GB 解压
(`cc1plus`418MB + `cc1`381 + `lto1`370 + `lto-dump`388 ≈ 1.5GB 调试信息)。

- **先误判 ENOSPC**:`bin/ld` 在解压 189MB 处,以为 790MB 包占盘 + 解压把手机空间撑爆。
- strip 到 116MB/337MB 后**仍卡同一个 `bin/ld`** → 推翻 ENOSPC。
- **真因**:`aarch64-linux-musl/bin/ld` 是 **hardlink**(→ `ld.bfd`),**Termux/Android 的
  `link()` 在此失败**(libarchive 对 hardlink entry 调 `link()`,失败即报 "Can't create";
  跨两次尝试都卡同一个 hardlink 是共同点)。

### 修复(资产层,无需改索引)
**两件事都要做**:
1. **strip**:用 `aarch64-linux-musl-strip`(交叉 strip,在
   `~/.mcpp/.../xpkgs/xim-x-aarch64-linux-musl-gcc/15.1.0/bin/`,**只需交叉 strip 工具,
   不需 aarch64 构建环境**——当初"deferred 需 aarch64 env"判断错了)。按 `file` 判类型:
   可执行全 strip、`.so` 用 `--strip-unneeded`、**跳过 `.a`/`.o`**(strip 破坏静态库链接,
   如 `libstdc++.a` 45MB 保留)。→ 1.86GB → 344MB。
2. **`tar --hard-dereference`** 重打包:把 hardlink 全展开成真实文件(解压不再调 `link()`)。
   → 最终 **132MB 压缩 / 337MB 解压**。

`qemu-aarch64` 跑 C + C++ 冒烟验证(gcc 是 musl-static,可直接 qemu/binfmt)。重传 xlings-res:
github `gh release upload --clobber`;**gitcode 同名资产不能覆盖**(gtc obs_callback 400),
要先删旧再传。`musl-gcc.lua` **无 sha pin** → 资产透明替换免改索引。

### 经验
- gtc 上传**文件名 = 索引 URL 期望名**:传成 `th-0.2.8.tar.gz` 而非 `tinyhttps-0.2.8.tar.gz`
  → CN 下载 404 `NOT_PATH`(127 字节错误页)。
- aarch64 只有 15.1.0 一个 musl-gcc(13.3/11.5/9.4 是 x86_64-only)。

---

## 6. ⭐ `set_robust_list` / musl 1.2.6 → Android 13 SIGSYS(当前最后阻塞)

### 症状
工具链装上后,mcpp 探测编译器:
```
error: '<...>/aarch64-linux-musl-g++ --version' exited with status 159
```
直接跑:`Bad system call`,`exit=159`(= 128 + 31 = SIGSYS)。`as` / `gcc` 同样 exit=159
→ **整个 musl 工具链**在 Android 13 上都跑不了,非 gcc 驱动特有。

### strace 铁证(真机)
```
set_tid_address(...)            = <tid>
set_robust_list(0x.., 24)
--- SIGSYS {si_signo=SIGSYS, si_code=SYS_SECCOMP,
            si_syscall=__NR_set_robust_list, si_arch=AUDIT_ARCH_AARCH64} ---
+++ killed by SIGSYS +++
```
被拦的是 **`set_robust_list`**(musl 启动时 `__init_tp` 为主线程注册 robust mutex 链表)。
Android 13 app 沙盒的 seccomp 把它 **TRAP 成 SIGSYS**(而非返回 ENOSYS)。

### 关键不对称 + 根因
| 工具链 | musl | set_robust_list | Android 13 |
|---|---|---|---|
| 交叉(构建 xlings/mcpp 用) | **1.2.5** | ❌ 不调 | ✅ 能跑 |
| 原生 musl-gcc 包(Termux 编译用) | **1.2.6** | ✅ 启动就调 | ❌ SIGSYS |

实测:
- 交叉 `aarch64-linux-musl-gcc`(musl 1.2.5)产出的 hello + xlings 二进制,strace **只有
  `set_tid_address`,无 `set_robust_list`** → Android 13 能跑。
- 原生包(musl 1.2.6)的 gcc/g++/as **二进制自身**调 `set_robust_list` → 全挂。
- musl 版本用 `qemu-aarch64 <libc.so>` 打印确认:交叉 = `Version 1.2.5`,原生 = `Version 1.2.6`。

**musl 1.2.6 改成主线程启动即注册 robust list**(`set_robust_list`),触发 Android 13 seccomp。
原生包的 gcc/g++/as 二进制是用 musl 1.2.6 **静态链接**的 → 换 libc 文件无用,**必须重建**。

差异来源:`musl-cross-make.lua` 的 config.mak 模板**没 pin `MUSL_VER`** → 用 musl-cross-make
默认版本;建交叉工具链时默认 1.2.5,后来建原生包时默认涨到 1.2.6。

### 修复(进行中,Plan C)
用 **musl 1.2.5** 重建原生 aarch64 musl-gcc:`config.mak` 加 `MUSL_VER = 1.2.5`。
构建环境三方案:
- A. 真 aarch64 机器(最稳)。
- B. qemu-aarch64 模拟构建(本机可跑,gcc bootstrap 模拟 10+ 小时)。
- **C. canadian-cross(本次采用)**:本机 x86_64 速度,用现成交叉工具链编译 host=aarch64
  的 gcc/g++,二进制链接 musl 1.2.5。

修复后:原生 gcc 二进制不再调 `set_robust_list` → Android 13 能跑 → `mcpp build` 在 Termux
上闭环。同时应给 `musl-cross-make.lua` **显式 pin `MUSL_VER = 1.2.5`**,避免未来默认版本漂移
再次引入 1.2.6(或更高)的不兼容。

### 经验 / 待办
- 任何"在 Android 上运行"的 musl-static 二进制都必须避开 musl ≥1.2.6 的启动期
  `set_robust_list`(Android app seccomp 拦截)。统一全生态 musl = 1.2.5(或打 patch
  去掉该调用)。
- 若将来需 musl 1.2.6+,改为**patch musl** 去掉 `__init_tp` 里的 `set_robust_list` 调用
  (robust mutex 极少用),而非靠 seccomp(无 root 改不了)。

---

## 7. 发版级联(本次产物)

每个修复都要跨仓级联发版(tinyhttps 是 xlings 依赖,xlings 被 mcpp 打包):

| 组件 | 版本 | 关键内容 |
|---|---|---|
| tinyhttps | 0.2.5 | musl DNS 兜底(§1) |
| tinyhttps | 0.2.6 | Windows 构建修复(`if constexpr` 弃分支编译) |
| tinyhttps | 0.2.7 | DNS 超时上限 + 缓存(§2) |
| tinyhttps | 0.2.8 | Windows `min` 宏修复 |
| xlings | 0.4.57 | bundle tinyhttps DNS 修复 |
| xlings | 0.4.58 | tinyhttps 0.2.8 + 跳过不可达索引(§3)+ 跳过管道下 OSC 查询(§4) |
| mcpp | 0.0.59 / 0.0.60 | bundle 对应 xlings;aarch64 release job 两段式修复 |
| musl-gcc aarch64 | 15.1.0 | strip + dereference(§5);**待**:musl 1.2.5 重建(§6) |

发版闭环细节见 memory `release-publish-pipeline` / `termux-musl-dns-resolv-conf` /
`aarch64-musl-static-ninja-closure`。

---

## 8. 一句话总结

Termux/Android 适配踩的是**"musl-static 二进制在 bionic + seccomp 环境下"**的一连串坑:
DNS(读不到 resolv.conf)、hardlink(`link()` 失败)、终端 OSC 查询泄漏、以及最致命的
**musl 1.2.6 `set_robust_list` 被 Android 13 seccomp SIGSYS**。前几个已修并发版;最后一个
正用 canadian-cross 以 musl 1.2.5 重建原生工具链解决。
