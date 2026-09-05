#!/usr/bin/env bash
# tests/matrix/compare.sh <measured.tsv> <expected.tsv> <host> <mode>
#
# 期望表是仓库里的文件,改了行为就必须同时改它。支持矩阵因此不是一份会过期的
# 文档,而是一次测量与一份声明的比对。
#
# 「跳过」必须是期望表说的,不是运行时发现的。一格因为「今天这台机器没装某个
# 载荷」而跳过,与「这个组合本就不支持」是两回事 —— 前者会让矩阵在缺件的机器上悄悄
# 变绿,而那正是本仓库反复付出代价的那种假绿。
set -u
measured="${1:?用法: compare.sh <measured> <expected> <host> <mode>}"
expected="${2:?}"
host="${3:?}"
# MODE 也是参数,不只是键的一列。一次扫描只产出一种体系,而拿它去比整张表,
# 另一种体系的每一行都会被报成「期望表说有而扫描没跑到」—— 一条真判据用错了论域,
# 得到的是一整屏假红。
mode="${4:?}"

# 键含 `mode`。两种体系(payload / graph)跑的是同一组 (host, target, compiler),
# 少了它,后跑的一遍会把先跑的一遍在期望表里的那一行「解释掉」。
key() { awk -F'\t' -v h="$host" -v m="$mode" '$1==m && $2==h {print $1"\t"$2"\t"$3"\t"$4}' "$1" | sort; }
val() { awk -F'\t' -v h="$host" -v m="$mode" '$1==m && $2==h {print $1"\t"$2"\t"$3"\t"$4"\t"$10"\t"$11}' "$1" | sort; }

# 先断言扫描真的跑了。一格没跑与全部通过,在退出码上没有区别。
n_m=$(key "$measured" | wc -l)
n_e=$(key "$expected" | wc -l)
echo "measured $n_m cells, expected table has $n_e for host '$host' mode '$mode'"
if [ "$n_m" = 0 ]; then
  echo "::error::the scan produced no rows for '$host'/'$mode' — it did not run"
  exit 1
fi

fail=0

# ── 期望表里有而实测没有:这一格没跑到 ────────────────────────────────
missing=$(comm -13 <(key "$measured") <(key "$expected"))
if [ -n "$missing" ]; then
  echo "::error::cells the expected table names but the scan never reached:"
  printf '%s\n' "$missing" | sed 's/^/        /'
  fail=1
fi

# ── 状态不符 ──────────────────────────────────────────────────────────
while IFS=$'\t' read -r md h t c st rs; do
  [ -z "${h:-}" ] && continue
  want=$(awk -F'\t' -v m="$md" -v h="$h" -v t="$t" -v c="$c" \
             '$1==m && $2==h && $3==t && $4==c {print $10"\t"$11}' "$expected")
  if [ -z "$want" ]; then
    # 实测有而期望表没有 —— 支持面扩大了,这也要有人确认。
    echo "::error::[$md] $h $t $c → $st, and the expected table does not mention this cell"
    echo "        新增一行到 tests/matrix/expected.tsv,或说明为何不该出现"
    fail=1
    continue
  fi
  # 理由也在判据里。一格从「因为能力 pin 而拒绝」变成「因为约定没被替换而
  # 拒绝」,status 仍是 unsupported —— 而那是两条不同的规则,换了一条却不红,
  # 这张表就只在说「它没建出来」,不在说「为什么」。
  wantSt="${want%%$'\t'*}"; wantRs="${want##*$'\t'}"
  if [ "$st" != "$wantSt" ]; then
    echo "::error::[$md] $h $t $c → measured '$st', expected '$wantSt'"
    fail=1
  elif [ "$rs" != "$wantRs" ]; then
    echo "::error::[$md] $h $t $c → $st for '$rs', expected '$wantRs'"
    fail=1
  fi
done < <(val "$measured")

[ "$fail" = 0 ] || { echo "::error::the matrix does not match what this repository declares"; exit 1; }
echo "OK: $n_m cells match the expected table for '$host'/'$mode'"
