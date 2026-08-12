#!/bin/bash
# Measured A/B for the "BMI-release scheduling" proposal.
#
# Same build directory, same compiler, same flags, same job cap, same source set.
# The ONLY difference is the shape of the ninja graph:
#   baseline : one edge per module; importers wait for the compiler to EXIT
#   split    : two edges per module; importers wait for the BMI to LAND
# One compiler process per module in both arms — total CPU work is identical.
set -u
PROTO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "$PROTO/../../.." && pwd)"
NINJA=$(command -v ninja)
J=${J:-$(nproc)}

# Regenerate build.ninja first: the benchmark matrix may have left the tree in a
# state where the newest build dir belongs to a different fingerprint.
( cd "$R" && mcpp build >/dev/null 2>&1 )

BD=$(ls -td $R/target/x86_64-linux-gnu/*/ | while read -r d; do [ -f "$d/build.ninja" ] && echo "$d" && break; done)
[ -z "$BD" ] && { echo "no build dir with build.ninja"; exit 1; }
echo "build dir : $BD"
echo "ninja     : $NINJA   -j$J"

cd "$BD" || exit 1
python3 "$PROTO/split_graph.py" build.ninja build-split.ninja "$PROTO" || exit 1

wipe() { rm -rf obj gcm.cache slots .bmisem bin .ninja_deps; }

# In the split arm, CPU parallelism is capped by the SEMAPHORE (MCPP_BMI_JOBS),
# not by ninja's -j: once a compiler is detached it no longer occupies a ninja
# slot. ninja's -j must therefore be much LARGER than the compiler cap, or its
# slots fill with edges that are merely sleeping — blocked on the semaphore or
# waiting for codegen — and the ready frontier starves. With -j equal to the cap
# the schedule degenerates to the baseline, which is what the first attempt here
# measured. Both arms still run at most $J compilers at once.
run() {  # name ninjafile ninja_jobs
    wipe
    local s e
    s=$(date +%s.%N)
    MCPP_BMI_JOBS=$J MCPP_BMI_SEM=.bmisem "$NINJA" -f "$2" -j"$3" > "/tmp/proto_$1.log" 2>&1
    local rc=$?
    e=$(date +%s.%N)
    printf '%-10s rc=%d  wall=%.2fs  ninja -j%-4s compilers<=%s  binary=%s\n' \
           "$1" "$rc" "$(echo "$e-$s" | bc)" "$3" "$J" \
           "$([ -f bin/mcpp ] && stat -c %s bin/mcpp || echo MISSING)"
    [ $rc -ne 0 ] && tail -15 "/tmp/proto_$1.log"
    return $rc
}

echo
echo "=== arm 1: baseline graph (edge-complete release) ==="
run baseline build.ninja "$J"

echo
echo "=== arm 2: split graph (BMI-flush release) ==="
run split build-split.ninja "$((J * 6))"

echo
echo "--- sanity: were BMI edges actually short-lived? ---"
python3 - <<'PY'
rows=[]
for line in open('.ninja_log'):
    if line.startswith('#'): continue
    p=line.rstrip('\n').split('\t')
    if len(p)>=5: rows.append((int(p[0]),int(p[1]),p[3]))
start=0
for i in range(1,len(rows)):
    if rows[i][1]<rows[i-1][1]: start=i
rows=rows[start:]
bmi=[e-s for s,e,o in rows if o.startswith('gcm.cache/')]
obj=[e-s for s,e,o in rows if o.startswith('obj/') and o.endswith('.o')]
f=lambda v: f"n={len(v)} sum={sum(v)/1000:.1f}s median={sorted(v)[len(v)//2] if v else 0}ms max={max(v) if v else 0}ms"
print("  BMI edges:", f(bmi))
print("  OBJ edges:", f(obj))
print("  => BMI median must be a FRACTION of the OBJ median, or phase 1 is not")
print("     exiting early and the arm is measuring the baseline in disguise.")
PY

echo
echo "=== verify the split build produced a WORKING binary ==="
./bin/mcpp --version || echo "BINARY BROKEN"

echo
echo "=== leftover detached compilers? (must be 0) ==="
pgrep -c cc1plus || echo 0
