# mcpp 模板系统(package-based templates)— 设计 v2

> 2026-06-04 · 状态:**设计定稿(v2,多级解析)**;builtin `bin|gui` 已实现,package 模板待实现(T1–T8)
> 关联:agentdocs/2026-06-03-capability-architecture-rfc.md §9
>      .agents/docs/2026-06-04-manifest-schema-ownership.md(语法封闭/词汇开放——本设计是该原则在"脚手架"维度的应用)

## 0. 目标与现状问题

`mcpp new` 的模板应**复用库模型:一个库同时携带实现 + 示例 + 可实例化模板**,
让库作者把"上手骨架"随库分发,消费者一条命令拉起。

现状(v0.0.48)的 builtin `--template gui` 是**反例教材**:imgui 的包名、版本、
示例代码硬编码在 `cmd_new` 里 —— 领域词汇泄漏进机制层,且版本已经腐化
(builtin 钉 `0.0.2`,生态已到 `0.0.5`,docking/viewports 模板全用不上)。
**库每发一版,mcpp 核心就过期一次** —— 这正是要根治的。

## 1. 设计原则

> 机制归 mcpp(解析文法、下载、渲染、注入);词汇归库(模板名、内容、默认选择)。
> —— 语法封闭 / 词汇开放在脚手架维度的投影。

- **架构通用**:mcpp 不认识任何具体库;任何 index 包放个 `templates/` 即获得模板能力。
- **使用方便**:常用路径一个词(`--template imgui`),精确路径全可表达(`pkg@ver:tmpl`)。
- **版本对齐**:模板随包 tarball 分发 ⇒ 模板版本 == 包版本,`{{self.version}}`
  注入 ⇒ **版本腐化在结构上不可能发生**。
- **纯数据信任边界**:模板 = 渲染 + 拷贝,**无钩子、无脚本执行**(信任面与普通包源码一致,
  不引入 install-script 攻击面)。

## 2. 解析文法(多级,核心 UX)

```
SPEC := NAME                    # L0 裸名(最方便)
      | PKG ':' TMPL            # L1 指定库的某个模板
      | PKG '@' VER             # L2 库默认模板 + 钉版本
      | PKG '@' VER ':' TMPL    # L3 全显式(最精确)
```

| 写法 | 语义 |
|---|---|
| `--template imgui` | imgui **默认模板**,版本 = 解析到的最新 |
| `--template imgui:docking` | imgui 的 `docking` 模板,最新 |
| `--template imgui@0.0.5` | 默认模板,钉 0.0.5 |
| `--template imgui@0.0.5:docking` | 全显式 |
| `--template imgui:`(空模板名) | 列出该库全部模板(= `--list-templates imgui`) |

**裸名解析顺序**(L0,类似 PATH 查找):
1. **builtin 注册表**(离线标准库,词表冻结:`bin`、`lib` —— 真正零网络可用的两种);
2. miss → 当作**包名**,经 index 解析,取其**默认模板**。

冲突治理:builtin 词表冻结(不再新增,杜绝未来遮蔽包名);`gui` 保留为
**过渡 alias**(打印 deprecation 提示指向 `--template imgui`,一个 minor 周期后移除)
—— 这是对现存领域泄漏的退场路径,不是新模式。

## 3. 库侧约定(词汇层,全部归库)

```
imgui-m/
├── src/                          # 库实现
├── examples/                     # 可运行示例
└── templates/
    ├── window/                   # 模板名 = 目录名
    │   ├── template.toml         # 元数据(下)
    │   ├── mcpp.toml.in          # .in = 渲染;非 .in = 原样拷贝
    │   └── src/main.cpp.in
    └── docking/
        ├── template.toml
        ├── mcpp.toml.in
        └── src/main.cpp.in
```

**template.toml**(模板元数据):

```toml
[template]
description = "Minimal imgui.app window"
default     = true            # 库的默认模板(0 或 1 个;多于 1 个 → 解析报错)
post_message = "cd 进项目后 `mcpp run` 即出窗口。"

# 依赖注入:生成的项目自动获得对所属库的依赖(含 features 形态)。
# `self` = 模板所属包,版本自动 = 解析到的包版本 —— 根治版本漂移。
[template.inject]
self = { features = [] }                      # window 模板:无 feature
# docking 模板则是:self = { features = ["docking-full"] }
```

**占位符集**(机制层固定,渲染引擎唯一识别的词表):

| 占位符 | 取值 |
|---|---|
| `{{project.name}}` | `mcpp new <name>` 的 name |
| `{{self.name}}` | 模板所属包名(如 `imgui`) |
| `{{self.version}}` | 解析到的包版本(如 `0.0.5`) |

(刻意最小;模板内容的可变性靠库出多个模板,而非把渲染引擎做成编程语言。)

## 4. 解析与执行流程(机制层,归 mcpp)

1. `cmd_new` 解析 SPEC(§2 文法 + 裸名两级回退);
2. builtin 命中 → 原有路径(离线);
3. 包路径:经现有 fetcher/index 解析下载 `pkg@ver`(**复用包缓存**,与依赖同一套);
4. 读 `templates/`:
   - 指定 TMPL → 取该目录;缺失 → 报错并列出可用模板;
   - 未指定 → 取 `default = true` 的模板;没有 default → 报错并列出;
5. 渲染:递归拷贝,`.in` 文件做占位符替换(去掉 `.in` 后缀),其余原样;
6. 依赖注入:按 `[template.inject]` 把 `self`(以及未来允许的兄弟依赖)写入生成的
   `mcpp.toml`(若模板清单已显式声明则模板优先,不重复注入);
7. 打印 `post_message`。

**发现**:`mcpp new --list-templates <pkg>[@ver]` → 下载(缓存命中则免)并列出
`templates/*/`:名字、description、是否 default。

### 代码定位
- `src/cli.cppm cmd_new`:SPEC 解析 + 分流 + gui deprecation;
- 新增 `src/scaffold/template.cppm`:渲染引擎(`.in` + 占位符)+ template.toml 解析 + 注入;
- 复用 `src/fetcher.cppm`(下载/缓存)、`src/manifest.cppm`(注入写回);
- index:**无 schema 改动**(模板随源码 tarball 分发)。

## 5. 与既有机制的协同

- **features**:`[template.inject] self.features` 把模板与 feature 体系打通
  (docking 模板 ⇒ 注入 `imgui = { version = "<self>", features = ["docking-full"] }`);
- **why/doctor**:生成的项目是普通项目,解析全程可解释(I4),模板不引入任何特殊态;
- **schema 所有权**:占位符词表、文法、注入键归 mcpp(机制);模板名/内容/默认选择/
  注入的 feature 词汇归库 —— 通过 §1 判定法审视,无泄漏。

## 6. 实施计划(每步:本地验证 → PR → 三平台 CI 绿 → 合入)

- [ ] **T1** SPEC 解析(四级文法 + 裸名 builtin→package-default 回退)+ `gui` deprecation 提示
- [ ] **T2** `src/scaffold/template.cppm` 渲染引擎(`.in`、占位符、template.toml)
- [ ] **T3** fetcher 集成:下载/缓存 `pkg@ver` → 读 `templates/`、default 选择、错误列举
- [ ] **T4** `[template.inject]` 依赖注入(self + features)+ post_message
- [ ] **T5** `--list-templates <pkg>[@ver]`(+ `pkg:` 空名列举)
- [ ] **T6** imgui-m:`templates/window/`(default)+ `templates/docking/`(inject docking-full)
       → 发版(模板首次随包分发)+ index 收录
- [ ] **T7** 文档(00-getting-started、mcpp new --help、imgui-m README)+ e2e
       `69_package_templates.sh`(L0–L3 四级 + 注入 + default + 列举 + 错误路径)
- [ ] **T8** 闭环:fresh 机器 `mcpp new app --template imgui && cd app && mcpp run` 出窗口;
       `--template imgui:docking` 出四分屏 + 分离窗口;一个 minor 后移除 builtin `gui`

## 7. 验收标准

- [ ] `--template imgui` 一词出窗口(默认模板,版本自动对齐最新)
- [ ] `--template imgui@0.0.5:docking` 全显式可复现
- [ ] 注入的依赖带正确版本与 features(版本漂移结构性消除)
- [ ] `--list-templates imgui` 列出 window(default)/docking 及描述
- [ ] builtin `bin|lib` 离线可用;`gui` 出 deprecation 提示
- [ ] 模板路径零脚本执行(纯渲染拷贝)
- [ ] e2e 69 全过,三平台 CI 绿
