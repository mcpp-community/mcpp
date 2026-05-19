# Windows LLVM/Clang 支持设计方案

Date: 2026-05-17

## 目标

mcpp 在 Windows x86_64 上通过 xmake bootstrap 达到可用水平，产出 mcpp.exe 作为后续自举依赖。

## 平台特征

### Windows LLVM 包（xlings-res 20.1.7）

```
bin/clang.exe, clang++.exe, clang-cl.exe, lld-link.exe
bin/llvm-ar.exe, llvm-lib.exe, llvm-rc.exe
lib/clang/20/lib/windows/clang_rt.*.lib
没有 libc++（没有 include/c++/v1，没有 std.cppm）
没有 clang-scan-deps.exe
```

Windows LLVM 包不含 libc++。Windows 上 clang 搭配 MSVC STL。

### Bootstrap 策略

用 xmake + MSVC（和 xlings 自身做法一致）：
- GitHub Actions windows-latest 预装 Visual Studio
- xmake 对 MSVC C++23 modules 支持成熟
- 不需要额外安装 LLVM（MSVC 即可）

## 代码适配清单

### 必须修改

| 文件 | 问题 | 方案 |
|------|------|------|
| ninja_backend.cppm | POSIX shell 命令 | #if _WIN32 cmd.exe 语法 |
| ninja_backend.cppm | mcpp_exe_path() 缺 Windows | GetModuleFileNameA() |
| config.cppm | MCPP_HOME 路径发现缺 Windows | 同上 |
| probe.cppm | command -v Unix only | where.exe |
| probe.cppm | LD_LIBRARY_PATH | Windows 用 PATH |
| flags.cppm | 链接 flags 缺 Windows 分支 | 无 sysroot/rpath |
| xlings.cppm | popen | _popen |

## 执行顺序

1. 创建 ci-windows.yml 用 xmake 构建，看编译错误
2. 根据 CI 错误逐步修代码
3. 产出 mcpp.exe bootstrap binary
4. 上传到 xlings-res
