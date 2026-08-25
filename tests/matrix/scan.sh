#!/usr/bin/env bash
# tests/matrix/scan.sh — 把「这台机器支持哪些目标」变成一次测量的输出。
#
# ⭐⭐ 两个入口,各回答自己那半:
#
#   mcpp why toolchain --target T --toolchain C --format json
#       这一格**会解析成什么**,以及如果不解析,是因为哪一条规则。结构化,
#       不构建。
#
#   mcpp build --target T
#       解析通过之后**它到底建不建得出来**。
#
# ⚠️⚠️ 两个都要,而这是被实测逼出来的。只查不建会把 `llvm × x86_64-windows-gnu`
# 报成绿:它解析得完全正常,失败发生在链接期的封闭性检查上。只建不查则回到老路
# —— 只能靠匹配句子来分辨「拒绝」与「炸了」,而 2026-08-26 同一次会话里,我把
# `cannot emit it` 改成 `cannot be emitted by`,一条断言当场变成空转。
#
# ⭐ 于是这里**没有一处字符串匹配**。分类全部来自 `data.status` / `data.reason`
# 与构建的退出码。
#
# ⚠️ 不直接问编译器。绕开被测对象去问它的组件,得到的是组件的默认行为而不是 mcpp
# 的行为 —— 分析文档里有一次就是这么错的。
#
# 输出 TSV 到 stdout,一行一格:
#   mode host target compiler compiler-triple c-lib c-abi c++-abi openkal status reason
#
# ⚠️ `mode` 是键的一部分,不是注释。两种体系跑的是同一组 (host, target, compiler),
# 少了它两张表会互相覆盖 —— 而覆盖的方向取决于谁后跑,不取决于谁对。
#
# status 只有三种,而三者的区别是整套验收的核心:
#   ok           解析通过,且真的建出来了
#   unsupported  mcpp 自己拒绝了这个组合(reason 列给出规则名)—— 拒绝也是一种正确
#   mismatch     解析说可以,构建却失败了 —— 需要有人看
set -u
MCPP="${MCPP:-mcpp}"
MODE="${1:-payload}"          # payload | graph

# ⚠️ jq 缺席必须是硬错误。它的失败方式本来是「每一格都空着」,而一张全空的表和
# 一张全绿的表在退出码上没有区别 —— 这正是这套矩阵存在的理由。
command -v jq >/dev/null 2>&1 || {
    echo "scan: jq is required (every GitHub-hosted runner ships it)" >&2
    exit 2
}

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"; cd "$work"

# ⚠️⚠️ 构建机是 (os, arch),不是 os。
#
# mcpp 自己发布四份宿主二进制:linux-x86_64 / linux-aarch64 / macosx-arm64 /
# windows-x86_64。两台 Linux 服务的行**不一样** —— `x86_64-linux-gnu` 需要本机
# 架构的 glibc 载荷,所以在 x86_64 上够得着,在 aarch64 上够不着。
#
# 只写 `linux` 会让两台在期望表里用同一个键,后跑的一台把先跑的那台的行「解释掉」,
# 而覆盖的方向取决于谁后跑,不取决于谁对。
case "$(uname -s)" in
  Linux)  HOST_OS=linux ;; Darwin) HOST_OS=macos ;;
  MINGW*|MSYS*|CYGWIN*) HOST_OS=windows ;; *) HOST_OS=unknown ;;
esac
case "$(uname -m)" in
  x86_64|amd64) HOST_ARCH=x86_64 ;;
  aarch64|arm64) HOST_ARCH=$([ "$HOST_OS" = macos ] && echo arm64 || echo aarch64) ;;
  *) HOST_ARCH="$(uname -m)" ;;
esac
HOST="$HOST_OS-$HOST_ARCH"

# 目标与编译器清单都取自 mcpp 自己的机器接口,而不是脚本里再抄一份 —— 抄一份
# 就会漂移,而按列宽解析一张给人看的表,会让列宽变成测试套件的一部分。实测过
# 的代价:两版测试对「版本在第几列」的看法不同,读 `$NF` 的那版取到了
# `(default)` —— 一个恰好只出现在最可能被选中的那一行上的值。
LIST="$("$MCPP" toolchain list --format json 2>/dev/null)"
[ -n "$LIST" ] || { echo "scan: \`toolchain list --format json\` produced nothing" >&2; exit 2; }

targets() { printf '%s' "$LIST" | jq -r '.data.targets[].target' | sort -u; }

# ⭐ 每族只取最新的一个。矩阵回答的是「这个目标支不支持」,同一族的三个版本对
# 这个问题给同一个答案,而 5×12 与 2×12 在 CI 上是小时级的差别。
#
# ⚠️ 但收窄必须说出来。被丢掉的版本写到 stderr —— 一次没跑的测量和一次通过的
# 测量,在退出码上没有区别。
compilers() {
    printf '%s' "$LIST" | jq -r '.data.toolchains[] | .family + "@" + .version' \
    | awk -F@ '{ if (seen[$1]++) print "scan: 略过 " $0 " —— 每族只取最新" > "/dev/stderr"
                 else print $0 }'
}

emit() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@"; }

for tc in $(compilers); do
  for t in $(targets); do
    # ⚠️⚠️ graph 体系不覆盖裸机行,而这是一句声明,不是一次省略。
    #
    # openkal 的这套依赖(`openkal-musl` + `openkal-llvm-runtime`)描述的是一个
    # **有宿主的**系统;把它加到零 libc 的目标上问的不是「图能不能供给这个目标」,
    # 而是一个没有人会写的组合 —— 实测 `x86_64-none-elf` 因此拉进
    # `openkal-opensbi`(RISC-V 的监管者接口)并编译失败。
    #
    # 裸机 × 图由 135/136/292 用正确的包覆盖。这里跳过并说出来。
    if [ "$MODE" = graph ] && printf '%s' "$t" | grep -q -- '-none-elf$'; then
        echo "scan: 略过 graph × $t —— 裸机行不在这套依赖的论域内" >&2
        continue
    fi

    { printf '[package]\nname = "mxscan"\nversion = "0.1.0"\n'
      [ "$MODE" = graph ] && printf '\n[dependencies]\nopenkal-musl = "0.3.5"\nopenkal-llvm-runtime = "0.1.3"\n'
    } > mcpp.toml

    # ⚠️⚠️ 探针必须按目标的层级选,一份源码服务不了整张表。
    #
    # 第一版对每一格都写 `#include <cstdio>`,于是四个裸机目标全报
    # `fatal error: 'cstdio' file not found` 并被记成 `mismatch`。那不是 mcpp
    # 的失败 —— 零 libc 的目标本来就没有 `<cstdio>`,是探针问错了问题。
    #
    # ⭐ 一个自己就编不过的探针,产出的整列都是关于探针的。
    if [ "$MODE" != graph ] && printf '%s' "$t" | grep -q -- '-none-elf$'; then
        printf 'extern "C" void kmain() { for (volatile int i = 0; i < 1; ++i) {} }\n' > src/main.cpp
        printf '\n[target.%s]\nsysroot = ""\n' "$t" >> mcpp.toml
    else
        case "$MODE" in
          graph) printf 'import std;\nint main(){ std::println("ok"); }\n' > src/main.cpp ;;
          *)     printf '#include <cstdio>\nint main(){ std::printf("ok\\n"); }\n' > src/main.cpp ;;
        esac
    fi

    # ── 第一问:这一格会解析成什么 ────────────────────────────────────
    q="$(timeout "${MATRIX_QUERY_TIMEOUT:-300}" \
         "$MCPP" why toolchain --target "$t" --toolchain "$tc" --format json 2>/dev/null)"
    if [ -z "$q" ]; then
        # ⚠️ 查询本身没跑起来。这不是「这一格不支持」,而是「不知道」—— 两者必须
        # 分开,否则一次环境故障会被整片读成「不支持」。
        emit "$MODE" "$HOST" "$t" "$tc" - - - - - mismatch query-failed
        continue
    fi
    jq_get() { printf '%s' "$q" | jq -r "$1" 2>/dev/null; }
    st="$(jq_get '.data.status // "-"')"
    rs="$(jq_get '.data.reason // "-"')"
    tri="$(jq_get '(.data.triple.llvm // "") | if . == "" then "-" else . end')"
    clib="$(jq_get '.data.cLibrary.origin // "-"')"
    # ⚠️ 缺席的层是 `-`,不是 `(none)`。一个空的接口名加一对括号,读起来像
    # 「有这一层而它没名字」,而实际是「这一层不存在」—— 裸机目标的 c-abi
    # 正是后者,那是一句陈述,不是一个空格。
    lay() { jq_get "[.data.layers[] | select(.layer==\"$1\")
                     | if .origin == \"none\" then \"-\"
                       else .interface + \"(\" + .origin + \")\" end][0] // \"-\""; }
    cabi="$(lay 'c-abi')"; cxxabi="$(lay 'c++-abi')"
    okpkg="$(jq_get '[.data.layers[].impl | select(startswith("openkal"))][0] // "-"')"
    : "${tri:=-}" "${clib:=-}" "${cabi:=-}" "${cxxabi:=-}" "${okpkg:=-}"

    if [ "$st" = refused ]; then
        emit "$MODE" "$HOST" "$t" "$tc" "$tri" "$clib" "$cabi" "$cxxabi" "$okpkg" \
             unsupported "$rs"
        continue
    fi

    # ── 第二问:解析说可以,那它建得出来吗 ────────────────────────────
    #
    # ⭐ 退出码就是判据。`why` 已经回答了「为什么不」那一半,所以这里不需要再去
    #    读任何一行输出。
    printf '\n[toolchain]\ndefault = "%s"\n' "$tc" >> mcpp.toml
    rm -rf target
    if timeout "${MATRIX_TIMEOUT:-600}" "$MCPP" build --target "$t" >/dev/null 2>&1; then
        emit "$MODE" "$HOST" "$t" "$tc" "$tri" "$clib" "$cabi" "$cxxabi" "$okpkg" ok none
    else
        emit "$MODE" "$HOST" "$t" "$tc" "$tri" "$clib" "$cabi" "$cxxabi" "$okpkg" \
             mismatch build-failed
    fi
  done
done
