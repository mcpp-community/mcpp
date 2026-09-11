# 13 — platform targets

一份源码，三个平台。这里没有任何平台感知的东西：没有 `cfg`、没有预处理分支、
没有按目标分开的源文件。在 Linux 二进制、WebAssembly 模块和 Android 产物之间
变的只有命令行上的 `--target`。

```bash
cd 13-platform-targets
mcpp build && mcpp run            # 宿主
```

## Web

```bash
mcpp run --target wasm32-emscripten
```

实测（linux-x86_64，`xim:emsdk` 6.0.9）：

```
bin/platform-targets        65389 bytes   the JavaScript
bin/platform-targets.wasm  447183 bytes   the module
node bin/platform-targets   ->  1-2-3
```

`mcpp run` 会用 `node` 跑它，所以不需要额外的一步。工程侧**一个新词汇都不需要**：
`wasm32-emscripten` 这一行自己命名了它的载荷（`emsdk@6.0.9`），载荷自带 sysroot，
而 Emscripten 自己就发布一份 libc++ 的模块面。

## Android

```bash
mcpp build --target x86_64-linux-android     # 模拟器
mcpp build --target aarch64-linux-android    # 真机
```

实测（同一台机器，`xim:android-ndk` 30.0.16248370）：

```
aarch64-linux-android  ->  ELF 64-bit LSB pie, ARM aarch64,
                           interpreter /system/bin/linker64
x86_64-linux-android   ->  ELF 64-bit LSB pie, x86-64, 同一个 interpreter
```

**一个钉服务两行。** NDK 不命名架构，`--target` 才命名 —— 所以
`[target.<triple>] toolchain` 不需要写，而两行共用
`android-ndk@30.0.16248370`。

跑起来（x86_64 键在平台自己的模拟器上，API 24 镜像 + KVM）：

```bash
adb push target/x86_64-linux-android/*/bin/platform-targets /data/local/tmp/
adb shell /data/local/tmp/platform-targets
#  ->  1-2-3
```

加载时会有一句告警，它**不是**缺陷：`unsupported flags DT_FLAGS_1=0x8000001`。
API 24 的 bionic 加载器不认识 lld 设置的 `DF_1_PIE` 位，于是告警一句，然后照常
把程序加载起来。

### API level 在 `mcpp.toml` 里，不在 triple 里

`mcpp.toml` 声明的是：

```toml
[target.aarch64-linux-android]
min_api_level = 24
```

规范 triple 保持 `aarch64-linux-android` —— 它命名输出目录、`cfg(env = ...)`
和 ABI tag。级别只进**编译器看到的** triple（`aarch64-unknown-linux-android24`）
和**构建指纹**：级别决定哪些 bionic 符号可见，所以两个级别是两个 ABI，绝不可共用
一个构建目录。

这个键是**可选的**。不写的话，mcpp 读 NDK 自己在 `meta/platforms.json` 里声明的
下限（r30 是 21）。写在这里是因为一个要发布到某个最低版本的工程应该自己说出来，
而不是继承载荷的下限恰好是多少。

`[package] macos_deployment_target` 是 Apple 目标上的同一根轴；一个目标要么是
Apple 要么是 Android，所以两者在指纹里共用一个槽。

## 这一行不能被覆盖

Android 和 wasm 的钉是**能力**而不是约定：

```bash
mcpp build --target aarch64-linux-android    # [target.…] toolchain = "llvm@22.1.8"
# error: target 'aarch64-linux-android' cannot be emitted by 'llvm@22.1.8'.
#        An Android target needs bionic, not just an aarch64 or x86_64 back end:
#        its headers, its per-API-level stubs and its loader path are inside the
#        NDK, and no package adds them to another compiler.
```

一个普通 clang 发 aarch64 ELF 完全没问题 —— 它拿不出来的是**体系**。说出来比
解析出 llvm 再在它内部失败要好。

## iOS

`aarch64-ios`、`aarch64-ios-sim`、`x86_64-ios-sim` 三行在词汇里，都是 `planned`：

```bash
mcpp build --target aarch64-ios-sim
# error: target 'aarch64-ios-sim' is registered but not yet supported (planned)
#        — no toolchain is published for it yet.
```

阻塞项是**许可**而不是载荷：NDK 是 Apache-2.0、Emscripten 是 MIT，而 iPhoneOS 与
iPhoneSimulator 的 SDK 在 Xcode 里，两者都不可再分发。这三行今天买到的是一句
点名那一行的 `tier-planned`，而不是一句假的 `unknown target`。

模拟器是**一个目标**而不是一个 runner：它有自己的 SDK、产出自己的对象，取
`-mios-simulator-version-min` 而真机取 `-miphoneos-version-min`。所以它有自己的
行，而不是折进设备那一行。

## 支持矩阵

`docs/21-the-target-triple.md` 的表是完整的那一份；这里只列这个例子碰到的行：

| target | tier | pin | 运行过？ |
|---|---|---|---|
| `wasm32-emscripten` | verified | `emsdk@6.0.9` | 是，`node` |
| `x86_64-linux-android` | verified | `android-ndk@30.0.16248370` | 是，平台模拟器 |
| `aarch64-linux-android` | preview | `android-ndk@30.0.16248370` | 否 —— 从 x86_64 宿主没有执行路径 |
| `aarch64-ios` / `*-ios-sim` | planned | — | 否 |
