# C++20 档位支持 — 实施计划(单 PR,目标 2026.7.31.1)

> 设计:`.agents/docs/2026-07-31-cpp20-standard-support-design.md`(review 已定案)
> 基线:`7af26ba` · 分支:`feat/cpp20-standard`
> 定案回顾:MSVC 取 20(带 `cl ≥ 19.38` 版本阈值)· 不动 scaffold/模板 · 不加"显式声明"位

## 步骤(按依赖顺序,每步都能独立验证)

### S1 解析层:白名单 + 别名 + 文案
- `src/manifest/types.cppm::normalize_cpp_standard`:加 `c++20`/`c++2a`(level 20,gnu=false)、
  `gnu++20`/`gnu++2a`(level 20,gnu=true);错误文案补全新值。
- 验证:`tests/unit/test_manifest.cpp` 新增用例;`mcpp build` 对 `standard="c++19"` 仍报错且文案含 c++20。

### S2 能力层:`hasImportStd` → 加 `importStdMinLevel`
- `src/toolchain/model.cppm`:`int importStdMinLevel = 0;`(0 = 未知,走旧布尔语义)。
- provider 赋值(与 `hasImportStd` 同点,知识留在各自 provider):
  - `gcc.cppm::enrich_toolchain` → 20
  - `clang.cppm::enrich_toolchain` libc++ 分支 → 20;MSVC STL 回退分支 → `msvc_std_min_level(tc)`
  - `msvc.cppm` → `msvc_std_min_level(tc)`,由本地 `toolset_at_least(tc, 19, 38)` 决定 20 / 23
- 验证:单测构造 `Toolchain{MSVC, "19.37"}` → 23,`"19.44"` → 20。

### S3 门:prepare 里从一维变二维
- `src/build/prepare.cppm:3735` 之后追加档位判断 + 可行动文案(设计 §4.2)。
- 本地小函数 `std_level_name(int)`(20/23/26 → "c++20"…),不造第二张表。
- 验证:单测覆盖文案;e2e 177(Windows)覆盖真机。

### S4 顺带缺陷:build.mcpp 的 `-std=` 走方言层
- `src/build/build_program.cppm`:`run_build_program` 的 `cppStandard` 形参改为
  `const manifest::CppStandardConfig&`,内部用 `cppfly::std_flag(hostTc, canonical, level)`。
- 调用点:`prepare.cppm:3479`(依赖)、`prepare.cppm:3606`(根)传 `.cppStandard` 整体。
- 验证:单测/或 e2e 92(build.mcpp import)保持绿;新增 c++fly 工程 + build.mcpp 的回归(单测层足够)。

### S5 单元测试(6 组,设计 §5.1)
`test_manifest.cpp` / `test_toolchain_dialect.cpp` / `test_cppfly.cpp` /
`test_cache_key.cpp` / `test_fingerprint.cpp` / 新增门测试。

### S6 e2e(设计 §5.2)
- `175_cpp20_import_std.sh` — c++20 + import std,构建/运行/`-std=c++20` 断言(POSIX)
- `176_cpp20_std_bmi_isolation.sh` — c++23 与 c++20 交替构建,两套缓存并存
- `177_cpp20_msvc.sh` — Windows:`/std:c++20` + import std 硬断言 + 打印 `cl` 版本

### S7 版本 + 文档
- `src/toolchain/fingerprint.cppm::MCPP_VERSION` → `2026.7.31.1`
- bootstrap pin(`.xlings.json` / `MCPP_PIN`)**不动**(自举起点,不随发布走)
- `scripts/check_version_pins.sh` 通过
- 文档已在设计阶段更新(EN/ZH),与本 PR 同批合入

### S8 本地验证
`mcpp build -p mcpp` → 单测全绿 → `tests/e2e/run_all.sh`(至少 175/176 + 回归面)

### S9 PR → CI → 合入
单 PR,bypass squash 合入 main。

### S10 发布闭环
release(四平台)→ gitcode 资源本地 `gtc` 补传 + GET 核验 → xim-pkgindex PR →
`xlings install mcpp` 真装验证 → bootstrap pin bump。

## 不做(设计已定案)
scaffold/模板改动、`mcpp new --standard`、依赖 floor 检查、"显式声明"位、C++17。
