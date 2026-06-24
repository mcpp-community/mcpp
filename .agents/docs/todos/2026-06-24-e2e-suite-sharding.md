# TODO: e2e 套件按耗时分片(并行 matrix)

**日期**: 2026-06-24
**状态**: 待做(已登记,先不做)
**仓库**: `mcpp-community/mcpp`

## 背景
`#157` 已把 e2e 拆成独立 `ci-linux-e2e.yml`,与 build/单测/工具链矩阵**并行**。
`tests/e2e/run_all.sh` 现在每个用例打印耗时 + 末尾「最慢优先」汇总。但 e2e 套件
(~75 个用例)仍是单 job 串行跑,是每个 PR 的最长一环。

## 目标
用现有计时数据把 e2e **按累计耗时切成 N 个均衡分片**,每片一个并行 matrix job,
进一步压缩 e2e 墙钟时间。

## 思路
1. 跑一次 `run_all.sh` 收集每个用例耗时(数据已有,每个 PR 都打印)。
2. 贪心装箱:按耗时降序把用例分到 N 组(N=2~3),使各组总秒数接近。
3. `ci-linux-e2e.yml` 改成 matrix(`shard: [1,2,3]`),`run_all.sh` 加
   `E2E_SHARD_INDEX` / `E2E_SHARD_TOTAL` 环境变量,只跑分到本片的用例。
4. 分片清单可写死(按一次测量),或 `run_all.sh` 读耗时缓存动态分。

## 注意
- 工具链 warm(gcc/musl/llvm)是各片共享成本——分片越多,重复 warm 成本越高,
  N 取 2~3 较优(别过度分片)。
- 保持「最慢优先」汇总;分片后每片各自汇总,或合并 artifact 汇总。
