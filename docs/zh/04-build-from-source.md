# 04 — 从源码构建 & 参与贡献

> mcpp 采用自托管模式 —— 通过 mcpp 自身从源码构建 mcpp。
> 任何已具备可运行 mcpp 二进制的环境均可完成源码构建。

## 准备

参照 [00 — 快速开始](00-getting-started.md) 安装一份现成的 mcpp,
然后克隆仓库:

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp
```

## 构建与测试

```bash
mcpp build              # 使用现成 mcpp 编译当前源码 → ./target/.../bin/mcpp
mcpp run -- --version   # 运行刚构建出的产物
mcpp test               # 构建并运行 tests/**/*.cpp 中发现的 C++ 测试(包含 tests/unit)
```

`mcpp test` 不执行 `tests/e2e/` 下的 shell 端到端套件;应单独让它使用刚构建的二进制。

首次构建会自动拉取默认工具链,详见
[03 — 工具链管理](03-toolchains.md)。

如需生成与 release 一致的全静态二进制(对应 `release.yml` 走的路径):

```bash
mcpp build --target x86_64-linux-musl
# → target/x86_64-linux-musl/.../bin/mcpp 为全静态 ELF
```

## 源码结构

```
src/
├── main.cpp              入口
├── cli.cppm              命令分发与参数解析
├── cli/                  命令实现
├── manifest/             manifest 模型、TOML 解析与 xpkg 描述符
├── lockfile.cppm         mcpp.lock
├── version_req.cppm      SemVer 约束
├── fetcher.cppm          fetcher 门面
├── fetcher/              包/索引下载与安装
├── config.cppm           ~/.mcpp/config.toml
├── bmi_cache.cppm        跨项目 BMI 缓存
├── bmi_cache/            缓存存储与失效
├── dyndep.cppm           ninja dyndep 生成
├── ui.cppm               进度条与输出格式
├── build/                构建编排与 ninja 后端
├── fallback/             回退解析路径
├── modgraph/             P1689 模块扫描与依赖图
├── pm/                   依赖解析器与包管理命令
├── platform/             平台与进程抽象
├── scaffold/             `mcpp new` 模板与工程创建
├── toolchain/            工具链探测、指纹与 std 模块
├── pack/                 mcpp pack 实现
├── publish/              mcpp publish 与 xpkg 生成
└── libs/                 第三方依赖(toml 解析等)

tests/
├── unit/                 C++ 单元/集成测试,通常按子系统分组
└── e2e/                  端到端 shell 脚本(run_all.sh 为 CI 入口)
```

## 测试组织

测试分为两层:

- **单元/集成测试** 是 `tests/**/*.cpp` 下由 `mcpp test` 发现的 C++ 文件。它们通常
  按所测子系统或模块命名(例如 `test_pm_lock_io.cpp`、`test_toolchain_triple.cpp`)。
- **E2E 测试** 位于 `tests/e2e/NN_<feature>.sh`,通过执行真实的 `mcpp`
  二进制覆盖端到端行为;`run_all.sh` 为 CI 调用入口。

根据变更的契约选择有针对性的单元和/或 E2E 覆盖。E2E 脚本可能需要 CI 使用的
同一套沙盒、镜像与 capability 配置。

执行单个 e2e 脚本:

```bash
MCPP=<fresh-mcpp-binary> bash tests/e2e/02_new_build_run.sh
```

`<fresh-mcpp-binary>` 必须替换为前一步刚构建二进制的绝对路径；Windows 上该文件为
`mcpp.exe`。

## Issue 与 PR 提交规范

### Issue

提交至 [github.com/mcpp-community/mcpp/issues](https://github.com/mcpp-community/mcpp/issues),
建议附带以下信息:

- `mcpp self env` 的完整输出
- 失败命令的完整输出(配合 `MCPP_LOG_LEVEL=debug` 可获得更详细信息)
- 操作系统、发行版、glibc 版本(可通过 `ldd --version` 查看)

### Pull Request

mcpp 处于早期迭代阶段,接口可能调整,提交 PR 前请注意:

1. 涉及 CLI 或 `mcpp.toml` schema 的改动,建议先开 issue 对齐方向。
2. 单个 PR 聚焦单一改动;commit 标题使用英文 imperative 形式
   (`fix: ...` / `feat: ...`)。
3. 行为改动或测试文档改动在提交前运行 `mcpp test`，并让相关 E2E 脚本使用刚构建的
   二进制通过；纯文档改动复核示例和链接，并用 `gh pr checks <pr-number>` 确认 PR
   实际 required checks。

## 社区资源

- [社区论坛](https://forum.d2learn.org/category/20)
- 交流群 QQ: 1067245099
- [mcpp-index](https://github.com/mcpplibs/mcpp-index) — 默认包索引
- [mcpplibs](https://github.com/mcpplibs) — 配套的模块化 C++ 库集合
