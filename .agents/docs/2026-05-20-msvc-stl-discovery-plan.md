# MSVC STL Discovery — msvc.cppm 模块设计

## 问题

mcpp 在 Windows 上查找 `import std` 的 MSVC STL `std.ixx` 时硬编码了
`C:\Program Files\Microsoft Visual Studio\2022`，导致非标准 VS 安装找不到。

## 方案

新建 `src/toolchain/msvc.cppm` 模块，提供 MSVC/Visual Studio 发现能力。

### 查找策略（按优先级）

1. **vswhere.exe** — 微软官方 VS 安装发现工具，最可靠
   - 路径: `C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe`
   - 命令: `vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
   - 返回: VS 安装路径如 `C:\Program Files\Microsoft Visual Studio\2022\Community`

2. **环境变量** — `VSINSTALLDIR`、`VS170COMNTOOLS`(2022)、`VS160COMNTOOLS`(2019)
   - 从 `VSINSTALLDIR` 直接得到 VS 根目录
   - 从 `VS*COMNTOOLS` 向上推算

3. **固定路径扫描** — 兜底方案
   - `C:\Program Files\Microsoft Visual Studio\{2025,2022,2019}\{Enterprise,Professional,Community,BuildTools}`
   - `C:\Program Files (x86)\Microsoft Visual Studio\{2019,2017}\...`

### std.ixx 路径推算

从 VS 安装路径推算:
```
<VS_ROOT>/VC/Tools/MSVC/<version>/modules/std.ixx
```
`<version>` 通过遍历 `VC/Tools/MSVC/` 目录取最新版本。

### 模块接口

```cpp
export module mcpp.toolchain.msvc;
import std;

export namespace mcpp::toolchain::msvc {

// 查找 VS 安装路径（返回最新版本）
std::optional<std::filesystem::path> find_vs_install_path();

// 查找 MSVC 工具链根目录 (VC/Tools/MSVC/<ver>/)
std::optional<std::filesystem::path> find_msvc_tools_dir();

// 查找 std.ixx 模块源文件
std::optional<std::filesystem::path> find_std_module_source();

// 查找 cl.exe（未来 MSVC 工具链支持用）
std::optional<std::filesystem::path> find_cl();

}
```

### 改动点

1. 新建 `src/toolchain/msvc.cppm`
2. `src/toolchain/clang.cppm` — `enrich_toolchain` 中的硬编码 VS 路径改为调用 `msvc::find_std_module_source()`
