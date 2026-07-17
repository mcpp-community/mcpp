# mcpp 通用构建能力需求清单(G1–G9)

> 日期:2026-07-17
> 视角:每条都是对媒体/加密/数值计算类库通用的机制,不是给 ffmpeg/opencv 开特例。
> 背景:mcpp-index 收录大型 C/C++ 库(ffmpeg、opencv、openssl、dav1d、x264、BLAS 族)的 POC 过程中沉淀的通用能力缺口。
> 对应设计方案:[2026-07-17-asm-sources-and-general-build-capabilities-design.md](2026-07-17-asm-sources-and-general-build-capabilities-design.md)

## 先回答:要支持 nasm 构建吗?

**要,建议做成一等公民(`.asm` 直接进 sources)**,理由四条:

1. 手写汇编是 FFmpeg/dav1d/x264/OpenSSL/BLAS 整个生态的常态,mcpp 定位"大型 C/C++ 库源码构建"迟早绕不开;
2. 声明式进 sources 自动获得指纹/增量/并行,不用每个包自己写 build.mcpp 管缓存重跑;
3. nasm 本身是交叉汇编器,原生支持后 cross 零特判(build.mcpp 路线在 cross 下还要先解决跳过问题);
4. Cargo 没有原生汇编(逼出了 cc crate 生态),Zig 有 `addAssemblyFile` —— mcpp 该站 Zig 这边。

## 通用能力清单(G1–G9)

### 声明式构建能力(高杠杆)

- **G1 原生汇编源**:`.S`(GAS,现有 cc 驱动就能编,覆盖 ARM 汇编,成本≈加后缀路由)+ `.asm`(NASM,`-f` 格式按目标三元组推导,覆盖 x86 汇编)★本清单最高杠杆
- **G4 per-glob flags**:`[build.flags."**/*.avx2.cpp"] cxxflags=["-mavx2"]` —— 两大用例:SIMD 多档 dispatch TU、三方代码告警隔离(现在只能全包 `-w`,连自己的封装层都被静默)
- **G5 mcpp.toml 的 features gate sources**:index 描述符能做(compat.gtest),mcpp.toml 不能——两边不对称;而"feature = 一组源文件 + 一个 define"正是 vendored 大库最高频形态(imgcodecs 17 种编解码器、ffmpeg codec 集合、highgui 后端)

### build.mcpp 补全(通用构建期逻辑)

- **G2 运行依赖包的 build.mcpp**(实测 0.0.93 静默跳过;作用域照 Cargo:flags 只进该包自身 TU,link 指令传播到终链)——没有它 build.mcpp 只对根项目有意义
- **G3 build.mcpp 环境契约**:cross 下运行 + 暴露 TARGET/HOST/活跃 features/PROFILE/OUT_DIR(对应 Cargo 的环境变量族)——不暴露 features,build 程序无法按 codec 集合筛汇编列表

### 供给与修缮

- **G7 xlings 打包 nasm**(≥2.16;缺失硬失败不降级)——**已完成:最新 xlings 已支持 nasm**
- **G8a** glob 不跟目录软链接、**G8b** 相对 `-I` 对 `.cppm` 单元不生效(两个 POC 实测的一致性问题)
- **G6 generated_files 进 mcpp.toml**(与 index 对齐,小项)
- **G9 跨项目构建产物缓存**(生态级远期;缓存键含工具链指纹,与"源码构建"哲学不矛盾)

## 排期建议

G7(已完成)→ **G1**(最高杠杆)→ **G5、G4**(声明式三连,vendored 大库形态就此完整)→ **G2+G3** → G8 顺手 → G6/G9 远期。

映射到收录项目:F1+ 的 ffmpeg 汇编加速硬依赖 G7 + (G1 或 G2+G3),G1 路线最短;P5 的 OpenCV SIMD dispatch 硬依赖 G4 或 G2+G3;而 F0/F1(纯 C)和 P0–P4 零阻塞,随时可开工。
