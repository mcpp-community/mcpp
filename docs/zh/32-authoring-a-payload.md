# 32 —— 编写一个载荷

**读者:**要把一个工具或一份预编译库打包出来,好让 mcpp 工程声明它、由 mcpp 安装
它的人。

**本章回答的那一个问题:**一个 `xim:` 载荷由什么构成,以及它的描述符必须说清什么,
消费者才能点名它并得到一个能用的程序。

**不在这里:**发布供他人 `import` 的**源码包**,那是
[11 —— 发布一个库](11-publishing-a-library.md);让**宿主**的库能被产物够到,那是
[33 —— 编写运行时适配包](33-authoring-an-adapter.md);以及消费一个载荷,那是
[23 —— 项目环境](23-the-project-environment.md)。

在此之前:[31 —— 编写规则包](31-authoring-a-rule-package.md) —— 规则声明它所驱动的
载荷。在此之后:[33 —— 编写运行时适配包](33-authoring-an-adapter.md)。

## 载荷的定义

一切由 mcpp 安装而不编译的东西:编译器、着色器编译器、设备工具包、模拟器、探针
驱动、预编译的 C 库。工程在 `[xlings.workspace]` 里点名它,或者规则包在
`[feature-xlings.<f>]` 里点名它,mcpp 在构建之前把它供给到位。

一个载荷在 `xim-pkgindex` 里是一个 Lua 文件:一个描述它的 `package` 表,加两个把
它放好并登记的函数。

能用的最短形态:

```lua
package = {
    spec = "2",
    name = "glslang",
    description = "Khronos reference GLSL/ESSL front end and validator",
    licenses = {"BSD-3-Clause", "Apache-2.0", "MIT"},
    type = "package",
    archs = {"x86_64"},

    xpm = {
        linux = {
            ["latest"]  = { ref = "15.1.0" },
            ["15.1.0"]  = {
                url = {
                    GLOBAL = "https://github.com/…/glslang-15.1.0-linux-x86_64.tar.gz",
                    CN     = "https://gitcode.com/…/glslang-15.1.0-linux-x86_64.tar.gz",
                },
                sha256 = "87167c9cb32f258addbedb607639b2c1f484c029ba91542a92f19ead21d65d13",
            },
        },
    },
}

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mv("glslang-15.1.0", dir)
    return true
end

function config()
    xvm.add(package.name)
    return true
end
```

`install()` 把解开的目录树放到 mcpp 会去找的位置;`config()` 登记这个载荷提供什么。
本章其余内容,都是这两个函数在多做一些事。

### 安装钩子收到的环境(mcpp 2026.9.12.2+)

mcpp 安装工程所依赖的包时,该包的 `install()` 在环境中收到本次构建的目标,变量名与规则同构建程序一致
([build.mcpp](30-build-mcpp.md)):每个变量都显式写出,没有取值时为空,因此钩子不会读到从启动 mcpp 的
进程继承来的值。

| 变量 | 依赖安装时的取值 |
|---|---|
| `MCPP_TARGET` | 本次构建所请求的目标三元组;原生构建时为宿主三元组 |
| `MCPP_TARGET_OS`、`MCPP_TARGET_ARCH`、`MCPP_TARGET_ENV` | 该三元组的各段 |
| `MCPP_COMPILER`、`MCPP_CXX_STDLIB` | 空 |

工具链的取值为空,因为工具链在依赖图之后才解析:依赖图中的包可能提供目标侧的层,所以依赖安装时编译器与
标准库都尚未确定,而猜测一个取值的钩子可能构建出错误的变体。在 Windows 上空变量即不存在的变量,
`os.getenv` 对它返回 `nil`。工具链载荷的钩子不会收到这些变量。

钩子可以用目标来拒绝或给出诊断,但不得把某种变体构建进名称未体现该变体的存储目录:存储目录按包名与版本
区分,否则第一个消费者就会替之后所有消费者决定变体。针对某一个 C++ 标准库编译的包应以
`requires = ["mcpp:c++-abi=libstdc++"]` 陈述这一点,该需求在工具链解析之后检查
([22 —— 目标侧](22-target-side.md))。

## 描述符必须做对的四件事

**一个版本,两个 URL。** 每个版本都带 `GLOBAL` 与 `CN` 两个 URL 和一个 `sha256`。
两个镜像服务同一份字节;在任一镜像后面的消费者解析到同一个哈希,而只写一个 URL 的
描述符对半个生态不可用。

**`latest` 是一个引用,不是一个版本。** `["latest"] = { ref = "15.1.0" }`。它是
消费者不点名版本时拿到的东西,移动它是一个刻意的动作 —— 钉在 `15.1.0` 的消费者
不受影响。

**`archs` 与平台表是解析要读的东西。** 只为 `linux` 与 `x86_64` 发布的载荷就这样
声明,于是别的平台上的消费者会被**点名拒绝**,而不是拿到一个跑不起来的东西。

**依赖是带下界的 `xim:` 地址。**

```lua
deps = { "xim:gcc-runtime@>=15", "xim:glibc@>=2.38" },
```

## 让载荷可被够到

只会解包的载荷不可用。三条声明把一个目录变成构建能消费的东西。

**路径上的一个程序。** `xvm.add(package.name)` 登记该载荷的 `bin/`,于是 mcpp 能
按裸名找到这个程序。规则包应当**写程序名,不写路径** —— mcpp 会搜索图中每个被声明
载荷的 `bin/`,然后才是 `PATH`,并且能准确报出它搜过哪些目录。

**消费者要链接或加载的库。**

```lua
exports = {
    runtime = { libdirs = { "lib" } },
},
```

`elfpatch` 从每个依赖里读这一项,写进消费者的 `RPATH` —— 这正是一叠载荷不需要任何人
设 `LD_LIBRARY_PATH` 就能解析的原因。

**头文件,好让这个环境里的编译器能对着它构建。** `sysroot.declare_libs(...)` 与
头文件声明把载荷放进 SubOS 的 sysroot 视图。**是声明而不是复制** —— xlings 会随包
一起移除它们,而一份复制会比它的主人活得更久。

## 档位:需要载荷的命令

```toml
"xim:qemu-arm" = { version = "9.2.4-1", when = "run" }
```

`when` 是选中该载荷的那个 feature 之外的**第二道独立闸门**。feature 说**谁**需要这个
工具;档位说**什么时候**。模拟器是运行时需要、编译时不需要的,所以一个只构建固件、
从不烧录的 CI 任务**一个字节都不下载**。

## 当前边界

- 载荷发布到 `xim-pkgindex` —— 那是一个独立仓库,有它自己的评审;mcpp 里没有任何
  东西发布载荷。
- `latest` 与「表里版本号最大的那个」是两个不同的问题,想要最新已发布版本的消费者
  应当点名 `latest`。
- 载荷自己的 CI 无法核验消费者能否解析到它:那是「对着已发布描述符做沙箱核验」的
  用途,也是唯一核验**已发布字节**而不是工作树的东西。
