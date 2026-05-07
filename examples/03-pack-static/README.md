# 03 — pack-static

演示：用 `mcpp pack --mode static` 出一个可发布到**任意 Linux x86_64**的
单文件静态二进制。

```bash
cd 03-pack-static

# bare `mcpp pack` 已在 [pack] default_mode 里 pin 成 "static"
mcpp pack

# 看产物
ls target/dist/
# static-app-0.1.0-x86_64-linux-musl-static.tar.gz
# static-app-0.1.0-x86_64-linux-musl-static/   # 打包前 staging 目录,与 tarball stem 同名

# tarball 解开后会得到一个跟文件名同名的顶层目录
mkdir -p /tmp/extracted
tar -xzf target/dist/static-app-0.1.0-x86_64-linux-musl-static.tar.gz -C /tmp/extracted
/tmp/extracted/static-app-0.1.0-x86_64-linux-musl-static/static-app

# 在 Docker scratch / Alpine 这种最小镜像也能跑:
docker run --rm \
  -v /tmp/extracted/static-app-0.1.0-x86_64-linux-musl-static:/app \
  alpine /app/static-app
```

## 关键配置（`mcpp.toml`）

```toml
[target.x86_64-linux-musl]
toolchain = "gcc@15.1.0-musl"      # musl 工具链
linkage   = "static"               # 全静态链接

[pack]
default_mode = "static"            # bare `mcpp pack` 走这条
```

第一次 build 时 mcpp 会自动装 musl-gcc 15.1（约 800 MB），后续所有 static
打包都共用这一份。
