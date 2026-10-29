#!/usr/bin/env bash
# requires: llvm python3
# 942 -- an ok image is built for `<arch>-openkal` and run by okrun (design
# 2026-10-26 part 2, §8.5; openkal SPEC clause 12).
#
# The target's platform, openkal-platform (SPEC-013), compiles for no operating
# system (`<arch>-unknown-none-elf`) with the convention's flags, links with
# `ld.lld` into an ET_DYN image entered at `ok_main`, and publishes the image
# only after its seal -- `okld note`, then `okld check` -- has accepted it.
# `okld` and `okrun` are the platform's tools, built for this machine; `okrun`
# is its runner. The image here has no C library and no C++ run-time: the
# project switches the platform's layers off.
#
# Criteria:
#   A. Both rows build: `x86_64-openkal` and `aarch64-openkal` each produce
#      `bin/app.oke`, an ELF shared object of its architecture, and bin/ holds
#      no unchecked image.
#   B. `mcpp run` on the row of this machine's architecture runs the image
#      through okrun: its arguments, its environment, a callback into the
#      image, thread-local storage and a by-value structure all arrive.
#   C. An image that imports a C library's name (`puts`) is refused by the
#      check, which names the name; the image under `bin/` is still the one the
#      last successful build published.
#   D. `import std;` with the platform's C++ run-time switched off is refused
#      before anything compiles, saying it was switched off and where.
#   E. A second build is up to date.
#   F. `mcpp pack` stages the image in the tree of its platform (`lib/<arch>/`,
#      the launchers in `bin/`), and `--mode self-contained` adds the runner,
#      so the launcher runs with nothing installed; `--pkg-format single-file`
#      is a program of this machine that carries the image (M3). Each gives
#      the image the arguments and the environment.
#   G. A workspace's root package is built as any root is: the image builds
#      and runs through okrun (the selected package is not the graph's first).
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64 ;;
    aarch64|arm64) HOST_ARCH=aarch64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac

# No C library, no C++ run-time, no compiler run-time: the image is its own.
NO_LAYERS='[target.'"'"'cfg(platform = "openkal")'"'"'.layers]
c-abi            = false
"c++-abi"        = false
compiler-runtime = false'

mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.c"

$NO_LAYERS
TOML
cat > src/main.c <<'C'
/* An image with no C library: it calls kal_* and supplies the two routines
 * the compiler may emit, and the emulated thread-local storage the
 * convention requires (SPEC 12.2). */
typedef unsigned long kal_uintptr;
typedef long kal_intptr;
struct kal_stream { kal_uintptr h; };
struct kal_task { kal_uintptr h; };
struct kal_stream kal_stdout(void);
kal_intptr kal_stream_write(struct kal_stream, const void*, kal_uintptr);
kal_uintptr kal_env_arg_count(void);
kal_uintptr kal_env_var_count(void);
int kal_task_start(void (*entry)(void*), void* arg, struct kal_task* out);
int kal_task_join(struct kal_task);
kal_uintptr kal_task_current(void);
void* kal_alloc(kal_uintptr size, kal_uintptr align);

void* memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* a = d; const unsigned char* b = s; while (n--) *a++ = *b++; return d;
}
void* memset(void* d, int c, unsigned long n) {
    unsigned char* a = d; while (n--) *a++ = (unsigned char)c; return d;
}

struct emutls_control { unsigned long size, align; union { unsigned long index; void* address; } object; void* value; };
static struct { kal_uintptr id; void* vars[8]; } g_ctx[16];
static unsigned long g_next;
static volatile int g_lock;
static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }
void* __emutls_get_address(struct emutls_control* c) {
    lock();
    if (c->object.index == 0) c->object.index = ++g_next;
    const unsigned long index = c->object.index;
    const kal_uintptr self = kal_task_current();
    int slot = -1;
    for (int i = 0; i < 16; ++i) if (g_ctx[i].id == self) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < 16; ++i) if (g_ctx[i].id == 0) { g_ctx[i].id = self; slot = i; break; }
    void** var = &g_ctx[slot].vars[index - 1];
    if (!*var) {
        unsigned char* p = kal_alloc(c->size, c->align ? c->align : 8);
        if (c->value) memcpy(p, c->value, c->size); else memset(p, 0, c->size);
        *var = p;
    }
    void* out = *var;
    unlock();
    return out;
}

static void put(const char* s) { unsigned long n = 0; while (s[n]) ++n; kal_stream_write(kal_stdout(), s, n); }
static __thread long tl = 7;
static void worker(void* arg) { long* seen = arg; *seen = tl; tl = 99; }

int ok_main(void) {
    put(kal_env_arg_count() == 3 ? "image: three arguments\n" : "image: arguments lost\n");
    put(kal_env_var_count() > 0 ? "image: an environment\n" : "image: no environment\n");
    tl = 1;
    long seen = 0;
    struct kal_task t;
    if (kal_task_start(worker, &seen, &t) == 0) kal_task_join(t);
    put(seen == 7 && tl == 1 ? "image: each context has its own storage\n" : "image: storage shared\n");
    return 0;
}
C

# A
for arch in x86_64 aarch64; do
    "$MCPP" build --target "$arch-openkal" > "build-$arch.log" 2>&1 \
        || fail "A: $arch-openkal did not build" "build-$arch.log"
    img=$(ls target/"$arch"-openkal/*/bin/app.oke 2>/dev/null | head -1)
    [ -n "$img" ] || fail "A: $arch-openkal published no bin/app.oke" "build-$arch.log"
    python3 - "$img" "$arch" <<'PY' || fail "A: $arch-openkal's image is not an ET_DYN of its architecture"
import struct, sys
d = open(sys.argv[1], 'rb').read(64)
machine = {'x86_64': 62, 'aarch64': 183}[sys.argv[2]]
ok = d[:4] == b'\x7fELF' and struct.unpack_from('<H', d, 16)[0] == 3 and struct.unpack_from('<H', d, 18)[0] == machine
sys.exit(0 if ok else 1)
PY
done
ls target/*-openkal/*/bin/*.unchecked > /dev/null 2>&1 && fail "A: an unchecked image is in bin/" "build-x86_64.log"
echo "ok: both rows publish an ELF image of their architecture"

# B
"$MCPP" run --target "$HOST_ARCH-openkal" -- one two > run.log 2>&1 \
    || fail "B: the image did not run through okrun" run.log
grep -q "image: three arguments" run.log || fail "B: the image's arguments" run.log
grep -q "image: an environment" run.log || fail "B: the image's environment" run.log
grep -q "image: each context has its own storage" run.log || fail "B: thread-local storage or the callback" run.log
echo "ok: okrun runs the image of this machine's architecture"

# E
"$MCPP" build --target "$HOST_ARCH-openkal" > again.log 2>&1 || fail "E: the second build failed" again.log
grep -q "Compiling" again.log && fail "E: the second build compiled again" again.log
echo "ok: a second build is up to date"

# F
"$MCPP" pack --target "$HOST_ARCH-openkal" --pkg-format dir -o m2 > m2.log 2>&1 || fail "F: pack" m2.log
[ -f "target/dist/m2/lib/$HOST_ARCH/app.oke" ] || fail "F: the package has no lib/$HOST_ARCH/app.oke" m2.log
[ -x target/dist/m2/bin/app ] || fail "F: the package has no launcher" m2.log
"$MCPP" pack --target "$HOST_ARCH-openkal" --mode self-contained --pkg-format dir -o m3 > m3.log 2>&1 \
    || fail "F: pack --mode self-contained" m3.log
env -i HOME="$HOME" PATH=/usr/bin:/bin target/dist/m3/bin/app one two > m3run.log 2>&1 \
    || fail "F: the self-contained launcher did not run" m3run.log
grep -q "image: three arguments" m3run.log || fail "F: the self-contained package's image" m3run.log
grep -q "image: an environment" m3run.log || fail "F: the self-contained package's image was given no environment" m3run.log
"$MCPP" pack --target "$HOST_ARCH-openkal" --pkg-format single-file -o m3-file > sf.log 2>&1 \
    || fail "F: pack --pkg-format single-file" sf.log
target/dist/m3-file one two > sfrun.log 2>&1 || fail "F: the single file did not run" sfrun.log
grep -q "image: three arguments" sfrun.log || fail "F: the single file's image" sfrun.log
echo "ok: pack stages the image in its tree, carries the runner, and a single file carries the image (M3)"

# C
before=$(cksum < "$(ls target/"$HOST_ARCH"-openkal/*/bin/app.oke | head -1)" )
# Reached from the entry: the link discards what nothing reaches.
cp src/main.c main.c.good
cat > src/main.c <<'C'
int puts(const char*);
int ok_main(void) { return puts("x"); }
C
if "$MCPP" build --target "$HOST_ARCH-openkal" > refuse.log 2>&1; then
    fail "C: an image importing puts was published" refuse.log
fi
grep -q "puts" refuse.log || fail "C: the refusal does not name the import" refuse.log
grep -q "was not sealed" refuse.log || fail "C: the refusal does not name the seal" refuse.log
after=$(cksum < "$(ls target/"$HOST_ARCH"-openkal/*/bin/app.oke | head -1)" )
[ "$before" = "$after" ] || fail "C: the refused image took the name in bin/" refuse.log
echo "ok: an image that imports a C library's name is refused and not published"

# D
mkdir -p "$TMP/std/src"
cd "$TMP/std"
cat > mcpp.toml <<TOML
[package]
name    = "std-app"
version = "0.1.0"

# The package of the C++ run-time is also the compiler run-time's: both off.
[target.'cfg(platform = "openkal")'.layers]
"c++-abi"        = false
compiler-runtime = false
TOML
printf 'import std;\nint main() { std::println("x"); }\n' > src/main.cpp
if "$MCPP" build --target "$HOST_ARCH-openkal" > std.log 2>&1; then
    fail "D: import std built without a C++ run-time" std.log
fi
grep -q "needs a C++ run-time inside the program" std.log || fail "D: the refusal's reason" std.log
grep -q "switched off" std.log || fail "D: the refusal does not say the layer was switched off" std.log
grep -q "mcpp.toml" std.log || fail "D: the refusal does not say where" std.log
echo "ok: import std with the C++ run-time switched off is refused, saying where"

# G
mkdir -p "$TMP/ws/src" "$TMP/ws/m/src"
cp "$TMP/app/main.c.good" "$TMP/ws/src/main.c"
cat > "$TMP/ws/mcpp.toml" <<TOML
[workspace]
members = ["m"]

[package]
name    = "wsapp"
version = "0.1.0"

[targets.wsapp]
kind = "bin"
main = "src/main.c"

$NO_LAYERS
TOML
printf '[package]\nname = "m"\nversion = "0.1.0"\n\n[targets.m]\nkind = "lib"\n' > "$TMP/ws/m/mcpp.toml"
printf 'int m_value(void) { return 0; }\n' > "$TMP/ws/m/src/m.c"
(cd "$TMP/ws" && "$MCPP" run --target "$HOST_ARCH-openkal" -- one two > run.log 2>&1) \
    || fail "G: a workspace root's image did not build or run" "$TMP/ws/run.log"
grep -q "image: three arguments" "$TMP/ws/run.log" || fail "G: the workspace root's image" "$TMP/ws/run.log"
echo "ok: a workspace root's image is built and run as any root's"

echo "PASS: 942 an ok image is built once and run by okrun"
