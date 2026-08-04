# 10 - 发布一个库到 mcpp-index

[English](../10-publishing-a-library.md) | **简体中文**

一个库如何变成 `[dependencies]` 可以写出来的东西。这是**库作者**的链路;
[09 - 发布 mcpp](09-release.md) 讲的是发布 mcpp 自身,
[02 - 发布打包](02-pack-and-release.md) 讲的是 `mcpp pack` 打包应用。

## 这条链,只有一种顺序成立

```
你的仓库           merge → git tag → GitHub 自动生成 tag tarball
      ↓
gitcode 镜像       逐字节相同的一份拷贝,供 CN 区
      ↓
mcpplibs/mcpp-index   pkgs/<x>/<name>.lua —— GLOBAL + CN 双 URL + sha256
      ↓            publish-artifact.yml 推出内容哈希 artifact
      ↓
消费方             在自己的 mcpp.toml 里升版本
```

每一支箭头都是一道关。漏掉任何一道都不会当场报错 —— 它会在几小时后,
以别人构建里的 "dependency not found" 或一个 404 的形式出现。

## 1. 打 tag

`mcpp.toml` 里的版本必须与 tag 一致。GitHub 会自动生成
`archive/refs/tags/<tag>.tar.gz`,**那个 tarball 就是产物**,不需要另行上传。

```bash
git tag 0.0.48 && git push origin 0.0.48
curl -fsSL -o pkg-0.0.48.tar.gz \
  https://github.com/<owner>/<repo>/archive/refs/tags/0.0.48.tar.gz
sha256sum pkg-0.0.48.tar.gz          # ← 索引里要写的摘要
```

tarball 解开是 `<repo>-<tag>/`,mcpp 会在这层包装目录里找 `mcpp.toml`。
仓库自带 `mcpp.toml` 时,索引条目不需要 `mcpp` 字段。

## 2. 镜像到 gitcode

CN 条目必须是 GitHub tarball 的**逐字节拷贝**,只改文件名。否则两个区域对同一个
`sha256` 的理解就不一致了。

```bash
gtc release publish mcpp-res/<name> --tag 0.0.48 --asset pkg-0.0.48.tar.gz
```

然后**验证**它 —— 上传报成功和资源真的能取到,不是一回事:

```bash
# 用 GET,绝不用 HEAD —— gitcode 对 HEAD 返回 401,对 GET 返回 302 → CDN 200
curl -fsSL -o cn.tar.gz \
  https://gitcode.com/mcpp-res/<name>/releases/download/0.0.48/pkg-0.0.48.tar.gz
cmp cn.tar.gz pkg-0.0.48.tar.gz      # 必须一致,而不只是"存在"
```

## 3. 加索引条目

在 `mcpplibs/mcpp-index` 的 `pkgs/<首字母>/<name>.lua`:

```lua
["0.0.48"] = {
    url = {
        GLOBAL = "https://github.com/<owner>/<repo>/archive/refs/tags/0.0.48.tar.gz",
        CN     = "https://gitcode.com/mcpp-res/<name>/releases/download/0.0.48/pkg-0.0.48.tar.gz",
    },
    sha256 = "<第 1 步得到的摘要>",
},
```

**三个平台块都要写** —— `linux`、`macosx`、`windows`。源码 tarball 在每个平台上是
同样的字节;只写了其中一个,在另外两个平台上会以 "no such version" 失败,
而那读起来像是消费方 manifest 里打错了字。

## 4. 等 artifact

**索引是 artifact,不是 git clone。** 合进 `main` 还不够:必须等
`publish-artifact.yml` 跑完并推出内容哈希 artifact,客户端之上还有一层刷新 TTL。
手改缓存里的 `pkgs/**` 不起任何作用。

```bash
gh run list --repo mcpplibs/mcpp-index --workflow publish-artifact.yml --limit 1
rm -rf ~/.mcpp/registry/data/<namespace>    # 强制客户端刷新
```

## 5. 冷解析验证,然后再升消费方

这一步的意义在于:本地那份库的 checkout 会掩盖上面每一个错误。要像一个陌生人那样解析它:

```bash
rm -rf ~/.mcpp/registry/data/xpkgs/<ns>-x-<name>/0.0.48
mcpp build                            # 必须真的下载并编译 0.0.48
```

通过之后,才去升消费方的 `[dependencies]`。

> **不要**只用
> `find ~/.mcpp/registry -mindepth 1 -maxdepth 1 ! -name data -exec rm -rf {} +`
> 去强制刷新。`data/xpkgs` 在第 2 层且名字不是 `data`,再来一遍粗心的清理就会把整个
> payload 仓(约 800 MB 工具链)删掉。恢复办法是 `mcpp self doctor` 重新 provision,
> 再 `mcpp update`。

## 对着尚未发布的版本做测试

在上面这条链还没走完时,可以手工播种 registry,让消费方提前编译:

```bash
REG=~/.mcpp/registry/data/xpkgs/<ns>-x-<name>/0.0.48
mkdir -p "$REG"
git -C /path/to/library archive --format=tar --prefix=<repo>-0.0.48/ HEAD \
  | tar -x -C "$REG"
touch "$REG/.mcpp_ok"                 # 表示"已解析"的标记
cp ../0.0.47/.xpkg.lua "$REG/.xpkg.lua"   # 往里加一条 0.0.48 条目
```

mcpp 的构建沙箱是网络隔离的,`file://` 和 `http://127.0.0.1` 形式的索引 URL 取不到,
播种缓存才是可行的办法。

**在相信"真的能用"之前,先把播种的那份删掉。** 播种的 0.0.48 和已发布的 0.0.48
对构建来说毫无区别 —— 而当发布其实失败了的时候,留在那里的正是播种的那一份。

## 检查清单

- [ ] `mcpp.toml` 版本 == git tag
- [ ] tag 已推;tarball 可下载,sha256 已记录
- [ ] gitcode 资源用 **GET** 验证过,且与 GitHub 那份逐字节一致
- [ ] 索引条目写进了**三个**平台块
- [ ] `publish-artifact.yml` 成功
- [ ] 冷解析(删掉播种拷贝后)能下载并编译
- [ ] 消费方已升版本
