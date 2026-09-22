# 11 —— 发布一个库到 mcpp-index

[English](../11-publishing-a-library.md) | **简体中文**

**读者：** 希望自己的包能被别人写进 `[dependencies]` 的库作者。

**本章回答的那一个问题：** 从打好一个 tag 到成为一个可解析的包，要走过哪些步骤，
以及它们必须按什么顺序发生。

**不在这里：** 什么使两个包成为同一个包 —— [SPEC-001](../specs/package-identity.md)；
以及交付编译好的产物，那是 [12](12-binary-distribution.md)。在此之前：
[10 —— 打包一个应用](10-pack-and-release.md)。

一个库如何变成 `[dependencies]` 能够写出来的东西。这是**库作者**的链路；
[92 —— 发布 mcpp](92-release.md) 讲的是发布 mcpp 自身，
[10 —— 打包与发布](10-pack-and-release.md) 讲的是 `mcpp pack` 打包一个应用。

## 发布顺序

```
the library repo          merge → git tag → GitHub auto-generates the tag tarball
      ↓
gitcode mirror     a byte-identical copy, for the CN region
      ↓
mcpplibs/mcpp-index   pkgs/<x>/<name>.lua — GLOBAL + CN URLs + sha256
      ↓            publish-artifact.yml pushes a content-hash artifact
      ↓
consumers          bump the version in their mcpp.toml
```

每一支箭头都是一道关。漏掉任何一道都不会当场报错，而是在几小时后，以别人构建里
一句 `dependency not found` 或一个 404 的形式出现。

## 1. 打 tag

`mcpp.toml` 里的版本必须与 tag 一致。GitHub 自动生成
`archive/refs/tags/<tag>.tar.gz`，**那个 tarball 就是产物**，不需要另行上传。

```bash
git tag 0.0.48 && git push origin 0.0.48
curl -fsSL -o pkg-0.0.48.tar.gz \
  https://github.com/<owner>/<repo>/archive/refs/tags/0.0.48.tar.gz
sha256sum pkg-0.0.48.tar.gz          # ← the digest the index will carry
```

tarball 解开后是 `<repo>-<tag>/`，mcpp 在这层包装目录里查找 `mcpp.toml`。仓库自带
`mcpp.toml` 时，索引条目不需要 `mcpp` 字段。

## 2. 镜像到 gitcode

CN 条目必须是 GitHub tarball 的**逐字节拷贝**，只改文件名。否则两个区域对同一个
`sha256` 的理解就不一致。

```bash
gtc release publish mcpp-res/<name> --tag 0.0.48 --asset pkg-0.0.48.tar.gz
```

随后验证它，因为上传报告成功，与资源真的能取到，不是同一回事：

```bash
# GET, never HEAD — gitcode answers HEAD with 401 and GET with 302 → CDN 200
curl -fsSL -o cn.tar.gz \
  https://gitcode.com/mcpp-res/<name>/releases/download/0.0.48/pkg-0.0.48.tar.gz
cmp cn.tar.gz pkg-0.0.48.tar.gz      # must be identical, not merely present
```

## 3. 添加索引条目

在 `mcpplibs/mcpp-index` 的 `pkgs/<首字母>/<name>.lua` 中：

```lua
["0.0.48"] = {
    url = {
        GLOBAL = "https://github.com/<owner>/<repo>/archive/refs/tags/0.0.48.tar.gz",
        CN     = "https://gitcode.com/mcpp-res/<name>/releases/download/0.0.48/pkg-0.0.48.tar.gz",
    },
    sha256 = "<the digest from step 1>",
},
```

**三个平台块都要写** —— `linux`、`macosx`、`windows`。源码 tarball 在每个平台上是
同样的字节；只写在其中一个平台块里，会在另外两个平台上以 `no such version` 失败，
读起来像是消费方 manifest 里打错了字。

## 4. 等待 artifact

**索引是一个 artifact，不是一次 git clone。** 合进 `main` 还不够：必须等
`publish-artifact.yml` 跑完并推出一个内容哈希 artifact，客户端之上还叠着一层刷新
TTL。手改缓存里的 `pkgs/**` 不起任何作用。

```bash
gh run list --repo mcpplibs/mcpp-index --workflow publish-artifact.yml --limit 1
rm -rf ~/.mcpp/registry/data/<namespace>    # force a client refresh
```

## 5. 从一次冷解析验证，再升消费方

这一步的意义在于：本地那份库的 checkout 会掩盖上面每一步的错误。要像一个陌生人
那样解析它：

```bash
rm -rf ~/.mcpp/registry/data/xpkgs/<ns>-x-<name>/0.0.48
mcpp build                            # must download and compile 0.0.48
```

只有到这时，才去升消费方的 `[dependencies]`。

> **不要**只用
> `find ~/.mcpp/registry -mindepth 1 -maxdepth 1 ! -name data -exec rm -rf {} +`
> 来强制刷新。`data/xpkgs` 位于第 2 层且名字不是 `data`，再来一次粗心的清理就会把
> 整个 payload 仓（约 800 MB 的工具链）一并删掉。恢复办法是先用 `mcpp self doctor`
> 重新 provision，再 `mcpp update`。

## 针对尚未发布版本的测试

在上面这条链还没走完时，可以手工向 registry 播种，让消费方在库发布之前就能编译：

```bash
REG=~/.mcpp/registry/data/xpkgs/<ns>-x-<name>/0.0.48
mkdir -p "$REG"
git -C /path/to/library archive --format=tar --prefix=<repo>-0.0.48/ HEAD \
  | tar -x -C "$REG"
touch "$REG/.mcpp_ok"                 # the marker that says "resolved"
cp ../0.0.47/.xpkg.lua "$REG/.xpkg.lua"   # add a 0.0.48 entry to it
```

mcpp 的构建沙箱与网络隔离，`file://` 和 `http://127.0.0.1` 形式的索引 URL 都取不到，
因此播种缓存是唯一可行的办法。

**在相信「真的能用」之前，先删掉播种的那一份。** 播种出来的 0.0.48 与已发布的
0.0.48 对构建而言毫无区别，而当发布实际上已悄悄失败时，留在那里的正是播种的那一份。

## 需要版本下限的 manifest 键

多数 `[build]` 键在较旧的 mcpp 上会干净地降级：警告该键不受支持、忽略它，构建要么
照常成功，要么以一条清楚的信息失败。`build_program_timeout` 就属于这一类 ——
较旧的 mcpp 会回落到 600 秒的默认值，如果这个值太短，也会明确报出。

**`module_extensions` 不属于这一类。** 较旧的 mcpp 会警告并忽略它，随后把那些文件
当作普通翻译单元编译 —— 得到的是一个**错误的构建**，而不是一次干净的失败：模块
接口不产生 BMI，故障在更晚的地方浮现，报错既不点名这条键，也不点名那个文件。

使用 `module_extensions` 的已发布包，必须在其索引描述符中声明一个 mcpp 版本下限。
下限机制必须能够**降级**：因客户端过旧而不可用的包，必须被报告为**不可用**，绝不能
报告为**不存在** —— 被告知「无此包」的客户端会不断刷新索引去找它。

## 检查清单

- [ ] `mcpp.toml` 版本 == git tag
- [ ] tag 已推送；tarball 可下载，sha256 已记录
- [ ] gitcode 资源已用 **GET** 验证，且与 GitHub 那份逐字节一致
- [ ] 索引条目写进了**三个**平台块
- [ ] `publish-artifact.yml` 已成功
- [ ] 冷解析（已删掉播种拷贝）能下载并编译
- [ ] 消费方已升版本
</content>
