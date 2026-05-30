# mcpp 0.0.35: Package-Owned Build Metadata Plan

> 状态: in progress
> 分支: `codex/package-owned-build-flags`
> 目标: 让依赖包在官方 mcpp-index 中携带自己的 C/C++ 编译 flags、include 优先级和配置头，从而让 xlings 不再把第三方 C 库的 configure 结果写在项目根 `mcpp.toml`。

## 背景

xlings 现在可以用 `mcpp 0.0.34` 构建，但工程文件里有大量 C 库宏:

- zlib/lua 的平台配置头。
- zstd 的 `ZSTD_DISABLE_ASM`。
- xz/liblzma 的 `HAVE_*`、encoder/decoder/check 配置。
- libarchive 的压缩后端和平台 API 配置。

这些宏本质上是 zlib、xz、zstd、libarchive 等包自己的 configure 结果。它们放在 xlings 根 manifest 中会导致:

- 任何消费者都要重复维护同一批宏。
- 包索引无法表达完整构建语义。
- 后续把依赖迁移到官方 mcpp-index 后，仍然删不掉根级 `build.cflags`。

## 设计目标

- [x] 依赖包描述中的 `mcpp.cflags` / `mcpp.cxxflags` 能作用到该包自己的 compile units。
- [x] 包级 include dirs 在编译该包源码时优先于全局 include dirs。
- [x] build fingerprint / cache key 包含包级 flags，避免 flags 变化后误用缓存。
- [x] Form B 描述支持 `generated_files`，让官方索引包可以生成少量包私有配置头。
- [x] 本地 `path` 索引读取使用命名空间候选，支持 `pkgs/c/compat.foo.lua` 这类官方索引布局。
- [x] 保持现有项目级 flags 兼容，不破坏已有 manifest。
- [x] 用 xlings 当前项目验证 `mcpp build` 和 `mcpp build --target x86_64-linux-musl`。

## 预计修改点

- `src/manifest.cppm`
  - 保持现有解析入口。
  - 若缺少字段，不改变默认行为。
  - 解析 `mcpp.generated_files = { ["path"] = "content" }`。
- `src/build/plan*.cppm` 或当前 build plan 生成位置
  - 在 compile unit 上记录 origin package 或直接保存 package-owned flags。
- `src/build/flags.cppm`
  - 生成编译命令时合并 per-unit package flags。
  - 合并顺序建议: package local include dirs -> project include dirs -> dependency propagated include dirs -> toolchain/sysroot。
- `src/build/fingerprint*.cppm` 或现有 fingerprint 生成位置
  - 加入 package build metadata hash。
- `tests/*`
  - 增加一个最小包级 cflags 测试: 依赖包源码必须依赖包内宏才能编译通过，主项目不声明该宏。
  - 增加一个最小 Form B `generated_files` 测试: 依赖包源码包含生成头，消费者不声明该头。

## 验证命令

```bash
mcpp test
```

```bash
cd /home/speak/workspace/github/openxlings/xlings
/home/speak/workspace/github/mcpp-community/mcpp/target/x86_64-linux-gnu/*/bin/mcpp build
/home/speak/workspace/github/mcpp-community/mcpp/target/x86_64-linux-gnu/*/bin/mcpp build --target x86_64-linux-musl
```

## Checkpoints

- [x] 文档 checkpoint commit: `9047604`.
- [x] failing test added: `tests/e2e/50_package_owned_build_flags.sh`.
- [x] package-owned flags implementation.
- [x] package-owned generated support files implementation:
  - `tests/unit/test_manifest.cpp` 覆盖 Form B `generated_files` 解析。
  - `tests/e2e/51_package_generated_files.sh` 覆盖生成文件参与依赖包编译。
- [x] local path index namespace lookup:
  - `tests/e2e/52_local_path_namespaced_index.sh` 覆盖 `compat` 命名空间文件名候选。
- [x] xlings local validation:
  - `mcpp build` -> `target/x86_64-linux-gnu/ff952c89919589bb/bin/xlings --version` = `xlings 0.4.45`.
  - `mcpp build --target x86_64-linux-musl` -> `target/x86_64-linux-musl/7e48a312cd4dbb49/bin/xlings` static ELF.
- [x] PR draft 创建: https://github.com/mcpp-community/mcpp/pull/88
- [ ] CI 每 120s 检查一次直到完成。
- [ ] 设计确认后 bump `0.0.35` 并发布。
