#!/usr/bin/env bash
# requires: elf python3 unix-shell
# Provider-neutral runtime provenance: same-short-name providers remain exact,
# requirements cannot self-provide, and `mcpp why runtime` only interprets the
# stored resolution (it succeeds even when the manifest can no longer parse).
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mk_provider() { # dir namespace value
    dir=$1
    ns=$2
    value=$3
    mkdir -p "$dir/src" "$dir/runtime"
    printf 'artifact-%s\n' "$ns" > "$dir/runtime/libbackend.fact"
    cat > "$dir/mcpp.toml" <<EOF
[package]
namespace = "$ns"
name = "backend"
version = "$value"

[modules]
sources = ["src/**/*.cppm"]

[targets.backend]
kind = "lib"

[runtime]
provides = ["render.demo"]
artifacts = [
  { role = "library", path = "runtime/libbackend.fact", provenance = "payload", abi = "fixture-v1", digest = "sha256:$ns" },
]
EOF
    cat > "$dir/src/backend.cppm" <<EOF
export module $ns.backend;
export int ${ns}_backend_value() { return ${value%%.*}; }
EOF
}

mk_provider "$TMP/alpha" alpha 2.0.0
mk_provider "$TMP/beta" beta 3.0.0
mkdir -p "$TMP/app/src"
cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name = "app"
version = "1.0.0"

[dependencies.alpha]
backend = { path = "../alpha" }

[dependencies.beta]
backend = { path = "../beta" }

[runtime]
requirements = [
  { kind = "capability", value = "render.demo", phase = "run", required = true },
  { kind = "capability", value = "abi:musl", phase = "link", required = false },
]

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
import alpha.backend;
import beta.backend;
int main() { return alpha_backend_value() + beta_backend_value() == 5 ? 0 : 1; }
EOF

cd "$TMP/app"
"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "runtime contract fixture build failed"
    exit 1
}

RES=$(find target -name resolution.json | head -1)
test -n "$RES" || { echo "resolution.json missing"; exit 1; }
python3 - "$RES" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
runtime = doc["runtime"]
assert doc["schema_version"] == 2, doc
providers = [p for p in runtime["providers"] if p["capability"] == "render.demo"]
ids = [p["provider"]["canonical"] for p in providers]
assert ids == ["alpha.backend@2.0.0", "beta.backend@3.0.0"], ids
assert all(p["provider"]["source"].startswith("path+") for p in providers), providers
requirements = [r for r in runtime["requirements"] if r["value"] == "render.demo"]
assert len(requirements) == 1, requirements
assert requirements[0]["requester"]["canonical"] == "mcpplibs.app@1.0.0", requirements
assert requirements[0]["requester"]["canonical"] not in ids, (requirements, ids)
optional = [r for r in runtime["requirements"] if r["value"] == "abi:musl"]
assert len(optional) == 1 and optional[0]["required"] is False, optional
artifacts = runtime["artifacts"]
assert [a["provider"]["canonical"] for a in artifacts] == ids, artifacts
assert all(a["provenance"] == "payload" for a in artifacts), artifacts
assert runtime["search"]["format"] == "elf", runtime["search"]
assert runtime["validation"]["status"] == "pass", runtime["validation"]
assert runtime["binding"]["contract_hash"], runtime["binding"]
PY

# The runtime explanation must not resolve the now-invalid manifest and must
# not launch familiar graphics diagnostic programs placed at the front of PATH.
cp mcpp.toml mcpp.toml.good
printf '[package\n' > mcpp.toml
mkdir -p fake-bin
for name in vulkaninfo glxinfo nvidia-smi; do
    cat > "fake-bin/$name" <<EOF
#!/usr/bin/env bash
printf '%s\n' '$name' >> '$TMP/probes.log'
exit 99
EOF
    chmod +x "fake-bin/$name"
done
PATH="$PWD/fake-bin:$PATH" "$MCPP" why runtime > why.log 2>&1 || {
    cat why.log
    echo "why runtime re-resolved instead of reading stored facts"
    exit 1
}
mv mcpp.toml.good mcpp.toml
test ! -e "$TMP/probes.log" || {
    cat "$TMP/probes.log"
    echo "mcpp launched a provider-specific probe"
    exit 1
}
grep -q 'alpha.backend@2.0.0' why.log || { cat why.log; exit 1; }
grep -q 'beta.backend@3.0.0' why.log || { cat why.log; exit 1; }
grep -q 'validation: pass' why.log || { cat why.log; exit 1; }
grep -q 'owned by xlings' why.log || { cat why.log; exit 1; }

echo "PASS 207_runtime_contract_provenance"
