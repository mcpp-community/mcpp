# mcpp 模板系统(package-based templates)— 设计 + TODO

> 2026-06-03 · 状态:builtin 模板已实现;**package 模板为设计/TODO(本轮未实现)**
> 关联:agentdocs/2026-06-03-capability-architecture-rfc.md §9

## 目标

`mcpp new` 的模板不应只有内置几种,而应**复用"库模型":一个库同时携带实现 + 示例 +
可实例化模板**。让库作者把"上手骨架"和库一起分发,消费者一条命令拉起。

```
mcpp new myapp                                  # builtin: bin(默认)
mcpp new myapp --template gui                   # builtin: imgui.app 窗口骨架(已实现)
mcpp new myapp --template imgui@0.0.2:window    # package 模板(本设计)
mcpp new myapp --template imgui:window          # 省略版本 = 最新
```

## 两层模型

### 1) builtin 模板(已实现)
- `--template bin|gui`,硬编码在 `src/cli.cppm cmd_new`。
- 用途:无网络/零依赖即可起步;`gui` 给出 imgui.app Tier-0 骨架。
- 这是 fallback,也是 package 模板的"标准库"等价物。

### 2) package 模板(设计 / TODO)
语法:`--template <pkg>[@<ver>]:<templatename>`。

**库侧目录约定**(库仓库里新增 `templates/`):
```
imgui-m/
├── src/                      # 库实现
├── examples/                 # 可运行示例
└── templates/
    └── window/               # 模板名 = 目录名
        ├── template.toml     # 模板元数据(见下)
        ├── mcpp.toml.in      # 带占位符的清单
        └── src/main.cpp.in   # 带占位符的源码
```

**template.toml**:
```toml
[template]
name        = "window"
description = "Minimal imgui.app window app"
# 占位符 → 取值来源
[template.vars]
PROJECT     = "{{name}}"          # mcpp new 的 name
IMGUI_VER   = "{{self.version}}"  # 该模板所属包的版本(自动)
# 生成后提示
post_message = "Edit src/main.cpp, then `mcpp run`."
```

**占位符渲染**:`{{name}}`、`{{self.version}}`、`{{self.name}}` 等;`.in` 后缀文件渲染后去掉 `.in`;非 `.in` 文件原样拷贝。

### 解析与执行流程(core)
1. 解析 `--template` 值:
   - 不含 `:` → builtin(`bin`/`gui`)。
   - 含 `:` → `pkg[@ver]:tmpl`。
2. 经现有 fetcher/index 解析并下载该 `pkg@ver`(复用 `mcpp.pm` / `fetcher.cppm`)。
3. 读取包内 `templates/<tmpl>/template.toml`;若缺失 → 报错并列出该包可用模板(`templates/*/`)。
4. 渲染:对模板目录递归拷贝,`.in` 文件做占位符替换,写入新项目目录。
5. 若模板 mcpp.toml 未声明对该库的依赖,自动注入 `[dependencies] <pkg> = "<ver>"`(让模板默认依赖它所属的库)。
6. 打印 `template.post_message`。

### 代码定位(实现时)
- `src/cli.cppm cmd_new`:解析 `--template`,分流 builtin vs package。
- 新增 `src/scaffold/template.cppm`:模板下载 + 渲染引擎(占位符、`.in` 处理)。
- 复用:`src/fetcher.cppm` / `mcpp.pm.*`(下载包)、`src/manifest.cppm`(注入依赖)。
- index:无需改 schema(模板随源码 tarball 分发,已在 `templates/`)。

### 发现/列举
- `mcpp new --list-templates <pkg>[@ver]`:下载并列出 `templates/*/` 及其 description。
- `mcpp new --template <pkg>:`(空模板名)→ 同上列举提示。

## 为什么这样设计(契合架构不变量)
- I5 复杂度下沉:模板由库作者写一次,消费者一条命令继承。
- I1/I4:`--template gui` builtin 保零配置;package 模板可被 `--list-templates` 解释。
- 与 capability 模型正交:模板只是"起点物料",不改变解析/能力体系。

## TODO(实现顺序)
- [ ] T1 模板字符串解析 `pkg@ver:tmpl`(+ builtin 分流)。
- [ ] T2 `template.cppm` 渲染引擎(`.in` + `{{var}}`)。
- [ ] T3 接 fetcher 下载模板包 + 读取 `templates/<tmpl>/`。
- [ ] T4 自动注入依赖 + post_message。
- [ ] T5 `--list-templates`。
- [ ] T6 imgui-m 仓增 `templates/window/`、`templates/headless/` 作为首批样例。
- [ ] T7 文档 + `mcpp new --help` 更新。

## 现状(本轮已落地)
- builtin `--template bin|gui` 已实现并验证(`mcpp new x --template gui` → imgui.app 窗口骨架 → 直接出窗口)。
- package 模板:本文件为设计与 TODO,留待后续实现。
