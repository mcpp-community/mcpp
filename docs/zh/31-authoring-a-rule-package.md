# 31 —— 编写规则包

**读者:**要把一步构建工作打包给别的工程使用的生态作者 —— 一种设备语言、一个
着色器编译器、一个生成的接口,或者一项检查。

**本章回答的那一个问题:**一个包怎样供给一条规则,以及消费者要写什么才能用上它。

**不在这里:**给单个工程自己的构建加一步,那是
[30 —— 构建程序](30-build-mcpp.md) —— 同一批原语,规模更小;feature 相关键本身,
那是 [05](05-features-and-capabilities.md);以及已发布规则各自的拼法,那属于
`mcpp:plugins`。示例:[`08-build-rules`](../../examples/08-build-rules/) 做检查与
嵌入,[`12-a-new-device-language`](../../examples/12-a-new-device-language/) 新增
一门语言。

## 规则包是什么

三部分,没有一部分是 mcpp 特有的:

| 部分 | 是什么 |
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
[03 —— mcpp.toml](03-mcpp-toml.md) 的*一个包,一个版本*。

## 生成岛的边界

设备翻译单元不能 import 模块,所以它与 C++ 侧之间的边界是一个 `extern "C"` 头。
`mcpp:plugins` 的 `mcpp.tools.island` 从两个实现里读出标了 `MCPP_EXPORT_C` 的
入口点,写出那个头以及它之上的模块,于是每个签名只存在一份。

```cpp
mcpp::tools::island::options opt;
opt.module_name = "myapp.kernels";
opt.out_dir     = std::string(mcpp::out_dir()) + "/island";

const auto entries = mcpp::tools::island::scan(halves, opt);
const auto out     = mcpp::tools::island::emit(*entries, opt);
mcpp::generated(out->interface_file.c_str());
```

可用的有四级,每一级覆盖上一级:

| 级 | 手写的部分 | 消费者写 |
|---|---|---|
| L0 | 只有被标记的入口点 | `import myapp.kernels` —— 岛自己的 C 形状接口 |
| L1 | 生成模块之上的一个接缝模块 | `import myapp.saxpy` —— 项目设计的接口 |
| L2 | 接缝,外加直接传给 `emit` 的入口点列表 | 同上,用于 scan 看不见的入口点 |
| L3 | 头文件与模块都手写 | 同上,签名写了两遍 |

生成的头经由 `mcpp::tools::island::force_include_flags` 到达岛,而这些旗标交给
驱动设备编译器的那条**规则**,不走 `mcpp::cxxflag` —— 把一个头强制灌进每个 C++
翻译单元,会让声明出现在模块接口的 `export module` 之前,那是非良构的。

[`examples/09-heterogeneous/boundary`](../../examples/09-heterogeneous/boundary/)
是 L0,并写明了每一级的代价。

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

| 要找什么 | 怎么找 |
|---|---|
| 规则包自己的目录树 | `mcpp::dep_dir("<name>")` —— 按**消费者**在 `[dependencies]` 里写的那个名字 |
| `[xlings.workspace]` 声明的载荷 | `mcpp::xpkg_dir("<name>")` |
| 由依赖构建出的宿主工具 | `mcpp::dep_bin("<pkg>", "<tool>")` |

`dep_dir` 按消费者 manifest 里的拼法作答。随模块一起发文件的规则要把该目录作为
一个 option 暴露出来,好让用别的键声明这条边的消费者能提供它;并且在找不到时以
一条点名「找的是什么」的消息拒绝,而不是拿一个空路径去执行命令。

## 当前边界

- 规则包自己 `[features] default` 里的规则 feature 不隐含 `host-module`。
  `mcpp:plugins` 声明 `default = []`;规则默认开启的规则包会被拒绝,消息点名
  该模块与需要补的键。
- `mcpp emit xpkg` 把 `manifest = "mcpp.toml"` 写进 `mcpp` 段,而
  `mcpp xpkg parse` 把该键报为未知并以 1 退出。`mcpp-index` 里没有任何描述符使用
  它(218 个里 0 个);自带 `mcpp.toml` 的包整个省略 `mcpp` 字段。见
  [08 —— 按场景选命令](08-commands-by-scenario.md)的*当前边界*。
