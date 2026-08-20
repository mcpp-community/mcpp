# `mcpp pack` 的 `kind = "shared"` 产物带走了构建机:#460 的实测、根因与优化方案

> 2026-08-20 · issue #460 · 复现于本仓库 HEAD 构建出的 `mcpp 2026.8.19.4`(与报告的 2026.8.18.3 同形)
> 状态:**P0-1 / P0-2 / P0-3 + P2-1 已实施,发布于 2026.8.20.1**;P1-1 / P1-3 已批准未实施

---

## 0. 一句话结论

报告说的现象是对的,但**它给出的期望行为有一半是错的**。

> 期望:打包出的 `.so` 应该是可重定位的——没有 `RUNPATH`(**或为 `$ORIGIN`**)

实测:`$ORIGIN` 修不好这个 bug,空字符串也修不好。因为在 ELF 装载器里,
**DT_RUNPATH 只要*存在*,消费方可执行文件的 DT_RPATH 就不再被继承**——不管
DT_RUNPATH 里写的是什么。库自己的 `libstdc++.so.6` 从此无解。

所以判据不是「路径是可重定位的」,而是「**这条 tag 不存在**」。

| 打包出的 `.so` 上的状态 | 消费方 exe 的 DT_RPATH 是否被继承 | 另一台机器上的结果 |
|---|---|---|
| `DT_RUNPATH = <构建机 store 路径>`(**今天的行为**) | 否 | `libstdc++.so.6: cannot open shared object file`,rc=127 |
| **无 tag**(`patchelf --remove-rpath`) | **是** | **`ok=42`** |
| `DT_RUNPATH = $ORIGIN`(报告建议的另一半) | 否 | 同样 rc=127 |
| `DT_RUNPATH = ""`(`--set-rpath ''`,最容易写出来的「修复」) | 否 | 同样 rc=127 |
| `DT_RPATH = <构建机 store 路径>`(`--force-rpath`) | 是 | `ok=42`(但违反 loader 契约,见 §5.1) |

四行都是在**真实的 `mcpp pack` 产物 + 真实的 mcpp 消费方**上跑出来的,复现命令见附录 A。

---

## 1. 现状:两个 packer,一条契约,只有一个实现了它

`mcpp pack` 有两条互不相干的实现路径,由 `[targets.<n>].kind` 分流
(`src/cli/cmd_publish.cppm::cmd_pack` → `mcpp::pack::route_pack_target`):

| | 应用打包 | **库打包** |
|---|---|---|
| 入口 | `mcpp::pack::build_and_pack` (`src/pack/pipeline.cppm`) | `mcpp::pack::build_and_pack_library` (`src/pack/library_pipeline.cppm:111`) |
| 落地 | `run_pack` (`src/pack/pack.cppm:~954`) | `run_library_pack` (`src/pack/library.cppm:197`) |
| 问「运行时需要什么」 | 是(`ldd` 闭包 + 捆绑) | 否(问的是「消费方要编什么、能链什么」) |
| **ELF 重定位** | **有**(§1.1) | **无**(§1.2) |
| 宿主/格式拒绝 | 有(`_WIN32` 上产 ELF 直接拒绝并给理由) | 无 |

两个文件都写了很长的注释解释自己在小心什么,但**「产物不能带走构建机」这一条只写在
应用侧**。

### 1.1 应用侧已经做对了,而且写了理由

`src/pack/pack.cppm:1028-1096`:

```cpp
auto patchelf = sandbox_patchelf(cfg);
if (!patchelf.empty()) {
    const char* rpath = toBundle.empty() ? "" : "$ORIGIN/../lib";
    set_search_path(bundledBinary, rpath, loader::Form::Executable, patchelf);

    // EVERY BUNDLED LIBRARY, not just the executable.
    // ...
    //   <store>/xim-x-glibc/2.44/lib64 : <store>/xim-x-gcc/16.1.0/lib64
    //   : <store>/compat-x-glx-runtime/…/lib : $ORIGIN
    // Those directories do not exist on the target ...
    for (auto const& dep : toBundle) { ... set_search_path(staged, "$ORIGIN", ...); }
```

注释里那段 store 路径,和 #460 贴出来的 `readelf -d` 输出**是同一段**。也就是说:
这个缺陷的形状在 2026-08-11 就已经被完整地描述过一次,并且在应用侧修好了。

### 1.2 库侧一行 ELF 处理都没有

`src/pack/library.cppm:247-337`,每条 leg 做的事:

```cpp
auto dst = plan.stagingRoot / "lib" / leg.triple / name;
copy_into(leg.artifact, dst);           // 255 —— 只是 copy_file
...
if (leg.shared && !leg.soname.empty()) { create_symlink(name, alias); }  // 287-294
...
.digest = file_digest(dst),             // 333
```

全文 grep 佐证:

```
$ grep -n "patchelf\|set_search_path\|set_rpath" src/pack/library.cppm src/pack/library_pipeline.cppm
(无输出)
```

`.so` 就这样带着链接期的 RUNPATH 原封不动进了 tarball。

### 1.3 那条 RUNPATH 是谁写进去的

不是 bug,是设计——**dev 构建需要它**:

- `src/toolchain/linkmodel.cppm:123-134` —— payload/sysroot 模式下,每个 `libDirs`
  条目发一对 `-L<dir> -Wl,-rpath,<dir>`(glibc payload、gcc payload)。
- `src/build/flags.cppm:624-637` —— SubOS farm 作为 `ldRuntimeFallback` 挂在最后。

于是本机构建出的 `.so` 拿到:

```
DT_RUNPATH = <home>/registry/data/xpkgs/xim-x-glibc/2.44/lib64
           : <home>/registry/data/xpkgs/xim-x-gcc/16.1.0/lib64
           : <home>/registry/subos/default/lib
```

`SharedLibrary` 形态拿 **DT_RUNPATH** 而不是 DT_RPATH,是 `src/build/loader_contract.cppm:49-56`
明确规定的,而且是实测出来的(给库强上 DT_RPATH 会让 `eglInitialize` 挂掉,
openxlings/xlings#593)。**这条契约本身没有问题**,问题在于打包时没人把它取下来。

### 1.4 消费方本来是能自己解决的

`src/pack/manifest_emit.cppm:273-280` 为 shared 包发:

```toml
[runtime]
runtime_search_dirs = ["lib/x86_64-linux-gnu"]
```

这条经 `linkIntent.runtimeSearchDirs` → `src/build/flags.cppm:362` 变成消费方的
`-Wl,-rpath`。实测消费方 exe 拿到的是:

```
0x0f (RPATH)  [<home>/…/xim-x-glibc/2.44/lib64        ← Payload
             : <home>/…/xim-x-gcc/16.1.0/lib64        ← Payload
             : <pkg>/lib/x86_64-linux-gnu             ← Package(来自 runtime_search_dirs)
             : <home>/registry/subos/default/lib]     ← SubosFarm
```

**这正是打包 `.so` 需要的那个闭包,只不过是在「将要运行它的那台机器上」解析的。**
所以正确答案不是给 `.so` 换一个可重定位的路径,而是**把这条 tag 删掉,让它落到
消费方的 DT_RPATH 上**——那份地址是对的、是本机的、而且是 mcpp 自己算的。

> 注意实测里的一个细节:消费方 exe 的 `DT_NEEDED` 里**没有 `libstdc++.so.6`**
> (它自己的代码没有拉到 libstdc++ 符号)。所以进程里通往 libstdc++ 的**唯一**路径
> 就是 `libmathkit.so.1` 自己的解析——而那条路被它自己的 DT_RUNPATH 堵死了。

---

## 2. 实测

全部在本机跑完,mcpp 用 `target/x86_64-linux-gnu/5adc2f74a17360b2/bin/mcpp`(2026.8.19.4)。

### 2.1 复现(与 issue 逐字一致)

fixture 就是 `tests/e2e/251_pack_library_shared.sh` 里的 `mathkit`:

```
$ mcpp pack mathkit-shared
      Packed …/target/dist/mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23.tar.gz

$ readelf -d <pkg>/lib/x86_64-linux-gnu/libmathkit-shared.so
 (NEEDED)   [libstdc++.so.6]
 (NEEDED)   [libm.so.6]
 (NEEDED)   [libgcc_s.so.1]
 (NEEDED)   [libc.so.6]
 (SONAME)   [libmathkit.so.1]
 (RUNPATH)  [/home/speak/.mcpp/registry/data/xpkgs/xim-x-glibc/2.44/lib64:
             /home/speak/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/lib64:
             /home/speak/.mcpp/registry/subos/default/lib]
```

### 2.2 模拟「另一台机器」的四态实验

把包里 `.so` 的 RUNPATH 改写成**同形但不存在**的路径
(`/other-machine/.mcpp/registry/…`),消费方 exe 不重新链接,直接跑:

| 状态 | `.so` 的 tag | rc | 输出 |
|---|---|---|---|
| S1 今天的行为 | `RUNPATH=[/other-machine/…]` | **127** | `error while loading shared libraries: libstdc++.so.6: cannot open shared object file` |
| S2 `--remove-rpath` | 无 | **0** | `ok=42` |
| S3 `--set-rpath '$ORIGIN'` | `RUNPATH=[$ORIGIN]` | **127** | 同 S1 |
| S4 `--set-rpath ''` | `RUNPATH=[]` | **127** | 同 S1 |

S1 的输出**与 #460 报告的错误信息一字不差**。

### 2.3 隔离出机制(合成最小例,不依赖 mcpp)

三个文件:`libdep.so` 在 `deps/`;`libfoo.so` NEEDED `libdep.so`;`app` 的
`DT_RPATH` 含 `deps/`(`--disable-new-dtags`)。

| `libfoo.so` 上的状态 | 结果 |
|---|---|
| `DT_RUNPATH=/nonexistent/…` | `libdep.so: cannot open shared object file`,rc=127 |
| 无 tag | `ok=42` |
| `DT_RUNPATH=$ORIGIN` | rc=127 |
| **`DT_RPATH=/nonexistent/…`**(`--force-rpath`) | **`ok=42`** |

最后一行是关键对照:**同样是失效路径,写成 DT_RPATH 就不阻断继承,写成 DT_RUNPATH 就阻断**。
所以这不是「路径找不到」,是「tag 的种类关掉了整条继承链」。

### 2.4 `LD_LIBRARY_PATH` 能盖住它,但 `mcpp run` 这次没盖住

| 启动方式 | 结果 |
|---|---|
| 裸执行 | rc=127 |
| `LD_LIBRARY_PATH=<gcc payload lib64>` 显式 | `ok=42` |
| `mcpp run` | **rc=127** |

装载器的顺序是 DT_RPATH → `LD_LIBRARY_PATH` → DT_RUNPATH,所以 `LD_LIBRARY_PATH`
**不受** DT_RUNPATH 抑制。但 `compute_run_env` 只把 `plan.runtimeLibraryDirs`
(依赖产物目录)放进去,不含工具链 payload,所以 `mcpp run` 忠实地复现了失败。

⇒ **守卫可以用 `mcpp run`,只要让构建机路径不可达。**(见 §5.3)

---

## 3. 根因

### 3.1 装载器物理

glibc 解析 `libfoo.so` 的 DT_NEEDED 时:

1. 沿 loader 链走 DT_RPATH(`libfoo.so` → 加载它的 exe → …)
2. `LD_LIBRARY_PATH`
3. **`libfoo.so` 自己的** DT_RUNPATH(只有自己的,不继承)
4. `ld.so.cache`
5. 默认目录

而 `elf_get_dynamic_info` 在 **每个对象** 上执行「若有 DT_RUNPATH 则丢弃 DT_RPATH」。
实测(§2.3 最后一行)说明:对携带 DT_RUNPATH 的对象,**第 1 步整条链都不再生效**。

于是:一个 tag 的存在,把消费方精心算好的四段 DT_RPATH 全部作废,只留下构建机上
那三个不存在的目录 + ld.so.cache。ld.so.cache 里没有 mcpp payload 的 libstdc++
(它就不该在),于是 127。

### 3.2 三层遮蔽,所以这个缺陷是外部用户报上来的

**遮蔽一:e2e 251 全程在同一台机器上。**
`tests/e2e/251_pack_library_shared.sh:97-118` 用 `path = "<pkg>"` 依赖 + `mcpp run`,
包和构建机 store 都在原地。RUNPATH 指向的目录**真的存在**,于是永远绿。
文件开头自己写的 "the process starts and the loader resolves the SONAME" 是真的——
只是它解析的是构建机那份。

**遮蔽二:`215_pack_has_no_build_machine_paths.sh` 的清扫到不了库包。**
这个测试的标题就是「打包产物不得含构建机路径」,清扫逻辑(`$MCPP_HOME` 出现在任何
ELF 的 RPATH/RUNPATH 里就失败)**正好能抓住 #460**。但它只跑 `mcpp pack`(应用),
从没跑过 `mcpp pack <lib-target>`。一个覆盖面窄于自身声明的检查,对它看不见的东西
报「clean」。

**遮蔽三:mcpp 自己的运行期闭包模型表达不了这条规则。**
`src/platform/elf_runtime.cppm:325-330`:

```cpp
std::vector<std::filesystem::path> dirs;
for (auto const& raw : requester.runpaths)
    append_unique_path(dirs, expand_origin(raw, requester.artifact));
for (auto const& dir : additionalSearchDirs) append_unique_path(dirs, dir);
for (auto const& dir : binding.libraryDirs) append_unique_path(dirs, dir);
```

它把「requester 自己的搜索路径」和「上层传下来的搜索路径」做**并集**,即把继承链
建模成恒为开、且可叠加。真实规则是:requester 带 DT_RUNPATH ⇒ 后两组**全部不适用**。

`ElfRuntimeFacts` 里**已经有** `searchPathTag`(`elf_runtime.cppm:52`,`loader_contract`
就靠它判违规),`resolve_needed` 只是没读它。所以这不是「信息不够」,是「同一份信息
两处解释,其中一处没跟上」。

这个模块自己的注释写着:

> When the model and the artifact disagree about which loader runs, the model
> wins the report and the artifact wins reality.

——正是这一条,只不过这次分歧不在 loader 上,在 tag 上。

### 3.3 政策模块有一个不存在的调用方

`src/platform/runtime_search.cppm:107-116`:

```cpp
// Is this directory part of THIS machine's private state?
//
// `mcpp pack` asks this to decide what may not be baked into a distributable
// artifact. ...
bool is_machine_local(Origin origin);
```

实际调用方:

```
src/build/prepare.cppm:6696   写 resolution.json 的 machine_local 字段
src/doctor.cppm:742           打印 [machine-local] 标记
tests/unit/test_runtime_search.cpp
```

**没有任何 packer 调用它。** 两个 packer 各自硬编码了自己的答案:应用侧无条件改写成
`$ORIGIN`,库侧什么都不做。注释描述的那个架构(策略在一处、打包器去问)从未接线。

### 3.4 同一类缺陷在 Mach-O 上解决过一次,ELF 侧漏了

`tests/e2e/259_shared_library_macho.sh` 的开头:

> a library built in `/private/var/folders/…/target/…/bin` records that path, and
> the moment it is packed and extracted somewhere else, every consumer of it fails
> at load time — **on the publisher's machine it works perfectly**.
>
> ⚠️ AND WHY IT ASSERTS THE PATH IS *GONE*. Checking that the consumer runs in
> place proves nothing … so the library is packed, **the producer's whole build
> tree is DELETED**, and only then is the consumer built and run.

Mach-O 侧的修法是**链接期**的(`ninja_backend.cppm:247` 无条件发
`-Wl,-install_name,@rpath/<name>`),守卫是**删掉构建树**。

ELF 侧:同样的缺陷类,既没有对应的修法,守卫(251)也恰恰是 259 明确点名「证明不了
任何事」的那种——原地跑。

---

## 4. 影响范围

| 轴 | 结论 | 证据 |
|---|---|---|
| `kind = "shared"` + ELF | **受影响**,产物在另一台机器上不可用 | 实测 §2 |
| `kind = "lib"`(静态) | 不受影响 —— `.a` 是归档,没有动态段 | 格式事实 |
| PE(`.dll`) | 不受影响 —— PE 没有 rpath;DLL 靠 exe 同目录 / PATH 解析 | 读码 |
| Mach-O(`.dylib`) | **需核验**。`install_name` 已是 `@rpath/`(259 已钉),但 `LC_RPATH` 是否会带上 payload 目录取决于该机是否用 payload clang(`linkmodel.cppm:206-209` 会发 `-Wl,-rpath,<libDir>`)。判据:`otool -l <packed dylib> \| grep -A2 LC_RPATH` | 读码 |
| 已发布的二进制库包 | **同样受影响,且不会因为 mcpp 升级而自愈**。注意机制:`mcpp publish` 走的是**源码**发布(`publish/pipeline.cppm:109` 用 `git archive` 打源码 tarball),**不**调 `build_and_pack_library`;二进制库包的发布是作者手动上传 `mcpp pack` 的产物。所以受影响的是**已经躺在 release / 索引里的那些 tarball**,里面就是本节讨论的 `.so` | 读码 |
| 交叉打包 | `run_library_pack` **没有** `pack.cppm` 那样的宿主/格式拒绝,所以 ELF leg 可以从 Windows / macOS 宿主产出(mcpp 支持 Windows 宿主产 Linux ELF,PR#339);而 `sandbox_patchelf` 在那些宿主上根本不存在 | 读码 |
| 应用打包 | **不受影响**,已修(§1.1)。被捆绑的 `.so` 拿 `$ORIGIN`,其未捆绑的系统依赖走 ld.so.cache;`--mode self-contained` 走 wrapper 的 `--library-path`(等价 `LD_LIBRARY_PATH`,不受 DT_RUNPATH 抑制) | 读码 + §2.4 |

---

## 5. 优化方案

按优先级排。P0 三条是一个整体:**不带守卫的修复会以同样的方式再次隐身**。

### P0-1 · 库包必须做 ELF 重定位:**删掉 tag**,而不是改写它

在 `run_library_pack` 的 leg 循环里,`copy_into(leg.artifact, dst)`(`library.cppm:255`)
之后、`file_digest(dst)`(`:333`)之前,插入一步 relocate:

```
对 staging 里的每个 ELF 产物:
  读 PT_DYNAMIC
  若无 DT_RPATH / DT_RUNPATH → 什么都不做(不是错误)
  否则 → 删除这两个 tag 的全部条目
```

**为什么是「删除」而不是「改写成 `$ORIGIN`」**:§2.2 的 S3/S4 已经实测否掉了改写。
更根本的理由在 §1.4:消费方的 DT_RPATH 是同一个闭包在正确的机器上解析的结果,
包括 payload、包目录、SubOS farm 三段。留下任何非空 DT_RUNPATH 都会把它们全部作废。

**顺序上的两个坑(都必须一起改)**:

1. **digest 必须覆盖重定位后的字节。** `file_digest(dst)` 在 `:333`,relocate 必须
   排在它前面,否则 manifest 记录的 digest 与包里的文件不一致。
2. **soname 别名的 copy 回退拷的是原始产物。** `library.cppm:293` 是
   `copy_into(leg.artifact, alias)` —— 拷的是 `leg.artifact` 不是 `dst`。今天两者
   逐字节相同所以看不出来;relocate 落地后,**符号链接创建失败的机器(Windows、
   部分网络文件系统)会拿到一份未重定位的别名**,而 SONAME 别名恰恰是装载器真正
   打开的那个名字。改成从 `dst` 拷。

**判据(四条都要,缺一条这个修复就会假绿)**:

- (a) `readelf -d <包里的 .so>` 里**没有** `RPATH` 也没有 `RUNPATH` 行 —— 判据是 tag
  缺席,不是路径为空(S4);
- (b) SONAME 别名(符号链接或副本)与主文件的 (a) 结论相同;
- (c) 包里任何 ELF 的任何字符串都不含 `$MCPP_HOME`(复用 215 的清扫谓词);
- (d) **消费方在构建机状态不可达时仍能启动**(§5.3 的双向探针)。

### P0-2 · 机制:进程内改写 PT_DYNAMIC,不要调 patchelf

**已写原型并实测通过**(附录 B)。做法:`Elf64_Dyn` 数组按 16 字节槽压缩——删掉目标
槽、后面的整体前移一槽、末尾补 `DT_NULL`。**文件尺寸不变,没有任何偏移需要修**。

```
removed 1 slot(s), 27 slots rewritten in place
size before/after: 17976 / 17976
readelf -h: ok;  readelf -d: RUNPATH 行消失;  消费方: ok=42
```

为什么不用 `sandbox_patchelf`:

- **交叉打包会静默跳过。** 库打包支持 `--target` 列表(fat 包),且没有宿主拒绝。
  从 macOS/Windows 宿主产 ELF leg 时 `sandbox_patchelf()` 返回空——今天应用侧对此
  的处理是 `if (!patchelf.empty())`,即**安静地不做**。在库侧照抄这个形状,等于把
  #460 保留给一半的宿主。
- **多一个外部工具就多一个「装了没装」的轴**,而这一步的正确性是包能不能用的前提。
- mcpp 已经有完整的 ELF 读取器(`mcpp.platform.elf_runtime`),缺的只是一个写侧,
  而这个写侧要做的事**只有删槽**——不需要 patchelf 那些搬 segment 的能力。

**边界(必须在实现里显式处理,不能默默跳过)**:ELF32 / 大端(RISC-V/ARM 32 位交叉
leg 会遇到)、`Both`(同时有 DT_RPATH 和 DT_RUNPATH)、非 ELF 输入(`.a`/`.dll`/`.dylib`
直接返回「不适用」)、只读文件权限。

**patchelf 缺席时怎么办**:不适用了——进程内实现没有缺席这一说。这也顺带删掉了
「库打包依赖 sandbox 里装了 patchelf」这条不成文前提。

> 建议把这个能力放在新模块 `mcpp.pack.relocate`(或 `mcpp.platform.elf_write`),
> 由**两个 packer 共用**,并让它按 `mcpp::platform::search::is_machine_local` 做决策——
> 这样 §3.3 里那句注释就第一次变成真的。应用侧现有的 `set_search_path` 保持不变
> (它要**写入** `$ORIGIN`,是另一个动作),但「哪些条目不许带走」应该由同一个谓词回答。

### P0-3 · 守卫:两侧钉,且必须让构建机状态不可达

新增 e2e(建议 `264_pack_library_is_relocatable.sh`),`# requires: elf python3`:

1. `mcpp pack mathkit-shared`;
2. **静态判据**:包里每个 ELF 都断言 §P0-1 的 (a)(b)(c)——复用 215 的 `read_tag`
   python 片段(建议抽到 `tests/e2e/_read_elf_tag.sh`,两个测试共用一份实现);
3. **动态判据,双向**:
   - 构建消费方 → `mcpp run` → 断言 `ok=42`;
   - **故意打回缺陷**:把包里 `.so` 的 RUNPATH 设成 `/nonexistent/…`,再跑 → 断言
     **失败**且信息里有 `cannot open shared object file`;
   - 还原 → 再跑 → 断言恢复 `ok=42`。

第三步的中间那半是**这个测试存在的理由**:只钉「修好之后能跑」区分不了「守卫生效」
和「根本没有这道门」——修复前后它都会绿(§3.2 遮蔽一)。反向那半只在缺陷真的能被
观测到时才会红。

> 打回缺陷这一步需要一个能写 RUNPATH 的工具。若不想让测试依赖 patchelf,可以让
> `mcpp` 暴露一个只在测试里用的内部子命令,或者直接用 python 就地把 `DT_NULL` 槽
> 改回 `DT_RUNPATH`——反向操作和 P0-2 的正向操作是同一段代码。

另外两处:

- **把 215 的清扫扩到库包**(或在新测试里复用它的谓词)。它的标题声称的范围本来就
  包含库包。
- **251 保留原样**,它钉的是「两个名字都在 + 消费方能起来」,不该被改成移植性测试。

### P1-1 · 让运行期闭包模型能表达这条规则 —— **已批准(2026-08-20)**

`resolve_needed`(`elf_runtime.cppm:310`)按 `requester.searchPathTag` 分支:

```
requester 带 DT_RUNPATH(含 Both,因为装载器按 RUNPATH 处理)
    → dirs = requester.runpaths (+ 非 hermetic 时的 host 默认目录)
否则
    → dirs = requester.runpaths + additionalSearchDirs + binding.libraryDirs (+ host 默认)
```

**收益**:mcpp 能在消费方 `build` 阶段就报出「这个预编译 `.so` 的 DT_RUNPATH 会挡住
你的 RPATH」,而不是等到用户在另一台机器上拿到 127。对**第三方/手写的**
`[[runtime.artifacts]]` 预编译 `.so`(#433 那条线),这是唯一的防线——那些 `.so`
不是 mcpp 产的,P0-1 管不到。

**风险(必须先量再定)**:模型收紧会把今天判 `Pass` 的一些产物变成 `Unresolvable`,
而 `Unresolvable` 是 blocking 的。farm 里那些自带 RUNPATH 的第三方库,其依赖在模型里
将只能从它们自己的 RUNPATH + host 默认目录找——hermetic binding 下没有 ld.so.cache
这一层,可能出现「真实能跑但模型说找不到」。

**建议的落法**:先把新规则作为**诊断**(`Inconclusive` + 明确文案)接进去,跑全量
e2e 与几个真实工程做对照,确认没有新的红,再决定是否升级为 blocking。不要一步到位。

### P1-2 · `is_machine_local` 收敛为唯一策略点

让 P0-2 的 relocate 走 `is_machine_local`,并让应用侧的 `set_search_path` 决策也从
它派生。这样「什么不许带走」只有一处定义;`resolution.json` / `doctor` 报的
`[machine-local]` 与打包器实际剥掉的东西,第一次成为同一个答案。

### P1-3 · 打包 shared 目标时,校验它自己的 DT_NEEDED 闭包(读码,未实测)

`run_library_pack` 只发 `targetName` 这一个产物。如果这个 `shared` 目标链接了同工程的
另一个 `shared` 目标,那个 `.so` 不在包里,`check_prebuilt` 也只校验**声明过的**
artifact 存在(`prebuilt.cppm:82-91`),不看 `.so` 自己的 DT_NEEDED。

删掉 RUNPATH 之后,packer 手里正好有了做这件事所需的一切:staging 里的 `.so` 用
`inspect_elf_runtime` 读一遍,每个 NEEDED 若既不是系统库、又不在包里、又不在
`[dependencies]` 里 → 拒绝或警告(带文件名)。这与 `library.cppm:303` 对「没有
archiver 就不许打包」的既有立场一致:**这类缺陷正是这个 feature 存在的理由**。

### P2-1 · 发布出去的是 debug、未 strip、且含发布者绝对源码路径的产物(实测)—— **已批准:默认 release + strip,并给可配置开关(2026-08-20)。设计见 `2026-08-20-pack-and-consumer-model-review.md` §6.1**

`build_and_pack_library` 里 `BuildOverrides ov` 只设了 `target_triple`,没有 profile。
实测产物:

```
$ file <pkg>/lib/…/libmathkit-shared.so
ELF 64-bit LSB shared object, …, with debug_info, not stripped

$ strings -a … | grep <build dir>
…/b460/mathkit/src/mathkit.cppm
…/b460/mathkit/target/x86_64-linux-gnu/e16a674b43e1ee6f
…/b460/mathkit/src
```

这不是运行期缺陷(DWARF 路径找不到只影响调试),但它是:

- **第二条构建机泄漏**——如果 §P0-1 的判据 (c) 收紧到「全文不含构建机路径」而不是
  「RPATH/RUNPATH 不含」,这一条会直接把测试打红。**所以 (c) 必须明确写成只查动态
  段**,否则守卫会因为一个不同的问题而红,读的人会以为 relocate 没生效。
- 体积与发布质量问题(参考 2026.7.29.1:未 strip 让镜像上传从 34.81MB 降到 4.62MB
  才是真因)。

**这是策略决定,不是 bug。已拍板(2026-08-20):默认 release + strip,并提供可选/可配置
开关。** 具体的键、CLI 形状、与 `dropObjects`/digest 的交互次序,见
`2026-08-20-pack-and-consumer-model-review.md` §6.1。

### P2-2 · `mcpp doctor` 侧的事后检查

对已安装的 mcpp-pack 包(`is_distribution_package`)扫一遍其 `[[runtime.artifacts]]`
指向的 ELF,发现带 DT_RPATH/DT_RUNPATH 就报出来。这能覆盖**已经发布出去的**那批
tarball(§4 最后一行)——它们不会因为 mcpp 升级而自动变好,只能靠重新打包;在那之前
至少要让使用者能看见原因,而不是拿到一句 `cannot open shared object file`。

---

## 6. 明确不建议做的

| 方案 | 为什么不 |
|---|---|
| 把 `.so` 的 RUNPATH 改写成 `$ORIGIN` | **实测无效**(S3)。任何非空 DT_RUNPATH 都会关掉继承链。 |
| `--set-rpath ''` / 任何「只清空字符串」的工具 | **实测无效**(S4)。tag 还在。判据必须是 tag 缺席。 |
| 给库强制 DT_RPATH(`--force-rpath`) | 实测**能跑**(§2.3),但违反 `loader_contract` 的实测结论:库上的 DT_RPATH 有传递性,会把自己的搜索路径压进其下每一次查找,`eglInitialize` 因此挂过(xlings#593)。用一个已知会炸的机制换另一个。 |
| 链接期就不给 `shared` 目标发 RUNPATH | 会同时打断 dev 流(工程内 `.so` 的自解析、`ldd` 可读性)和被第三方宿主 `dlopen` 的场景。**dev 产物和 dist 产物的要求本就不同**——差异应该发生在打包这一步,这也正是应用侧的做法。 |
| 把 libstdc++ 捆进库包 | 与 `dist::Role::SharedLibrary` 的契约直接冲突:一个自带静态 libstdc++ 的 `.so` 导出过 777 个 GLOBAL std 符号,劫持了 exe 自己的 `-static-libstdc++`(#336 / 2026.8.11.3)。 |
| 只改文档、把 `patchelf --remove-rpath` 写成已知规避 | 报告里给的规避**是对的**,但它要求每个使用者都知道这件事;而失败发生在下游用户的机器上,发布者永远看不到。 |

---

## 7. Review 对照表

| # | 主张 | 证据等级 |
|---|---|---|
| 1 | 库打包路径对 ELF 零处理 | **实测 + grep** |
| 2 | 产物 RUNPATH 与 issue 逐字一致 | **实测** |
| 3 | 非空 DT_RUNPATH 关掉消费方 DT_RPATH 的继承 | **实测**(真实产物 + 合成对照,4+4 态) |
| 4 | `$ORIGIN` 与空串都修不好 | **实测**(S3/S4) |
| 5 | 删 tag 后消费方在同一条件下能跑 | **实测**(S2) |
| 6 | 进程内删 `Elf64_Dyn` 槽可行、尺寸不变、产物有效 | **实测**(原型,附录 B) |
| 7 | 251 因为「同机」而永远绿;215 覆盖不到库包 | **读码 + 实测** |
| 8 | `resolve_needed` 把继承链建模成并集 | 读码(`elf_runtime.cppm:325-330`) |
| 9 | `is_machine_local` 没有 packer 调用方 | **grep** |
| 10 | 已发布的二进制库包同样受影响(但 `mcpp publish` 本身发的是源码,不是这条路径) | 读码(`publish/pipeline.cppm:109`) |
| 11 | soname 别名的 copy 回退拷的是未重定位的源 | 读码(`library.cppm:293`) |
| 12 | 库包产物是 debug、未 strip、含发布者源码路径 | **实测** |
| 13 | 打包 shared 目标不校验其 DT_NEEDED 闭包 | 读码,**未实测** |
| 14 | Mach-O 的 `LC_RPATH` 是否泄漏 | **未核验**,需在 macOS 上跑 |

---

## 附录 A · 复现步骤

```bash
# 1. 造 fixture(与 tests/e2e/251 相同)
mkdir -p mathkit/src && cd mathkit
cat > src/mathkit.cppm <<'EOF'
export module mathkit;
export namespace mk { int answer(); }
EOF
cat > src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
EOF
cat > mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit-shared]
kind   = "shared"
soname = "libmathkit.so.1"
EOF

# 2. 打包并看 RUNPATH
mcpp pack mathkit-shared
PKG=$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*')
readelf -d "$PKG/lib/x86_64-linux-gnu/libmathkit-shared.so" | grep -E 'RUNPATH|NEEDED'

# 3. 造消费方(path 依赖),构建
#    app/mcpp.toml 里 [dependencies] mathkit = { path = "<PKG 绝对路径>" }
#    exe 会拿到含 payload + 包目录 + farm 的 DT_RPATH

# 4. 模拟另一台机器:把包里 .so 的 RUNPATH 指向不存在的同形路径
SO="$PKG/lib/x86_64-linux-gnu/libmathkit-shared.so"; cp "$SO" "$SO.orig"
patchelf --set-rpath '/other-machine/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/lib64' "$SO"
./app/target/*/*/bin/app          # → libstdc++.so.6: cannot open shared object file

# 5. 三个对照
patchelf --remove-rpath      "$SO"; ./app/target/*/*/bin/app   # ok=42
cp "$SO.orig" "$SO"; patchelf --set-rpath '$ORIGIN' "$SO"; ./app/target/*/*/bin/app  # 127
cp "$SO.orig" "$SO"; patchelf --set-rpath ''        "$SO"; ./app/target/*/*/bin/app  # 127
```

## 附录 B · 进程内删 `Elf64_Dyn` 槽的原型

```python
# 删除 DT_RPATH(15) / DT_RUNPATH(29):按 16 字节槽压缩,末尾补 DT_NULL。
# 文件尺寸不变,不动任何 segment/section 偏移。
phoff,            = struct.unpack_from('<Q',  d, 0x20)
phentsize, phnum  = struct.unpack_from('<HH', d, 0x36)
for i in range(phnum):                       # 找 PT_DYNAMIC
    off = phoff + i * phentsize
    if struct.unpack_from('<I', d, off)[0] == 2:
        dynoff, = struct.unpack_from('<Q', d, off + 0x08)
        dynsz,  = struct.unpack_from('<Q', d, off + 0x20)

slots, j = [], dynoff                        # 读到 DT_NULL 为止
while j < dynoff + dynsz:
    tag, val = struct.unpack_from('<qQ', d, j)
    slots.append((tag, val))
    if tag == 0: break
    j += 16

kept = [s for s in slots if s[0] not in (15, 29)]
kept += [(0, 0)] * (len(slots) - len(kept))  # DT_NULL 填回,长度不变
for k, (tag, val) in enumerate(kept):
    struct.pack_into('<qQ', d, dynoff + k * 16, tag, val)
```

实测结果:`removed 1 slot(s), 27 slots rewritten in place`;尺寸 17976 → 17976;
`readelf -h` 正常;`readelf -d` 中 RUNPATH 行消失;消费方 `ok=42`。

C++ 实现需补:ELF32 / 大端、`Both` 情形、非 ELF 直接返回不适用、写权限。
