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

运行用的 `node` 取自 `xim:emsdk` 声明的依赖 `xim:node`,而不是 PATH 上的某一个。载荷在
`.mcpp-toolchain.json` 里用 `runner` 写出它,项目与依赖图都没有声明 runner 时 mcpp 使用
它。这需要 mcpp 2026.9.12.1,以及在配方更新之后安装的 emsdk 载荷;更早安装的载荷没有
描述文件,仍按产物首行的 `#!/usr/bin/env node` 取 PATH 上的 `node`。

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

三行：`aarch64-ios`（真机）、`aarch64-ios-sim` 与 `x86_64-ios-sim`（模拟器）。

```bash
mcpp build --target aarch64-ios        # 真机产物
mcpp run   --target aarch64-ios-sim    # 模拟器，经由 runner
```

**编译器是生态的，只有 SDK 是 Apple 的。** 这三行钉 `llvm@22.1.8` —— 和
`aarch64-macos` 用的是同一个普通载荷。任何足够新的 clang 都能为一个 iOS 部署目标
产出 arm64 Mach-O；不可打包的是 iPhoneOS 与 iPhoneSimulator 的 SDK，它在 Xcode 里
且不可再分发。所以 mcpp **定位**它，经由 `xcrun --sdk <名字> --show-sdk-path`，与
它一直以来定位 macOS SDK 的方式完全相同。

实测（macos-15，Xcode 16.4，iPhoneOS/iPhoneSimulator 18.5，
`ios_deployment_target = "18.0"`）：

```
aarch64-ios      ->  Mach-O 64-bit executable arm64
                     LC_BUILD_VERSION  platform 2 (IOS)          minos 18.0
aarch64-ios-sim  ->  Mach-O 64-bit executable arm64
                     LC_BUILD_VERSION  platform 7 (IOSSIMULATOR) minos 18.0
mcpp run --target aarch64-ios-sim  ->  1-2-3
```

`platform 2` 与 `platform 7` 是这里唯一值得盯住的读数：一次构建成功分不开这两者，
而一个在设备行上报告 `IOSSIMULATOR` 的产物是一个没有任何后续步骤会拒绝的错误产物。

### 部署目标与 runner

```toml
[build]
ios_deployment_target = "18.0"

[target.aarch64-ios-sim]
runner = ["simctl-run"]
```

部署目标由**有效三元组**承载，别处都不承载 —— `arm64-apple-ios18.0` 与
`arm64-apple-ios18.0-simulator`，这是 Apple 自己的拼法。不发
`-miphoneos-version-min`：三元组已经说过了，而一个标志会成为第二个说它的地方。

`simctl-run` 来自 `xim:apple-simulator-tools`，需要先装：

```bash
xlings install apple-simulator-tools
```

这一步没有写进清单，而这是一处**限制**而不是一个选择：`deps` 不按目标条件化，
而把它写在顶层会让这个例子的 Linux 构建依赖一个只为 macOS 存在的包 —— 两条都实测
过，`mcpp.toml` 里记着那两条消息。

runner 是一个 argv 前缀，而一次**会话**不是：挑一台设备、启动、等待、spawn、把程序
自己的退出状态返回 —— 清单里的一行没有开始也没有结束，这就是那部分知识住在一个包里
而不是住在引擎里的原因。

设备行的 `runner` 保持未设：没有开发者自己拥有的签名，一个产物无法在一台 iOS 设备
上被运行。

### 这台机器上没有 SDK 时

缺失是一次**点名那个 SDK 的拒绝**，而且它发生在任何载荷被解析之前 —— 一台没有
Xcode 的机器不应该先下载一个编译器，然后才被告知缺的不是编译器：

```
error: target aarch64-ios needs the iphoneos SDK, which this machine does not provide.
       ... `xcrun --sdk iphoneos --show-sdk-path` must answer, which needs Xcode on
       macOS (not the Command Line Tools alone -- those ship the macOS SDK only).
```

模拟器是**一个目标**而不是一个 runner：它有自己的 SDK、产出自己的对象，取
`-mios-simulator-version-min` 那一段而真机取 `-miphoneos-version-min` 那一段。
两个架构都在，因为模拟器跑的是**宿主**的架构。

## 支持矩阵

`docs/21-the-target-triple.md` 的表是完整的那一份；这里只列这个例子碰到的行：

| target | tier | pin | 运行过？ |
|---|---|---|---|
| `wasm32-emscripten` | verified | `emsdk@6.0.9` | 是，载荷声明的 `node` |
| `x86_64-linux-android` | verified | `android-ndk@30.0.16248370` | 是，平台模拟器 |
| `aarch64-linux-android` | verified | `android-ndk@30.0.16248370` | 是，`qemu-aarch64-static` + 从镜像取出的 bionic |
| `aarch64-ios` | preview | `llvm@22.1.8` | 否 —— 真机需要开发者自己的签名 |
| `aarch64-ios-sim` | verified | `llvm@22.1.8` | 是，`simctl-run`（macos-15） |
| `x86_64-ios-sim` | preview | `llvm@22.1.8` | 否 —— 模拟器跑宿主架构，而那台宿主是 arm64 |
