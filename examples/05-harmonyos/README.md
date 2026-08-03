# 05 — HarmonyOS / OpenHarmony

把一个 C++23 具名模块工程交叉编译到鸿蒙 `aarch64-linux-ohos`。

```bash
export OHOS_NDK_HOME=/path/to/ohos-sdk/linux/native
cd 05-harmonyos
mcpp build --target aarch64-linux-ohos

# 没有真机时,用 qemu 验证产物真的能跑
qemu-aarch64 target/aarch64-linux-ohos/*/bin/harmonyos-demo
```

## 这个例子想说明什么

**mcpp 把鸿蒙 SDK 当 sysroot,不当工具链。** 编译器始终是 mcpp 自己的 LLVM ——
因为 SDK 自带的 clang 是 15.0.4(SDK 6.1 / API 23 实测),连 `-fmodule-output`
都没有,一个 C++20 模块也编不出来。SDK 提供的是 libc、CRT、目标 libc++ 和
compiler-rt。

所以这里没有 `import std`:原版 SDK 的 libc++ 是 15.004,不带 `std` 模块。
**具名模块是可以的**,这就是本例的形状。想要 `import std`,需要额外一份为该目标
编译的 libc++ —— 见
[docs/03-toolchains.md#harmonyos--openharmony](../../docs/03-toolchains.md#harmonyos--openharmony)。

`mcpp.toml` 里那个 `[target.aarch64-linux-ohos]` 段**不是必需的**:目标本身就
带一个约定工具链(`src/toolchain/triple.cppm` 的 known-target 表),不写也会选到
LLVM。写出来只是为了把版本钉死。
