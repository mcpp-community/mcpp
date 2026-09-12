# 31 —— 编写规则包

**读者:**要把一步构建工作打包给别的工程使用的生态作者 —— 一种设备语言、一个
着色器编译器、一个生成的接口,或者一项检查。

**本章回答的那一个问题:**一个包怎样供给一条规则,以及消费者要写什么才能用上它。

**不在这里:**给单个工程自己的构建加一步,那是
[30 —— 构建程序](30-build-mcpp.md) —— 同一批原语,规模更小;feature 相关键本身,
那是 [06](06-features-and-capabilities.md);以及已发布规则各自的拼法,那属于
`mcpp:plugins`。示例:[`08-build-rules`](../../examples/08-build-rules/) 做检查与
嵌入,[`12-a-new-device-language`](../../examples/12-a-new-device-language/) 新增
一门语言。

## 扩展模型

mcpp 的构建表面由包扩展,而不是由发布扩展。做扩展的有五个点,而经由它们进来的任何
名字,引擎都不持有。

| 扩展点 | 效果 | 声明位置 |
|---|---|---|
| `mcpp::action` | 构建图里的一条边:一条命令,带声明的输入与输出 | 构建程序,或它 import 的规则模块 |
| `device_extensions` | 一个扩展名被引擎归类为**设备源**,而不是被拒绝 | 包的某个 feature |
| `rule_module` | 消费者的构建程序 import 哪个模块来够到这条规则 | 同一个 feature |
| `tools = [...]` | 一个**从源码为构建机构建**的生成器或编译器,用 `mcpp::dep_bin` 取到 | 一条依赖边 |
| `[xlings]`、`[feature-xlings]` | 规则要运行的预建工具,按需安装 | 包本身,或它的某个 feature |

生态用它们建出来的东西:

| 扩展面 | 所在包 | 使用的扩展点 |
|---|---|---|
| CUDA、HIP、SYCL、Ascend C | `mcpp:plugins`,各一个 feature([42](42-heterogeneous-builds.md)) | 驱动厂商编译器的 action,加上按加速器设闸的载荷 |
| Slang | `mcpp:plugins` 的 `rules-slang` | `device_extensions = [".slang"]` —— 第一门无需引擎点名即被支持的语言 |
| GLSL 与 HLSL 到 SPIR-V,以及其上的模块 | `mcpp:plugins` 的 `rules-spirv` | 每个着色器一条 action,加上一个生成的模块 |
| 设备与 C++ 之间的岛边界 | `mcpp.tools.island` | 一个生成器,加上 `mcpp::generated` |
| 作为可链接对象的资源文件 | [`08-build-rules`](../../examples/08-build-rules/) | `role = "object"` |
| 能让构建失败的检查 | [`08-build-rules`](../../examples/08-build-rules/) | `role = "check"` |
| 引擎从未听说过的语言 | [`12-a-new-device-language`](../../examples/12-a-new-device-language/) | `device_extensions`,加上经 `tools = [...]` 构建出来的编译器 |

### 这个模型能表达的形态

**一门新语言,不论由什么编译它。** 规则声明它认领的扩展名、为每个源提交一条
action,并把编译器列进这条 action 的输入。这个编译器可以是厂商工具包、一个 LLVM
前端、一个发出设备二进制的解释器,也可以是规则包自己从源码构建出来的程序。它产出
的是设备二进制、目标文件还是 C++,由 action 的 `role` 决定,别无其他。引擎始终不
学习这门语言:它学到的是「某个扩展名是设备源」以及「某条 action 认领它」。

**预处理与代码生成。** `role = "source"` 的 action 产出 C++,由声明它的包随后编译,
而该包的每一条编译边都等它。输入可以是模板、接口定义、一张表,或另一条 action 的
输出 —— 串联是常规做法,因为 action 之间由文件定序并计入指纹。

**一个一半是 C++、一半是另一种语言的文件。** 归类发生在任何规则运行之前,所以带着
引擎自有扩展名的文件按 C++ 编译,永远到不了规则那里。因此一个携带外来代码块的源要
用规则认领的扩展名,再由规则把它拆开:抽出来的 C++ 走 `role = "source"`,外来的那
一半走规则自己的编译器,两者之间的缝就是
[42 —— 异构构建](42-heterogeneous-builds.md) 里的 `extern "C"` 边界。本仓库今天没有
任何包是这个形态。

### 边界

**声明不能重新归类引擎已经拥有的东西。** 依赖的 `device_extensions` 在内建角色
**之后**才被查询,所以规则包认领不了 `.cpp`、`.cppm`、`.c` 或 `.S`。这些是引擎自己
的词汇,一个包不得把文件从中挪走。认领它们不会被诊断,也不产生任何效果:实测把
`".cpp"` 加进某条规则的 `device_extensions`,消费者的 `main.cpp` 仍按 C++ 编译,
构建成功。

**模块接口的扩展名是工程的轴,不是规则的轴。** 把接口写成 `.ixx` 的工程声明
`[build] module_extensions`([04 —— mcpp.toml 工程文件指南](04-mcpp-toml.md))。没有
任何规则包的键能新增一个,因为模块接口要被扫描 import、要产出 BMI、还要进链接 ——
这是三项引擎行为,而不是一条要跑的命令。

**两张表都不包含的扩展名会被点名拒绝**,所以写错的 `device_extensions` 会立刻显形,
而不是把一个源默默丢掉:

```
error: scanner errors:
  .../orphan.zzz: 'orphan.zzz' is listed in [build] sources, and mcpp has no role
  for the extension '.zzz'.
  Its object would be compiled and then linked by nothing, so this is refused rather
  than built.
```

## 规则包的定义

三部分,没有一部分是 mcpp 特有的:

| 部分 | 内容 |
|---|---|
| 一个包 | 普通的 `mcpp.toml`,有版本与许可证 |
| 一个模块 | 一个 `.cppm`,导出 `options` 与一个提交构建边的函数 |
| 一个 feature | 选中它的开关,也是它自己的依赖挂靠的地方 |

引擎里没有规则名,也没有厂商名。消费者命名这个包、激活一个 feature,构建程序
import 那个模块。

## manifest 的三个键

```toml
[features]
default = []

[features.rules-toy]
sources           = ["src/rules-toy.cppm"]
rule_module       = "example.rules.toy"
device_extensions = [".toy"]
```

| 键 | 对消费者的作用 |
|---|---|
| `sources` | 只有 feature 激活时才编译该模块 |
| `rule_module`(2026.9.7.1+) | 消费者构建程序 import 的模块。它隐含 `host-module = true` |
| `device_extensions`(2026.9.7.1+) | 这些扩展名被归类为**设备源**:不做 import 扫描、不产出 BMI,且没有 action 认领时被拒绝 |

只做生成或检查、不编译某种语言的规则不声明 `device_extensions`;
`08-build-rules` 是这个形状,它的消费者在依赖边上自己写 `host-module = true`。

设备扩展名不在默认 source glob 里。消费者通过点名这些文件来选入:

```toml
[build]
sources = ["src/*.cpp", "src/kernels/*.toy"]
```

## 消费者 import 的那个模块

每条已发布规则遵循的写法是:一个带默认值的 `options` 结构,加一个提交边的
`compile`(或 `generate`)函数:

```cpp
export module example.rules.toy;
import std;
import mcpp;

export namespace example::rules::toy {

struct options {
    std::string out_dir  = std::string(mcpp::out_dir());
    std::string rule_dir = std::string(mcpp::dep_dir("rules-toy"));
};

bool compile(options opt = {});

}
```

把「规划边」与「提交边」拆成两个函数的规则,给了消费者一条越过它最后一个旋钮
而不必手写 action 的路;`08-build-rules` 的 `plan` / `submit` 就是这个形状。

**规则只取自己认领的扩展名,其余留给别人。** `mcpp::device_sources()` 是一个串、
每行一个包根相对路径,而且它是这个包**全部**的设备集合。一个有两个后端的构建把
两边的源都放进同一个列表,而构建程序里的每条规则读到的是同一个值。

## 声明工作:`mcpp::action`

```cpp
mcpp::action a;
a.id          = id.c_str();          // 包内稳定且唯一
a.role        = "source";
a.description = desc.c_str();
a.arg("sh").arg(script.c_str()).arg(input.c_str()).arg(output.c_str());
a.input(input.c_str());
a.input(script.c_str());
a.output(output.c_str());
a.submit();
```

这些字符串必须比 action 活得久。`a.id = ("toy:" + stem).c_str()` 交给它的是一个
指向临时对象的指针,而该临时对象在 `submit()` 之前就已经消失。

### 四种 role

`role` 决定这条边的输出去哪里,以及这条边什么时候跑。

| `role` | 输出 | 次序 |
|---|---|---|
| `source` | 可编译的那些进入编译集合 | 声明它的包的每条编译边都等它 |
| `check` | 一个 stamp 文件 | 与编译并行;`blocking = true` 让编译等它 |
| `object` | 进入**链接**集合 | 链接边消费它们 |
| `artifact` | 一个新文件 | 它的输入是链接产物,所以它在链接之后跑 |

### 声明的输入,以及其中的编译器

声明的输入变化时 action 重跑。**命令调用的那个工具也是输入。** 少了它,改动规则
自己的编译器会让每条边都是干净的,产物保留上一个编译器产生的字节 —— 一次覆盖在
陈旧结果之上的绿色构建。

action 的命令跑在**构建目录**里,不是包根。`mcpp::device_sources()` 答的是包根
相对路径,所以规则要先用 `mcpp::manifest_dir()` 拼成绝对路径再放上命令行。

### 命令自己发现输入时,声明 depfile

一份 include 了别的文件的着色器或 kernel,有规则枚举不出来的输入。涉及的每个
编译器都能产出 depfile ——`glslangValidator --depfile`、`glslc -MD -MF`、
`slangc -depfile`,以及 clang 家族驱动的 `-MD -MF`:

```cpp
a.depfile = dep.c_str();          // 命令写出的一个路径
a.arg("--depfile").arg(dep.c_str());
```

depfile 不得同时被声明为 `output()`。

### 串接 action

一个 action 可以消费另一个的产物。引擎只负责给它们定序与做指纹,而这就是一次
device link 在引擎侧的全部内容:N 个 `artifact` action,其输出不进链接;再加一个
`object` action 读取它们,产出真正进链接的对象。

## 规则自带的环境

规则拥有它所驱动的那份包清单,因为运行编译器、读头文件、把库目录放上链接行的
正是它的代码。

```toml
[target.'cfg(accelerator = "cuda")'.feature-xlings.rules-cuda]
"xim:cuda-nvcc"   = "12.9.86"
"xim:cuda-cudart" = "12.9.79"
```

两道闸,下载一个字节之前两道都要开:feature 说这条规则要不要,
`cfg(accelerator = ...)` 选择器说这次构建要不要为设备编译。不带加速器的构建两道
都不开。

裸版本是项目可以覆盖的**选择**;`>=` 是项目不得低于的**要求**。见
[23 —— 项目环境](23-the-project-environment.md) 的*一个包一个版本*。

## 岛的边界是生成的,而且不由规则生成

`mcpp.tools.island` 写出设备翻译单元包含的 `extern "C"` 头,以及 C++ 侧 import 的
模块。它**不是**规则包,`mcpp:plugins` 里也没有任何东西调用它:调用它的是工程自己的
`build.mcpp`,因为模块名与命名空间的形状是工程的决定而不是规则的。

规则欠它的只有一个字段。生成的头经由 `mcpp::tools::island::force_include_flags`
到达岛,而这些旗标要落在设备编译器自己的命令行上 —— `cuda`、`hip`、`sycl` 与
`ascendc` 的 `options::flags` —— 因为那个驱动不继承 `mcpp::cxxflag` 的任何东西,
也因为把一个头强制灌进每个 C++ 翻译单元会让声明出现在模块接口的 `export module`
之前,那是非良构的。

生成器本身见 [42 —— 异构硬件构建](42-heterogeneous-builds.md) 的*生成这个边界*。

## 报告构建应当知道的事

### `warning` —— 成功了,并且仍然被听见

`mcpp::warning` 是规则「干完了活、但有话要说」的通道:它不得不选择的宿主编译器、
它回落到的载荷。除此之外,构建程序的输出只在非零退出时才被打印。

### `fact` / `floor` —— 探针通道

规则测量,引擎在编译任何东西之前比较。

```cpp
mcpp::fact("cuda.driver", driver_version);
mcpp::floor("cuda.driver", runtime_needs);
```

引擎读到的是一个名字、一个关系和一个版本;名字是穿过它的数据。探不到答案的探针
不陈述任何答案。

## 找到规则自己的文件

| 对象 | 获取方式 |
|---|---|
| 规则包自己的目录树 | `mcpp::dep_dir("<name>")` —— 按**消费者**在 `[dependencies]` 里写的那个名字 |
| `[xlings.workspace]` 声明的载荷 | `mcpp::xpkg_dir("<name>")` |
| 由依赖构建出的宿主工具 | `mcpp::dep_bin("<pkg>", "<tool>")` |

`dep_dir` 按消费者 manifest 里的拼法作答。随模块一起发文件的规则要把该目录作为
一个 option 暴露出来,好让用别的键声明这条边的消费者能提供它;并且在找不到时以
一条点名「找的是什么」的消息拒绝,而不是拿一个空路径去执行命令。

## 编写一个分发成员(mcpp 2026.9.11.1+)

一个 `dist-*` 成员既不是规则也不是工具。它不编译编译单元,也不在构建程序运行期间把活
干完:它消费**链接产物**,产出用户去安装的东西 —— 一个 `.msi`、一个 `.deb`、一个
AppImage、一个签过名的 `.app`。机制是一条 `artifact` action 加上
`mcpp pack --format <name>`,见
[30 —— 产出可分发物](30-build-mcpp.md#产出可分发物pack_format-与-stage_dir20269111)。

本章其余内容原样适用。有六条在这里绑得更紧,每一条都是这个类别会犯、而规则不会犯的
错。

**无条件声明,有条件提交。** `provides_pack_format` 是引擎用来回答「这张图提供哪些
格式」的东西,而回答发生在一次「什么格式都没要」的构建上。一个只在被问到时才声明的
成员,对它的作者照常工作 —— 作者永远传自己那个格式 —— 而对其他所有人,这个集合变成
不可知的。

**同时提供 plan 与 submit。** 可分发物是交到用户手上之前的最后一环,因此它是整个构建
里最可能需要项目自己改一笔的部分:换一个压缩等级、多带一个文件、加第二个签名。
`generate_all(opt)` 恰好等于 `submit(plan_all(opt))`,正是让这样一笔改动不至于变成把
成员重新实现一遍的东西。

**点名输入,不要去 harvest 一个目录。** 一个解析成空的路径是无声的,而一个缺失的具名
输入是错误。实测:一条 WiX action 绑定一个目录并 harvest 它,当那个路径在 Windows 上
解析成空时,产出了一个**有效的、空的、52 KB 的安装包,并且没有任何诊断**。
`${mcpp.target_file:<name>}` 就是这个机制 —— 构建程序既不知道三元组也不知道指纹,而一个
不存在的 target 名会被拒绝,而不是展开成一个空路径。

**在成功路径上,为自己的输出断言一条下界。** 只要成员能判断自己的结果是空的或不合理
的,它就必须经 `mcpp::warning` 说出来,因为成功构建的 stderr 会被丢弃。这与上一段实测到
的是同一个失败,只是被拦在了后一层。

**在工具会被查找的那个位置声明它。** `xpkg_dir` 从 `MCPP_XPKG_*_DIR` 回答,而这些是
mcpp 为**正在被构建的那个包**设置的。一个依赖的声明会把载荷装上,却不会让它对消费者的
构建程序可见,所以运行载荷工具的成员要自己声明它 —— 并且在查找返回空时把这件事说出来,
而不是指向一份读者并不拥有的 manifest。

host module 是这条规则的例外,它的声明在它被编入的**每一个**构建程序里都可见
(`tests/e2e/622`),因为 host module 自己的代码作为那些构建程序各自的一部分运行,
而不是作为一个只经由依赖图才能看到的依赖。这正是一个成员要在自己身上声明自己
的载荷、而拉入这个成员的项目从不重复这条声明的原因。dist 成员的载荷因此也归属
于该成员——当工具服务于某一个 target 时归到 target 轴上,与任何其它按目标区分
的声明一样:

```toml
[target.'cfg(env = "android")'.feature-xlings.dist-apk]
"xim:android-build-tools" = ""
```

**构建不许碰网络,而被包起来的工具可能会碰。** 在 `appimagetool` 1.9.1 上实测:除非用
`--runtime-file` 指定一份本地副本,它每次被调用都会从一个 GitHub release 下载它的
type-2 runtime 存根。包装这类工具的成员必须从自己声明的载荷里把这个文件供上。安装期
是下载合法的时候;构建期不是,而一个会去取东西的构建既不可复现也不能离线用。

**一个 `(name, version)` 只指一份载荷。** 包装签名或打包工具的成员继承了那个工具的兼容
面,所以与被包装的工具同步版本是正当的,而且陈述了一件真事。

## 当前边界

- 规则包自己 `[features] default` 里的规则 feature 不隐含 `host-module`。
  `mcpp:plugins` 声明 `default = []`;规则默认开启的规则包会被拒绝,消息点名
  该模块与需要补的键。
- `mcpp emit xpkg` 把 `manifest = "mcpp.toml"` 写进 `mcpp` 段,而
  `mcpp xpkg parse` 把该键报为未知并以 1 退出。`mcpp-index` 里没有任何描述符使用
  它(218 个里 0 个);自带 `mcpp.toml` 的包整个省略 `mcpp` 字段。见
  [09 —— 按场景选命令](09-commands-by-scenario.md)的*当前边界*。
