# macOS `mcpp build`:`library not found for -lSystem` 根因与修复

**日期**: 2026-06-24
**症状**: macOS 上全新安装的 mcpp 构建用户工程时,**编译通过、链接失败**:
```
ld64.lld: error: library not found for -lSystem
ld64.lld: error: undefined symbol: __stack_chk_fail / __stdoutp / _Unwind_Resume / fflush / ...
```
全是 libSystem(macOS libc)的符号。

## 1. 为什么「以前 ok、CI 也 ok、新装的就挂」

链接 macOS 代码**没变**(0.0.60→0.0.61 我合的 PR 都没碰 `flags.cppm`/`macos.cppm`/工具链 SDK 逻辑),
所以不是版本回归。关键在 `src/build/flags.cppm` 的 macOS 链接命令 `f.ld`:

```cpp
// macOS 分支(needs_explicit_libcxx)
f.ld = std::format("{}{}{} -fuse-ld=lld{}{}{}", full_static, static_stdlib,
                   b_flag, version_min, user_ldflags, link_extra);
```

**`f.ld` 从不显式传 SDK**(无 `-isysroot`/`--sysroot`)。编译侧拿到了 `--sysroot=<SDK>`
(flags.cppm:186,经 `xcrun --show-sdk-path`),但**链接侧依赖 clang 的「隐式 SDK 探测」**
(xcrun / `SDKROOT` → 给 ld64 传 `-syslibroot`)去找 `libSystem`。

- **GitHub CI 的干净 Xcode runner**:隐式探测正常 → `-syslibroot` 自动加上 → `-lSystem` 解析成功。**缺陷被掩盖**。
- **用户真机**:当隐式探测失效——`xcode-select` 指向错/换了、只装了 Command Line Tools、
  或新装 mcpp 拉了全新的捆绑 clang——ld64.lld 拿不到 `-syslibroot`,`/usr/lib/libSystem.tbd`
  搜不到 → `library not found for -lSystem`,继而所有 libc 符号未定义。

所以这是个**潜伏缺陷**:链接从来就靠隐式探测,真机环境一旦不满足就暴露。「编译过、链接挂」
正是这个特征(编译用 bundled libc++ 头,不依赖 SDK 探测;链接才需要 libSystem)。

## 2. 修复

让 macOS 链接**显式带上 SDK**,不再赌隐式探测:

1. **`flags.cppm`**:macOS `f.ld` 加 `-isysroot <SDK>`(SDK 取自 `macos::sdk_path()`)。
   ```cpp
   std::string macos_sdk;
   if (auto sdk = mcpp::platform::macos::sdk_path())
       macos_sdk = " -isysroot " + escape_path(*sdk);
   f.ld = std::format("{}{}{}{} -fuse-ld=lld{}{}{}", full_static, static_stdlib,
                      b_flag, macos_sdk, version_min, user_ldflags, link_extra);
   ```
2. **`macos.cppm::sdk_path()` 加固**:不止 `xcrun --show-sdk-path`,按序回退——
   `SDKROOT` 环境变量 → `xcrun --sdk macosx --show-sdk-path` → `xcode-select -p` 推导
   `.../SDKs/MacOSX.sdk` → 固定路径(`/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`、
   Xcode.app 内 SDK)。即使 xcrun 返回空也能定位 SDK。

**为何安全**:CI 上 `sdk_path()` 返回 Xcode SDK,`-isysroot` = clang 本会隐式探到的同一个,
行为不变;真机上把以前「碰运气」变「确定」。`ci-macos` 验证未回归工作路径。

## 3. 用户侧自查(若仍失败)

```
xcrun --show-sdk-path     # 应打印有效 SDK 路径
xcode-select -p           # 应指向 CommandLineTools 或 Xcode.app/Contents/Developer
```
若 `xcrun` 为空:`xcode-select --install`(或 `sudo xcode-select --reset`)。
加固后 mcpp 即便 xcrun 异常也会走回退路径找到 SDK。

## 4. 相关
- macOS 部署底线 / 静态 libc++:xlings `.agents/docs/2026-06-05-macos-min-version-support.md`。
- 同批 macOS 首跑问题(单独追踪):ninja bootstrap ~145s、首跑需回车(stdin)、
  `xlings update` 子索引 artifact 404。
