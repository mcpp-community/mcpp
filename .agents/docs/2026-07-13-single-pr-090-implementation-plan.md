# 0.0.90 单 PR 实施计划(post-089 路线图全量)

> 2026-07-13 · 执行 `2026-07-13-post-089-roadmap-and-std-dialect-flags-design.md` 的
> **全部内容**于一个 PR。步骤是顺序实施的里程碑(每步本地 build+test 过),不拆 PR。

## 步骤

**S1 — D1 方言旗标(#210)**
manifest 解析 `[build] dialect_cxxflags`;`extract_dialect_flags(cxxflags)`
(known-list:reflection/contracts/char8_t/`-D_GLIBCXX_USE_CXX11_ABI=`;
fno-exceptions/rtti 首版不入);`BuildPlan.dialectFlags`;注入:flags.cppm 全局
`$cxxflags`(cxx_std_flag 旁)、`ensure_built` 新形参 → gcc/clang/msvc std 命令、
metadata 自然失效。单测 + e2e `98_reflection_import_std.sh`(两变体,flag 探测 SKIP)。

**S2 — A 债务**
stdmod `shellEsc` → `shq`;std/scan 命令的 `env …` 前缀 → argv+env(执行层带
tc.envOverrides + runtime dirs);build_program/p1689 的 is_clang → caps/dialect。

**S3 — C1 环境模型**
msvc.cppm:`find_windows_sdk()`(注册表→Windows Kits 目录扫描,取最高版本)+
`build_env(inst, sdk)` → INCLUDE/LIB/PATH(+`VSLANG=1033`);`enrich_toolchain_from_cl`
填 `tc.envOverrides`;SDK 缺失=检测时警告、构建时硬错误。doctor msvc 段加 SDK 行。

**S4 — 对象/标准旗标方言化(C2 前置)**
`object_filename_for` 接 objExt(.o/.m.o ↔ .obj/.m.obj);ninja std staging
`obj/std.o` 名参数化;`cppStandardFlag` 由 dialect.stdPrefix 产出
(msvc:>c++20 → `/std:c++latest`,20→`/std:c++20`);defines/`/D` 走 dialect;
CRT:`/MD` 默认、linkage=static → `/MT`。

**S5 — C2/C5 msvc 规则发射**
ninja_backend:LinkStyle::SeparateLinker 时 `cxx_link` = `$ld /nologo /OUT:$out @rsp`
(rspfile/rspfile_content),`cxx_shared` = `/DLL /IMPLIB:`,`cxx_archive` =
dial.archiveCmd(lib.exe @rsp);compile 规则 `deps = msvc` + `/showIncludes`;
CompileFlags 增 `ldBinary`(link.exe,cl 同目录);registry archive_tool msvc →
lib.exe;flags.cppm msvc ld 分支(dep runtime dirs → /LIBPATH,user ldflags 透传)。

**S6 — C3 std/std.compat staging**
msvc.cppm `std_module_build_commands`(cl /c std.ixx /ifcOutput /Fo:)+
std.compat.ixx 同法;`std_bmi_path`/`staged_std_bmi_path`/compat 变体 registry 分发
(flags.cppm 的 clang 硬编码 staging 调用一并解除);stdmod:objectPath 扩展名
per-dialect、msvc 分支走新命令、执行带 envOverrides;`hasImportStd` 已由 0.0.88 检测。

**S7 — C4 .ifc 管线 + /scanDependencies**
ninja `cxx_module` msvc 形态(moduleOutputPrefix=` /ifcOutput ` 已就绪;确认
`/Fo:` 同行共存);`/ifcSearchDir`(bmiSearchPrefix,已就绪);cxx_scan msvc 变体
`cl /scanDependencies $out /std:… /c $in`;provider msvc `has_builtin_p1689_scan=true`;
dyndep DyndepOptions(ifc.cache/.ifc)零改动。

**S8 — C7 删门 + fast-path env**
删 prepare.cppm MSVC 构建门;BuildResult/fast-path 缓存的单对 runtime env 推广为
env 列表(持久化 tc.envOverrides,否则 msvc 增量构建丢 INCLUDE/LIB)。

**S9 — MinGW 打磨 + 基建**
lifecycle:mingw 在非 Windows 明确报 windows-only;e2e 97 objdump 路径 glob;
doctor mingw 段;release.yml publish-ecosystem timeout 20→30。

**S10 — 测试与 CI 面**
e2e `99_msvc_native_build.sh`(requires: msvc):default msvc → new → build → run →
多模块 → import std → 增量;**95 号改造**:门断言(“not yet supported”)翻转为构建
成功断言;ci-windows.yml MSVC 步骤同步改为 build+run 矩阵(llvm+mingw+msvc);
单测:dialect 提取器、SDK 路径解析纯函数、std: 映射、msvc std 命令拼装。

**S11 — 版本与文档**
0.0.90(mcpp.toml/fingerprint.cppm/CHANGELOG);docs/03-toolchains.md MSVC 节
撤 “not yet supported”、补构建说明;#210 关联说明。

**S12 — 流程**
临时分支(仅 ci-windows+release.yml)迭代绿 → 真 PR(全 CI)→ --admin 合入 →
tag v0.0.90 → release(修复版 mirror 的实战验收,publish-ecosystem 应全自动)→
`xlings install mcpp@0.0.90` 验证 → bootstrap pin → 总汇报。

## 风险与回退
- MSVC 模块管线在真实 CI 的未知坑(cl 版本细节、/showIncludes 本地化、rsp 转义):
  全部集中在 S5-S7,靠临时分支 CI 循环消化;门在 S8 才删,之前任何一步可安全暂停。
- e2e 98 的 -freflection 依赖 gcc16 驱动接受度:探测 SKIP 兜底。
- fast-path 缓存格式变更(S8):版本化格式头,老缓存自然失效重建。
