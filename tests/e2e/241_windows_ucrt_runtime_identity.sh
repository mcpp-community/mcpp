#!/usr/bin/env bash
# requires: msvc python3
# 241_windows_ucrt_runtime_identity.sh — the Windows SDK has an identity, and
# it reaches the build's runtime contract.
#
# `RuntimeBinding::runtimeId`'s own comment has documented `ucrt@…` since the
# field existed, and nothing ever wrote one. The cost was not cosmetic: the
# SDK version never reached `runtimeContractHash`, which keys the build cache,
# so TWO SDKs shared ONE cache key — the version axis simply stopped existing
# one layer below the compiler.
#
# The unit tests pin the hash function (two versions → two hashes). What they
# cannot see is whether the value ever gets there on a real Windows build, and
# a green Windows CI does not distinguish "the identity is filled in" from
# "the code path ran and produced nothing" — an empty string flows through
# every one of those jobs without a complaint. So this asserts the VALUE.
#
# WHY IT PINS msvc@system EXPLICITLY. The identity is produced where mcpp
# RESOLVES the SDK itself, which is the native cl.exe path. Windows' default
# toolchain is clang targeting the MSVC ABI, and there clang finds its own SDK
# — mcpp does not know which one, so there is honestly nothing to declare. A
# test that took the default would therefore assert an empty identity and pass
# for the wrong reason.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

"$MCPP" new ucrtid > /dev/null
cd ucrtid
cat >> mcpp.toml <<'EOF'

[toolchain]
windows = "msvc@system"
EOF

"$MCPP" build > build.log 2>&1 || { cat build.log; exit 1; }

RES="$(find target -name resolution.json | head -1)"
[[ -n "$RES" ]] || { echo "FAIL: no resolution.json"; exit 1; }

python3 - "$RES" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
b = d.get("runtime", {}).get("binding", {})
rid = b.get("runtime_id", "")

assert rid.startswith("ucrt@"), (
    "the Windows runtime identity is not filled in: runtime_id="
    f"{rid!r}. The SDK version never reaches runtimeContractHash, so two "
    "SDKs share one build-cache key.")

version = rid[len("ucrt@"):]
assert version and version[0].isdigit(), f"implausible SDK version in {rid!r}"

# The identity is only worth anything if it PARTICIPATES. An empty contract
# hash would mean the value was recorded and then not used for anything.
assert b.get("contract_hash"), "runtime binding has no contract hash"

# ...and it must NOT have been projected into the private-libc field. That
# field is read by the loader/patchelf machinery, and ucrt has no payload for
# it to name — `ucrtbase.dll` is a Windows component. See
# mcpp.runtime.binding on why the two providers are not isomorphic.
assert not b.get("libc"), (
    f"ucrt was projected into `libc` ({b.get('libc')!r}); that field names a "
    "private libc PAYLOAD, and there is no such thing for ucrt")

print(f"OK: runtime identity {rid}, contract {b['contract_hash']}")
PY

echo "OK"
