#!/usr/bin/env bash
# requires: gcc unix-shell jq
# A declared toolchain overrides a convention. It does not override a capability.
#
# ⭐⭐ THE TARGET TABLE'S PIN MEANS TWO DIFFERENT THINGS AND ONLY ONE OF THEM IS
# A PREFERENCE.
#
#   hosted row      `x86_64-linux-musl → gcc@16.1.0`
#                   "this payload supplies the target's C library" — an author
#                   who names their own compiler has said they supply it
#                   instead, so the declaration wins.
#
#   bare-metal row  `riscv64-none-elf → llvm@22.1.8`
#                   the table's own words: "the pin is llvm on every host
#                   because clang/lld are cross-compilers by construction".
#                   A host g++ does not emit riscv64 whatever anyone declares.
#
# ⚠️ MEASURED 2026-08-26, before this file existed:
#
#     [toolchain] default = "gcc@16.1.0"
#     $ mcpp build --target riscv64-none-elf
#       g++: error: unrecognized argument in option '-mabi=lp64d'
#       g++: note: valid arguments to '-mabi=' are: ms sysv
#
# — a message about an option, for a decision made a hundred lines earlier.
#
# ⭐⭐ BOTH DIRECTIONS, BECAUSE REFUSING EVERYTHING ALSO STOPS THE BAD MESSAGE.
# Half two declares gcc for a HOSTED target and requires it to be honoured; a
# guard that refused there would take away the escape hatch the whole
# convention/preference distinction exists to protect.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
cd "$work"
printf 'extern "C" int main(int, char**, char**) { return 0; }\n' > src/main.cpp

# ⭐ From mcpp's machine interface rather than from the table it prints for
# people: a column position is not a contract, and two earlier drafts of the
# neighbouring test disagreed about which column held the version.
gccver="$("$MCPP" toolchain list --format json 2>/dev/null \
          | jq -r '[.data.toolchains[] | select(.family=="gcc") | .version][0] // empty' | tr -d '\r')"
if [ -z "$gccver" ]; then
    echo "SKIP: gcc is not installed here, and this test is about declaring it"
    exit 0
fi

# ── Half one: a bare-metal target refuses, and says why ───────────────────
printf '[package]\nname    = "capprobe"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "gcc@%s"\n\n[target.riscv64-none-elf]\nsysroot = ""\n' \
    "$gccver" > mcpp.toml
rm -rf target
out="$(cd "$work" && "$MCPP" build --target riscv64-none-elf 2>&1 || true)"

# ⭐⭐ THE CLASSIFICATION COMES FROM THE MACHINE INTERFACE, THE WORDING FROM THE
# MESSAGE. `data.reason` is a finite token, so "did it refuse, and under which
# rule" survives any rewording; the assertions below still require the sentence
# to name the target, the reason and the way out, because that is a promise the
# code cannot keep on its own.
#
# ⚠️ MEASURED COST OF NOT SPLITTING THEM: this file asserted `cannot emit it`,
# the message was reworded to `cannot be emitted by` in the same session, and
# the assertion then passed by matching nothing at all.
reason="$(cd "$work" && "$MCPP" why toolchain --target riscv64-none-elf \
            --toolchain "gcc@$gccver" --format json 2>/dev/null \
          | jq -r '.data.reason // "-"' | tr -d '\r')"

case "$reason" in
  capability-pin)
    echo "  ok  a bare-metal target refuses a compiler that cannot emit it" ;;
  none)
    # ⚠️ THE OLD BEHAVIOUR EXACTLY: resolution succeeded, gcc was handed a
    # riscv64 target, and the complaint arrived a hundred lines later as
    # `unrecognized argument in option '-mabi=lp64d'`.
    echo "FAIL: the declaration was honoured and the build was left to fail later"
    printf '%s\n' "$out" | grep -iE 'error|note' | head -3 | sed 's/^/        /'
    exit 1 ;;
  *)
    # ⚠️ NEITHER OUTCOME MEANS THIS MACHINE CANNOT RUN THE TEST. A build that
    # SUCCEEDED with gcc would be a third answer entirely, and one worth
    # failing on: it would mean a host g++ emitted riscv64.
    if printf '%s\n' "$out" | grep -q 'Finished'; then
        echo "FAIL: gcc built a bare-metal riscv64 target — that should not be possible"
        exit 1
    fi
    echo "SKIP: the refusal reason here was '$reason', not one this test knows"
    printf '%s\n' "$out" | grep -iE 'error' | head -2 | sed 's/^/        /'
    exit 0 ;;
esac

# ⭐ AND THE MESSAGE POINTS AT THE DECISION. A refusal that does not name the
# target and the way out leaves the reader where the `-mabi` message did.
ok=1
printf '%s\n' "$out" | grep -q 'riscv64-none-elf'       || ok=0
printf '%s\n' "$out" | grep -qi 'capability'            || ok=0
printf '%s\n' "$out" | grep -q '\[toolchain\]'          || ok=0
if [ "$ok" = 1 ]; then
    echo "  ok  and it names the target, the reason and the way out"
else
    echo "FAIL: the refusal does not point at the decision"
    printf '%s\n' "$out" | head -6 | sed 's/^/        /'
    exit 1
fi

# ── Half two: a hosted target still honours the declaration ───────────────
#
# ⚠️⚠️ THE TARGET COMES FROM THE QUERY, NOT FROM A LITERAL. This half used to
# name `x86_64-linux-gnu`, which is a hosted row on Linux and a CROSS row on
# Windows — where the host target is `x86_64-windows-msvc` and gcc does not
# serve it. Measured on windows-2022, the query for that combination produced
# something jq could not parse, and the test died on `jq: parse error` with no
# statement about what it had found.
#
# The claim is "a hosted row still honours a declared toolchain", so the row to
# use is the one this toolchain would use anyway.
printf '[package]\nname    = "capprobe"\nversion = "0.1.0"\n\n[toolchain]\ndefault = "gcc@%s"\n' \
    "$gccver" > mcpp.toml
printf '#include <cstdio>\nint main() { std::printf("ok\\n"); }\n' > src/main.cpp
hostedTarget="$(cd "$work" && "$MCPP" why toolchain --toolchain "gcc@$gccver" \
                  --format json 2>/dev/null | jq -r '.data.triple.toolchain // empty' | tr -d '\r')"
if [ -z "$hostedTarget" ]; then
    echo "SKIP: the query did not name a target for gcc@$gccver on this host"
    exit 0
fi
rm -rf target
hosted="$(cd "$work" && "$MCPP" build --target "$hostedTarget" 2>&1 || true)"
hostedReason="$(cd "$work" && "$MCPP" why toolchain --target "$hostedTarget" \
                  --toolchain "gcc@$gccver" --format json 2>/dev/null \
                | jq -r '.data.reason // "-"' 2>/dev/null | tr -d '\r')"
[ -n "$hostedReason" ] || hostedReason="-"

case "$hostedReason" in
  capability-pin)
    echo "FAIL: $hostedTarget refused a declared toolchain — the escape hatch is gone"
    printf '%s\n' "$hosted" | head -4 | sed 's/^/        /'
    exit 1 ;;
  none)
    # ⭐ AND IT MUST ALSO BUILD. "Not refused" alone would pass on a machine
    # where resolution succeeded and the link then failed.
    case "$hosted" in
      *"Finished"*) echo "  ok  a hosted target still honours the declared toolchain" ;;
      *) echo "FAIL: the hosted control resolved but did not build"
         printf '%s\n' "$hosted" | grep -iE 'error' | head -2 | sed 's/^/        /'
         exit 1 ;;
    esac ;;
  *)
    echo "SKIP: the hosted control's reason was '$hostedReason', not one this test knows"
    printf '%s\n' "$hosted" | grep -iE 'error' | head -2 | sed 's/^/        /'
    exit 0 ;;
esac

echo "OK: a capability pin is not a preference, and a convention still is"
