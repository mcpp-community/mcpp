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
mcpp test               # 执行 tests/unit 与 tests/e2e
```

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
├── manifest.cppm         mcpp.toml 解析
├── lockfile.cppm         mcpp.lock
├── version_req.cppm      SemVer 约束
├── fetcher.cppm          依赖下载(git / index / path)
├── config.cppm           ~/.mcpp/config.toml
├── bmi_cache.cppm        跨项目 BMI 缓存
├── dyndep.cppm           ninja dyndep 生成
├── ui.cppm               进度条与输出格式
├── build/                构建编排与 ninja 后端
├── modgraph/             P1689 模块扫描与依赖图
├── toolchain/            工具链探测、指纹与 std 模块
├── pack/                 mcpp pack 实现
├── publish/              mcpp publish 与 xpkg 生成
└── libs/                 第三方依赖(toml 解析等)

tests/
├── unit/                 各 .cppm 模块的 gtest 单元测试
└── e2e/                  端到端 shell 脚本(run_all.sh 为 CI 入口)
```

## 测试组织

测试分为两层:

- **单元测试** 位于 `tests/unit/test_<module>.cpp`,与 `src/<module>.cppm`
  按模块一一对应。
- **e2e 测试** 位于 `tests/e2e/NN_<feature>.sh`,通过执行真实的 `mcpp`
  二进制覆盖端到端行为;`run_all.sh` 为 CI 调用入口。

修改 `src/` 下任意 `.cppm` 模块时,应同步检查对应单元测试是否覆盖了
变更点;新增功能优先补充 e2e 用例。

执行单个 e2e 脚本:

```bash
cd tests/e2e
MCPP=$(realpath ../../target/x86_64-linux-musl/*/bin/mcpp) ./02_new_build_run.sh
```

## Issue 与 PR 提交规范

### Issue

提交至 [github.com/mcpp-community/mcpp/issues](https://github.com/mcpp-community/mcpp/issues),
建议附带以下信息:

- `mcpp self env` 的完整输出
- 失败命令的完整输出(配合 `MCPP_LOG=debug` 可获得更详细信息)
- 操作系统、发行版、glibc 版本(可通过 `ldd --version` 查看)

### Pull Request

mcpp 处于早期迭代阶段,接口可能调整,提交 PR 前请注意:

1. 涉及 CLI 或 `mcpp.toml` schema 的改动,建议先开 issue 对齐方向。
2. 单个 PR 聚焦单一改动;commit 标题使用英文 imperative 形式
   (`fix: ...` / `feat: ...`)。
3. 提交前确认 `mcpp test` 全部通过。

## 社区资源

- [社区论坛](https://forum.d2learn.org/category/20)
- 交流群 QQ: 1067245099
- [mcpp-index](https://github.com/mcpp-community/mcpp-index) — 默认包索引
- [mcpplibs](https://github.com/mcpplibs) — 配套的模块化 C++ 库集合
