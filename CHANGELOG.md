# Changelog

> 本文件追踪 `mcpp-community/mcpp` 公开仓的版本演进。
> 格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [0.0.3] — 2026-05-10

依赖解析体系的三步演进:0.0.2 release tag 之后合入 transitive walker,
这一版补齐 SemVer 合并(Level 2)+ 多版本 mangling 兜底(Level 1)。

### 新增

- ✅ **依赖图传递性遍历** —— 直接依赖的子依赖(以及更深层)自动跟随入解析图,
  消费者不必再在自己的 `mcpp.toml` 里把 grandchild 也写一遍;子依赖的
  `[build].include_dirs` 也会沿链路传播,让中间层在编译时看得到 grandchild
  的头文件。冲突检测同时区分 path / git / version 三类来源,跨来源不允许
  混用。

- ✅ **SemVer 合并解析(Level 2)** —— 同一个包在传递依赖图里被多个消费者
  以不同版本约束声明时,resolver 会把两条原始约束 AND 合并(裸版本号视作
  `=X.Y.Z`),向 index 重新查询,选出同时满足两侧的具体版本。若该版本与
  此前已 pin 的不一致,旧的 manifest 与 `[build].include_dirs` 会被原地
  替换为新版本的内容,孩子依赖也按新 manifest 重新入队。新增 e2e
  `32_semver_merge.sh` 覆盖兼容合并 + 不可调和两条主链路。

- ✅ **多版本 mangling 兜底(Level 1)** —— SemVer 合并失败时(典型如
  `=0.0.1` ⨯ `=0.0.2` 这种无重叠的 pin),resolver 不再硬报错,而是把次要
  版本的源码 stage 到 `target/.mangled/<consumer>/...` 下,通过正则改写
  `(export )?module X;` / `(export )?module X:Y;` / `(export )?import X;`
  把模块名替换成 `<X>__v<M>_<m>_<p>__mcpp` 形式,让两个 BMI 在同一构建图
  里以不同模块名共存(C++23 module attachment 帮我们做 ABI 隔离,无需额外
  namespace mangle)。直接 consumer 的源码也一并 stage + 改写,让它的
  `import` 指向 mangled 副本。MVP 范围:仅处理 dep-as-consumer + 叶子
  secondary 两种情形,主包做 consumer 或 secondary 还有自己的 transitive
  deps 时报清晰错误并建议显式 pin。新增 `src/pm/mangle.cppm`(纯改写
  helper + 11 个单元测试)和 e2e `33_multi_version_mangling.sh`。

### 改进

- 🔧 **构建后端按需为多包做 obj 路径命名空间** —— `plan.cppm` 检测到
  跨包同名源文件(多版本 mangling 后两个 `parse.cppm` 同时存在的常见情形)
  时,自动把 `obj/<file>.o` 改为 `obj/<sanitized-pkg>/<file>.o`,`.ddi`
  扫描产物随之放在 object 同目录下。无碰撞时仍是原始 `obj/<file>.o`
  布局,不影响现有缓存命中。

第二个公开版本。新增 C 语言一等公民支持、xpkg 风格依赖命名空间、包管理子系统骨架重构,以及 lib-root 约定。

### 新增

- ✅ **C 语言源文件支持** — `mcpp.toml` 的 `[build]` 段新增 `cflags`、
  `cxxflags`、`c_standard` 三个字段;ninja 后端探测 `.c` 源文件后自动派
  生兄弟 C 编译器(`g++ → gcc`、`clang++ → clang`、跨编译器前缀如
  `x86_64-linux-musl-gcc` 同样适用),发出独立的 `c_object` 规则。
  按文件扩展名分发:`.cppm → cxx_module`、`.c → c_object`、其它 →
  `cxx_object`;dyndep / 模块扫描自动跳过 `.c`。**实测可直接编译
  mbedtls 3.6.1 全部 108 个 `.c` 源文件**(SHA-256 测试向量与 FIPS
  180-4 一致)。

- ✅ **lib-root 约定** — 库项目(`kind = "lib"` / `shared`)的 primary
  module interface 默认在 `src/<package-tail>.cppm`,且必须
  `export module <full-package-name>;`(无 `:partition` 后缀);可用
  `[lib].path = "src/foo.cppm"` 显式覆盖(cargo `lib.rs` 风格)。
  违规组合(显式 path 但文件缺失 / 文件 export partition / module 名
  不匹配 [package].name)报 error;约定文件缺失只报 warning,给已有
  项目软迁移时间。纯 binary 项目跳过所有检查。

- ✅ **xpkg 风格依赖命名空间** — `mcpp.toml` 现在原生支持三种依赖书写形式:
  - 平铺默认命名空间:`gtest = "1.15.2"` ⇒ `(mcpp, gtest)`,无引号
  - TOML 子表命名空间:`[dependencies.mcpplibs] cmdline = "0.0.2"` ⇒
    `(mcpplibs, cmdline)`,无引号
  - 老式带点字符串(向后兼容):`"mcpplibs.cmdline" = "0.0.2"` 仍能解析
  - CLI 同步:`mcpp add mcpplibs:cmdline@0.0.2` 接受 `<ns>:<name>`
    冒号分隔形式,写出仍是子表写法
  - 解析层在 `DependencySpec` 增加 `namespace_` + `shortName` 结构化
    字段,fetcher / lockfile / cache 等下层逻辑沿用现有完全限定 key。

### 改进

- 🛠 **`src/pm/` 包管理子系统(7 步重构,全部完成)** — 包管理相关代码
  从 `cli.cppm`(3510→2900 行) / `manifest.cppm` / `lockfile.cppm` /
  `fetcher.cppm` / `publish/xpkg_emit.cppm` 中抽出,集中到独立的
  `src/pm/` 目录下,跟 `build/` / `toolchain/` / `pack/` 平级。
  最终 8 个内部模块:
  - `pm/pm.cppm`(子系统门面,re-export 数据类型)
  - `pm/dep_spec.cppm` — `DependencySpec` + `kDefaultNamespace`
  - `pm/index_spec.cppm` — 占位,等索引仓配置实现
  - `pm/lock_io.cppm` — `mcpp.lock` IO
  - `pm/package_fetcher.cppm` — xlings NDJSON 客户端
  - `pm/resolver.cppm` — `resolve_semver` + `is_version_constraint`
  - `pm/commands.cppm` — `cmd_add` / `cmd_remove` / `cmd_update`
  - `pm/publisher.cppm` — `emit_xpkg` + tarball / sha256 / release helpers

  整个重构严格保持**零行为变更**:每一步独立 PR、独立 CI 通过、独立可
  回滚;旧模块名(`mcpp.lockfile` / `mcpp.fetcher` / `mcpp.publish.xpkg_emit`)
  保留薄 shim 透传到新模块,所有调用点零改动。规划与依赖图见
  `.agents/docs/2026-05-08-pm-subsystem-architecture.md` §3-§5。
- 📄 **新增设计文档** `.agents/docs/`:
  - `2026-05-08-package-index-config.md` — 多源包索引仓配置 +
    `mcpp.lock` 索引 commit 锁定 + 两层不可变性
    (L1 publish policy + L2 lock mechanism)
  - `2026-05-08-pm-subsystem-architecture.md` — 包管理子系统目标布局
    与 7 步落地计划

### 修复

- 🐛 path 依赖的 `[package].name` 比对支持 xpkg 标准 `name` + 旧式
  `<ns>.<name>` 复合名两种形式,兼容当前 mcpp-index 描述符尚未迁移的
  状态。
- 🐛 module 扫描器解析 partition import(`import :foo`)时,不再把当前
  TU 自己的 partition 后缀拼进 logical name。
  之前 `export module M:bar;` 里的 `import :foo;` 被解析成 `M:bar:foo`
  (没人 provide,产生 7 条 stale warning);现在正确解析为兄弟分区
  `M:foo`。GCC dyndep 实际能分辨,所以 build 不影响,但 mcpp 自己的
  warning 噪音消失。在 `mcpplibs/tinyhttps` 上验证(7 条 warning →
  0 条)。

### 兼容性

向后兼容。老的 `mcpp.toml` / `mcpp.lock` 不需要任何改动即可在 0.0.2 下
继续工作。带引号的 `"ns.name"` 形式继续被解析,只是新写出的 `mcpp add`
会用无引号的子表形式。

## [0.0.1] — 2026-05-07

mcpp 首个公开发版本。

### 已具备的能力

- ✅ 基础工程命令：`mcpp new` / `build` / `run` / `clean` / `test`
- ✅ C++23 模块（`import std` / `import foo.bar`）一等公民支持
- ✅ 跨项目依赖：[mcpp-index](https://github.com/mcpp-community/mcpp-index)
  远程仓库、git、本地 path 三种来源
- ✅ SemVer 约束：`"foo" = "^0.0.1"` / `"~1.2.0"` / `">=1, <2"`
- ✅ P1689 编译器驱动模块扫描 + ninja `dyndep`
- ✅ 跨项目 BMI 持久缓存
- ✅ 私有 toolchain 沙盒（`mcpp toolchain install / default / list`），
  跟系统 PATH 完全隔离；首次使用自动装 musl-gcc 默认工具链
- ✅ 部分版本号支持（`mcpp toolchain install gcc 15` 自动选最高匹配）
- ✅ `mcpp pack` 三种自包含发布模式：
  - `static` — musl 全静态，单文件可分发
  - `bundle-project`（默认）— 只 bundle 项目第三方 .so
  - `bundle-all` — 全自包含含 ld-linux + libc，附 `run.sh` wrapper
- ✅ `mcpp self {doctor,env,version,explain}` 自诊断
- ✅ 下载 / 安装实时进度（速度、字节数、终端宽度自适应）
- ✅ 项目相对路径显示（`@mcpp/...`、project-relative）

### 发布产物（GitHub Release）

- `mcpp-0.0.1-linux-x86_64.tar.gz` — bundled tarball（mcpp + 内置 xlings）
- `mcpp-linux-x86_64.tar.gz` — `latest` 别名
- `install.sh` — `curl | bash` 装机脚本
- `SHA256SUMS` + 各资产 sha256 sidecar
- 二进制为 musl 全静态 ELF，无 PT_INTERP / RUNPATH 依赖，任意 Linux x86_64
  直接可跑

### 限制

- 仅支持 Linux x86_64（glibc / musl 通用）
- macOS / Windows / aarch64 还在路上
- workspace、`mcpp publish --auto`（自动 PR 到 mcpp-index）等功能未发版

### 反馈

接口、命令、产物形态可能在后续小版本调整。issue / 想法 / 协作意向都欢迎到
[issues](https://github.com/mcpp-community/mcpp/issues) 来。
