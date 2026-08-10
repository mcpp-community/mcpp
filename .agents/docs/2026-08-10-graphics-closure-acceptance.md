# 验收记录:图形栈闭合与分发档位(2026.8.10.2)

> 日期:2026-08-10
> 被测:`feat/graphics-closure-and-distribution-tiers`(PR #408)
> 本机:x86_64-linux-gnu / gcc 16.1.0 / NVIDIA RTX 4080 / 550.144.03 / X11 `:1`
> 设计:`2026-08-10-graphics-closure-and-distribution-tiers-design.md`
>
> **本文只写实测。** 每一条结论都带命令与输出;凡未实测的一律标 **未验**,
> 凡本机环境导致的一律与代码结论分开。

---

## 0. 一句话

**mcpp 那一半全部验到了;GPU 那一半在本机验不了 —— 而"验不了"和"验过是好的"必须分开写,
这正是本轮到处在建的那条判据。**

---

## 1. #405 —— 在真实 imgui 模板上端到端验过

不是构造的 fixture,是 issue 里那个模板本身。它生成的 `main.cpp` 只有:

```cpp
import imgui.core;
import imgui.app;
```

**自己不 `import std`** —— 这正是复现所需的形状。

```console
$ rm -rf ~/.mcpp/build-cache/v1/pkg/mcpplibs/imgui@*      # 强制第一次 MISS
$ mcpp new a --template imgui:window && (cd a && mcpp build)
   Compiling imgui v0.0.6
    Finished dev [unoptimized + debuginfo] in 1.15s        ← 缓存 MISS,一直是好的

$ mcpp new b --template imgui:window && (cd b && mcpp build)
      Cached imgui v0.0.6 (9 units)                        ← 缓存 HIT
    Finished dev [unoptimized + debuginfo] in 0.78s        ← 修复前这里必挂
```

修复前的输出(同一形状,e2e `212` 的 RED 实录):

```
std: error: failed to read compiled module: No such file or directory
std: note: compiled module file is 'gcm.cache/std.gcm'
mcpplibs.cmdline: error: failed to read compiled module: Bad import dependency
```

**判据达成。** 顺带,这条也复核了 issue 里「第一个人好、后面都坏」的形状:
`a` 与 `b` 是同一个 mcpp、同一个 home、同一分钟内建的,与版本无关。

---

## 2. 加载器标签 —— 在真实图形产物上验过

`b` 的产物,原生解析动态段:

```
form= executable   tag= RPATH
```

`resolution.json` 的 `loader_tags`(rule E 的记录,共 13 条):

| 对象 | form | required | actual | status |
|---|---|---|---|---|
| `bin/b` | executable | DT_RPATH | **DT_RPATH** | ok |
| `bin/libX11.so` | shared_library | DT_RUNPATH | DT_RUNPATH | ok |
| `bin/libXcursor.so` … `bin/libxcb.so`(共 12 个) | shared_library | DT_RUNPATH | DT_RUNPATH | ok |

**一分为二的两侧都验到了**:可执行 RPATH、12 个库全部 RUNPATH,零 violation。

产物的 DT_RPATH 内容,四条全部存在(逐条核过):

```
<store>/xim-x-glibc/2.39/lib64
<store>/xim-x-gcc/16.1.0/lib64
<store>/compat-x-glx-runtime/2026.08.08/mcpp_generated/glx_runtime/lib    ← 52 个文件
$ORIGIN
```

> ⚠️ **一次我自己的测量错误,记在这里。** 我一度报告第三条"指向一个不存在的版本"
> ——那是 `find … | head -1` 先返回了同目录下的 `2026.06.03` 造成的。
> `2026.08.08` 存在且完整。**`head -1` 不是判据。**

---

## 3. GPU 那一半:本机 **NOT_EXERCISED**,不是 PASS 也不是 FAIL

程序能起来,窗口创建失败:

```console
$ ./b
imgui.app: window creation failed: GLFW error 65545 (GLX: Failed to find a suitable GLXFBConfig)
```

**宿主 GL 本身是好的** —— 所以这不是"这台机器没有显卡":

```console
$ DISPLAY=:1 glxinfo -B
direct rendering: Yes
OpenGL vendor string:   NVIDIA Corporation
OpenGL renderer string: NVIDIA GeForce RTX 4080/PCIe/SSE2
OpenGL core profile version string: 4.6.0 NVIDIA 550.144.03
```

派发与 vendor 也都在载荷里(`libGLX_nvidia.so.0 → 550.144.03`,与宿主驱动同版本)。

**但这台机器的载荷是被污染的**(见 §5),而 `<subos>/lib` 里一个 GL 都没有、
`.wiring` 记录不存在 —— 也就是说**这个 home 从来没有被接线过**。

> **所以本机的诚实判决是 `NOT_EXERCISED`。**
> mcpp 侧的三条职责(标签对、路径通且存在、不打包不该打包的)全部验到;
> 「桥有没有搭上宿主驱动」需要一台接过线的机器,本轮**未验**。
> 这正是设计 §6.3 写的:那部分是 `xlings doctor` 的事,mcpp 该报 `NOT_EXERCISED`。

`mcpp why runtime` 的实际输出(逐字),把 L3 的缺口直接摆出来:

```
requirements:
  - capability:opengl.glx.driver [run] <- compat.glfw@3.4 (required)
  - capability:opengl.glx.driver [run] <- compat.glx-runtime@2026.08.08 (required)
  …
providers:
  - opengl.glx.driver -> compat.glx-runtime@2026.08.08 [index+compat@2026.08.08]
  - x11.display       -> compat.glx-runtime@2026.08.08 [index+compat@2026.08.08]
artifacts:
  (not declared by the environment — nothing to verify)
  note: a resolved provider with no artifact is UNVERIFIED,
        not verified-good
validation: pass (source post_link)
  - bin/b: pass          ← 13 个对象逐条 pass(rule E + 闭包)
  …
provider and host-service re-diagnostics are owned by xlings; run `xlings doctor`
```

**一个 provider 按名字解析成功、身后一个物都没有** —— 这就是设计里那句
「provider 有名无物,所以没有任何东西可以校验」的现场。
身份判决因此无事可做,而它**说出来了**,没有伪装成 `(none declared)`
那种可以被读成"没问题"的措辞。

---

## 4. 分发档位

| 档 | 判据 | 结果 |
|---|---|---|
| `vendored` | bundle 内**每个** ELF 无构建机 store 路径;可执行 RPATH、库 RUNPATH | ✅ e2e `215` |
| `self-contained` | 有 run 期能力需求时 plan 期硬拒并给出 `vendored` 出路 | ✅ e2e `216` |
| `static` | 同上 | ✅ e2e `216` |
| `self-contained`(无能力需求) | 仍然可打包、`run.sh` 可运行 | ✅ e2e `30` |

`215` 的 RED 实录(撤掉 F2 之后),正是设计里描述的那条:

```
lib/libgcc_s.so.1  shared_library RUNPATH
  <store>/xim-x-glibc/2.39/lib : <store>/xim-x-gcc/16.1.0/lib64
FAIL: bundled object still points at the BUILD MACHINE's store
```

**一个真回归,由 `30_pack_modes` 抓到并已修**:第一版 F2 把 **动态加载器本身**
也 patchelf 了。它不是被搜索的库,它是执行搜索的程序 —— 改它让 `self-contained` 档
在 `main` 之前段错误。修法是把加载器排除在重写之外。

---

## 5. 本机环境的三个缺陷(与本 PR 无关,但解释了本地 e2e 的红)

本地 e2e:**183 通过 / 25 失败 / 8 跳过**。
**25 条里 24 条用已发布的 `2026.8.8.2` 逐条复现** —— 同一个根因,三种表现:

### 5.1 共享 gcc 载荷的 specs 被历史安装污染

```
--dynamic-linker → <store>/xim-x-glibc/2.44/lib64/ld-linux-x86-64.so.2   ← 该目录已被改名为 2.44.aside
rpath           → 约 40 条 /tmp/tmp.XXXXXX/mcpphome/... (全部来自已删除的 e2e 沙箱)
```

后果:每一个 `build.mcpp` helper 的 PT_INTERP 指向不存在的加载器,
`posix_spawnp` 返回 **ENOENT**,报成 `exited with 127`。
这打掉了 112 / 124 / 125 / 179 / 181 / 186–194 全部。

> **这是设计 §3.1「specs 是共享可变状态,不能承载契约」的现场证据**,
> 也正是 `mcpp-clean-link.specs` 存在的原因 —— mcpp 自己的构建因此不受影响,
> 而**独立编译的 `build.mcpp` helper 走不到那条防线**。这条值得单独跟。

### 5.2 `-print-search-dirs` 指向另一个工程的 subos

```
libraries: … /home/speak/workspace/github/openxlings/xim-pkgindex-fromsource/.xlings/subos/default/lib/ …
```

与本工程毫无关系。同一族。

### 5.3 xim binutils 的 shim 指向已删除的会话目录

```
[error] xlings: executable 'as' not found
[error]   path: /tmp/claude-1000/…/accept-run1/home/.mcpp/registry/data/xpkgs/xim-x-binutils/2.42/bin
```

与记忆里 #293 同族。**因此本轮所有 ELF 判据都用原生解析,不 shell out** ——
一个坏掉的 `readelf` 会让标签断言静默空转。

**CI 是这部分的真判据**(干净机器,四平台)—— 结果回填:

```
e2e 1/2 (linux x86_64):  95 passed, 0 failed, 13 skipped
e2e 2/2 (linux x86_64):  97 passed, 0 failed, 11 skipped
```

**192 通过、0 失败**,并且五条新用例逐条确认真的跑了(不是被 `# requires:` 跳过):

```
PASS: 212_cached_dep_std_is_ordered.sh
PASS: 214_executable_carries_dt_rpath.sh
PASS: 216_selfcontained_refuses_host_capability.sh      ← shard 1
PASS: 213_build_after_test_is_not_the_test_graph.sh
PASS: 215_pack_has_no_build_machine_paths.sh            ← shard 2
```

> **这一条特意查了**:`# requires:` 里一个不认识的 token 会让用例**从不运行**
> 而不报错(记忆里 `65_*` 就这样从未在 CI 跑过)。所以不是看总数,
> 是看这五个名字逐个出现在 `PASS:` 行上。

18 项 PR 检查全绿,含 macOS 与 Windows —— 也就是说加载器契约没有扰动非 ELF 平台。

---

## 6. 未验 / 明确不做

| 项 | 状态 |
|---|---|
| 图形程序真正拿到 GPU | **未验**,需要一台接过线的机器 |
| pack 产物在没有 xlings 的机器上运行 | **未验**,需要第二台机器;e2e 只能验到「产物里没有构建机路径」 |
| rule E 转硬门禁 | **不做**,结论只来自一台 NVIDIA/X11/x86_64 机器(与 xlings E5 同一笔欠账) |
| `.wiring` 读取 | **不做**,主判据已改为 mcpp 自算;见设计 §2.1 |

---

## 附:今天重复了三次的同一条

> **要说"验过了",先说清楚验的是哪一片。**

- e2e 25 红里 24 红是环境的 —— 不逐条对照已发布二进制,就会把它们当成回归,
  或者更糟,当成"本来就红"而放过其中真的那一条(`30_pack_modes`)。
- `find | head -1` 让我报了一个不存在的缺陷。
- 宿主 `glxinfo` 好、程序拿不到 context —— 只报前者是撒谎,只报后者也是。
