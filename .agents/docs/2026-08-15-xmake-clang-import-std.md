# xmake + clang 的 `import std`:错误消息把人指向了死路(2026-08-15)

**结论先说**:xmake 那句 `maybe try to add --sdk=<PATH/TO/LLVM>` 在这个场景下是
**死路**。真正的开关是 **runtime**,而 `--sdk` 只在 runtime 已经把库判定成 libc++
之后才会被读到。为此我试了三轮 `--sdk`,全部落空。

---

## 1. 现象

```
warning: std and std.compat modules not found! maybe try to add --sdk=<PATH/TO/LLVM> or install libc++
error: <mcpp> missing std dependency for module mcpp.build.flags
```

在 Linux + 载荷 clang(`xim-x-llvm/22.1.8`)+ 真实工程上稳定复现;同一个 clang 在
生成的 fixture 上没有问题,同一个工程用 gcc 也没有问题。

## 2. 真因(读 xmake 源码得到,不是推测)

`rules/c++/modules/support.lua`:

```lua
function get_cpplibrary_name(target)
    if target:has_runtime("c++_shared", "c++_static") then return "c++"      -- libc++
    elseif target:has_runtime("stdc++_shared", ...)   then return "stdc++"   -- libstdc++
    ...
    -- 没有指定 runtime 时,按平台回落
    elseif target:is_plat("linux", ...) then return "stdc++"
end
```

`rules/c++/modules/clang/support.lua` 再按这个值分支:

```lua
if cpplib == "c++" then
    -- 找 libc++.modules.json;找不到才提示 --sdk
elseif cpplib == "stdc++" then
    -- 委托给 gcc 的实现
end
wprint("std and std.compat modules not found! maybe try to add --sdk=...")
```

我们的工具链**从未声明 runtime**,所以 Linux 上回落成 `stdc++` —— xmake 跑去
**LLVM 载荷里**找 GCC 的 modules.json,当然没有,于是走到函数末尾那句通用警告。

**`--sdk` 只在 `c++` 分支里被读到。** 我们从来没进过那条分支,所以传它没有任何
效果 —— 而消息本身就是这么建议的。

⚠️ **这是"错误消息指向了一个它自己没走到的分支"**:那句话是函数**末尾**的兜底
warning,不属于任何一个分支,却引用了只有其中一个分支才用得到的选项。

## 3. 有效的组合(实测)

| 尝试 | 结果 |
|---|---|
| `--sdk=<llvm root>` | 无变化 |
| 内建 `llvm` 工具链 + `--sdk` | 无变化 |
| target 上 `set_runtimes("c++_static")` | 无变化(自定义 standalone 工具链拿不到这套接线) |
| 配置层 `--runtimes=c++_static` + 自定义工具链 | 无变化 |
| **内建 `llvm` 工具链 + `--sdk` + `--runtimes=c++_static`** | **警告消失,开始编译模块 BMI** |

载荷里 `lib/<triple>/libc++.modules.json` **一直存在** —— 正是 `c++` 分支要找的
那个文件。

已落到 `bench/src/engines/xmake.cppm`:载荷 clang 走内建 `llvm` 工具链并同时传
`--sdk` 与 `--runtimes`。

## 4. 推进之后撞到的下一个问题(真上游缺陷)

```
.../include/c++/v1/__format/format_functions.h:99:30:
    error: call to implicitly-deleted default constructor of
           'formatter<basic_string<char>, wchar_t>'
    ...
    note: in instantiation of 'std::basic_format_string<wchar_t, ...>'
```

**这与 mcpp 自己修过的是同一个缺陷**,见
`.agents/docs/.../clang-precompile-emits-full-bmi`:clang 的 `--precompile` 发的是
**full BMI**,把它发布给下游会让 clang 22 编错一个下游 TU —— 窄格式串报
`formatter<..., wchar_t>`,报错点在 std 头文件里,离真因很远。mcpp 的解法是
`-Xclang -emit-reduced-module-interface`。

xmake 的 clang 模块实现目前发布的是 full BMI,所以真实工程上会撞到同一个坑。
**这一条是上游的**,但现在被精确定性了,而不是"xmake 不行"。

## 5. 给下一个人的判据

* **不要按错误消息的建议行动,先确认那条建议属于哪个分支。** 这次的建议在兜底
  warning 里,而兜底 warning 按定义是"所有分支都没走成"。
* **xmake 选 std 模块看的是 C++ 库,而库是从 runtime 推的。** 用 clang + libc++
  时,`--runtimes=c++_static`(或 `c++_shared`)是必需的,不是可选优化。
* **自定义 `standalone` 工具链拿不到 runtime 的接线。** 需要 runtime 语义时用内建
  工具链(`llvm`),把载荷通过 `--sdk` 指进去。
