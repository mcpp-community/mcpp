#!/usr/bin/env bash
# requires: gcc python3
# 788 -- under `emit build-database`, a host tool that fails to build is a
# warning, not a refusal that costs the plan (mcpp-community/mcpp#699 item 2,
# design 2026-09-26 §4.5).
#
# `user` requests the host tool `t` of package `tool`, whose build carries a
# blocking `check` action that always fails (docs/30's `dep_bin` pattern, and
# e2e 315's fixture for a blocking check). Before this fix `emit` in `user`
# reported `MCPP_BUILD_DATABASE_PLAN_FAILED` with no `data` at all (measured
# in the design record, Appendix A.3) -- the same failure that correctly ends
# `mcpp build`, which does not plan around missing tools. Criteria:
#   A. `emit --format json` in `user`: exit 0, `data` present with `user`'s
#      set, and exactly one warning `MCPP_BUILD_DATABASE_HOST_TOOL_UNBUILT`
#      naming the tool, its package and the first line of the failure.
#   B. `mcpp build` in `user` still exits non-zero: the tool's build itself,
#      and its blocking check, are unchanged.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3

mkdir -p "$TMP/tool/src" "$TMP/user/src"

cat > "$TMP/tool/mcpp.toml" <<'EOF'
[package]
name    = "tool"
version = "0.1.0"

[targets.t]
kind = "bin"
main = "src/main.cpp"
EOF
echo 'int main() { return 0; }' > "$TMP/tool/src/main.cpp"
cat > "$TMP/tool/check.sh" <<'EOF'
#!/usr/bin/env bash
echo "the check says no" >&2
exit 1
EOF
chmod +x "$TMP/tool/check.sh"
# Same shape as e2e 315's blocking-check fixture: a `check` action, marked
# `blocking = true`, that always fails.
cat > "$TMP/tool/build.mcpp" <<'EOF'
#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    mcpp::action a;
    a.id       = "gate";
    a.role     = "check";
    a.blocking = true;
    a.arg((root + "/check.sh").c_str())
     .output("${mcpp.out_dir}/gate.stamp")
     .submit();
}
EOF

cat > "$TMP/user/mcpp.toml" <<'EOF'
[package]
name    = "user"
version = "0.1.0"

[dependencies]
tool = { path = "../tool", tools = ["t"] }
EOF
echo 'int main() { return 0; }' > "$TMP/user/src/main.cpp"

cd "$TMP/user"

# ── A ──────────────────────────────────────────────────────────────────────
set +e
"$MCPP" emit build-database --format json > a.json 2> a.err
rc=$?
set -e
[ "$rc" = 0 ] || fail "A: emit exited $rc, expected 0" a.err a.json
"$PY" - a.json <<'EOF' || fail "A: the envelope" a.json
import json, sys
e = json.load(open(sys.argv[1]))
d = e["data"]
sets = [s["name"] for s in d["database"]["sets"]]
assert sets == ["user"], sets
diags = e["diagnostics"]
assert len(diags) == 1, diags
diag = diags[0]
assert diag["code"] == "MCPP_BUILD_DATABASE_HOST_TOOL_UNBUILT", diag
assert diag["severity"] == "warning", diag
assert "t" in diag["message"] and "tool" in diag["message"], diag["message"]
EOF
echo "ok: A, emit succeeds with user's set and one host-tool warning"

# ── B ──────────────────────────────────────────────────────────────────────
set +e
"$MCPP" build > build.log 2>&1
rc=$?
set -e
[ "$rc" != 0 ] || fail "B: mcpp build succeeded despite the failing blocking check" build.log
grep -q "the check says no" build.log || fail "B: the check's own failure is not on the build's output" build.log
echo "ok: B, mcpp build still fails on the same blocking check"

echo "PASS: 788_emit_host_tool_unbuilt_is_a_warning"
