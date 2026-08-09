#!/usr/bin/env bash
# requires: elf gcc python3
# 209_runtime_verdict_never_reruns_green.sh — a proven runtime mismatch must
# not disappear when you simply run the build again.
#
# The Linux runtime closure check (Rule A/B) runs after the link, and its
# verdict is cached against the artifact's stat plus the RuntimeBinding
# contract hash. Caching a FAILURE is the interesting half: an unchanged
# artifact that was proven wrong is still wrong, and a build system that
# reports success on the second invocation has told the user the problem went
# away when nothing about the program changed.
#
# It would be easy to get wrong by accident, because the project-level fast
# path exists precisely to skip work when nothing changed — including, if
# nobody stopped it, the check that would have failed. So the fast path is only
# allowed to engage when every stored verdict is a PASS, and this test pins
# that: poison one stored verdict and the very next `mcpp build` must fail
# without touching a single source file.
#
# The other half matters too: the verdict is cached, not permanent. Change the
# program and it is re-derived from the new artifact.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"
EOF
cat > app/src/main.cpp <<'EOF'
int main() { return 0; }
EOF

cd app
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "initial build failed"; exit 1; }

CACHE=$(find target -name '.mcpp-runtime-verdicts.json' | head -1)
if [[ -z "$CACHE" ]]; then
    echo "SKIP: no glibc runtime binding on this host (no stored verdicts)"
    exit 0
fi

# Every artifact must have been validated and recorded as passing, or the rest
# of this test is asserting against the wrong starting state.
python3 - "$CACHE" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
artifacts = doc.get("artifacts") or {}
assert artifacts, "no artifacts recorded in the verdict cache"
bad = {k: v.get("status") for k, v in artifacts.items() if v.get("status") != "pass"}
assert not bad, f"expected every artifact to pass on a clean build, got {bad}"
print(f"stored verdicts: {len(artifacts)} pass")
PY

# ── 1. a stored mismatch survives a no-op re-run ────────────────────
# The fingerprint is left untouched: this is exactly the state a real Rule A/B
# failure leaves behind, and exactly the state the fast path would otherwise
# treat as "nothing to do".
python3 - "$CACHE" <<'PY'
import json, sys
path = sys.argv[1]
doc = json.load(open(path))
for entry in doc["artifacts"].values():
    entry["status"] = "proven_mismatch"
    entry["diagnostics"] = ["synthetic mismatch recorded by 209"]
json.dump(doc, open(path, "w"), indent=2)
PY

if "$MCPP" build > rerun.log 2>&1; then
    cat rerun.log
    echo "FAIL: a stored proven mismatch went green on a plain re-run"
    exit 1
fi
grep -q "runtime closure validation failed" rerun.log || {
    cat rerun.log
    echo "FAIL: the re-run failed, but not for the recorded runtime mismatch"
    exit 1
}
grep -q "synthetic mismatch recorded by 209" rerun.log || {
    cat rerun.log
    echo "FAIL: the stored diagnostics were not reported to the user"
    exit 1
}

# ── 2. `mcpp self doctor` explains the stored verdict without re-probing ──
doctor_out=$("$MCPP" self doctor 2>&1 || true)
printf '%s\n' "$doctor_out" | grep -qi "mismatch" || {
    printf '%s\n' "$doctor_out"
    echo "FAIL: doctor did not surface the stored runtime mismatch"
    exit 1
}

# ── 3. cached, not permanent ────────────────────────────────────────
# A changed artifact gets a fresh verdict. Otherwise the first real mismatch
# would wedge the project forever and the only cure would be deleting target/.
cat > src/main.cpp <<'EOF'
int main() { return 0; }
// touched so the artifact is relinked and the verdict is re-derived
EOF
"$MCPP" build > recovered.log 2>&1 || {
    cat recovered.log
    echo "FAIL: a rebuilt artifact must be re-validated, not stay condemned"
    exit 1
}

python3 - "$CACHE" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
statuses = {k: v.get("status") for k, v in (doc.get("artifacts") or {}).items()}
assert statuses, "verdict cache lost its artifacts after the rebuild"
assert all(s == "pass" for s in statuses.values()), statuses
print("re-derived verdicts:", statuses)
PY

echo "OK"
