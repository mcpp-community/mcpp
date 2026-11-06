#!/usr/bin/env bash
# requires: python3
# 1016 -- a component of `mcpp emit sbom` names the package, not the spelling
# of the manifest that declared it. The purl is what correlates a component with
# an advisory feed, so one package has one purl.
#
# The command reads the lock and nothing else, so the lock is written by hand
# here: no toolchain, no index and no network.
#
# Criteria:
#   A. `cmdline` and `mcpplibs.cmdline` are one name and one purl, and a
#      transitive `compat.zlib` is not `pkg:mcpp/compat/compat.zlib`.
#   B. A variant keeps its digest and stays another component.
#   C. A purl percent-encodes `+`; the `version` field stays as recorded.
#   D. An entry with no namespace (a git dependency) keeps its name whole.
#   E. A lock that holds one identity under two spellings gives one component.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

cat > mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0+7"
TOML

cat > mcpp.lock <<'TOML'
version = 2

[package."mcpplibs.cmdline"]
namespace = "mcpplibs"
version = "0.0.1"
source  = "index+mcpplibs@0.0.1"
hash    = "fnv1a:0000000000000001"

[package."compat.zlib"]
namespace = "compat"
version = "1.3.2"
source  = "index+compat@1.3.2"
hash    = "fnv1a:0000000000000002"

[package."compat.zlib+pabcdef012345"]
namespace = "compat"
version = "1.3.2"
source  = "index+compat@1.3.2"
hash    = "fnv1a:0000000000000003"
variant = "abcdef0123456789"
patches = ["patches/0001-fix.patch#sha256:00"]

[package."fmt"]
namespace = "mcpplibs"
version = "11.0.0+7"
source  = "index+mcpplibs@11.0.0+7"
hash    = "fnv1a:0000000000000004"

[package."acme.tool"]
version = "1.0.0"
source  = "git+https://example.invalid/acme/tool#tag=v1.0.0"
hash    = "fnv1a:0000000000000005"
TOML

"$MCPP" emit sbom -o sbom.json >/dev/null 2>&1 || { echo "FAIL: mcpp emit sbom"; exit 1; }
python3 - <<'PY' || exit 1
import json
d = json.load(open("sbom.json"))
got = {(c.get("group", ""), c["name"]): c for c in d["components"]}
want = {
    ("mcpplibs", "cmdline"): "pkg:mcpp/mcpplibs/cmdline@0.0.1",
    ("compat", "zlib"): "pkg:mcpp/compat/zlib@1.3.2",
    ("compat", "zlib+pabcdef012345"): "pkg:mcpp/compat/zlib%2Bpabcdef012345@1.3.2",
    ("mcpplibs", "fmt"): "pkg:mcpp/mcpplibs/fmt@11.0.0%2B7",
    ("", "acme.tool"): "pkg:mcpp/mcpp/acme.tool@1.0.0",
}
assert set(got) == set(want), sorted(got)
for key, purl in want.items():
    assert got[key]["purl"] == purl, (key, got[key]["purl"])
    assert got[key]["bom-ref"] == purl, (key, got[key]["bom-ref"])
assert got[("mcpplibs", "fmt")]["version"] == "11.0.0+7", got[("mcpplibs", "fmt")]
assert "pedigree" in got[("compat", "zlib+pabcdef012345")]
root = d["metadata"]["component"]
assert root["purl"] == "pkg:mcpp/mcpp/app@0.1.0%2B7", root["purl"]
assert root["version"] == "0.1.0+7", root["version"]
PY
echo "  ok  a component states the package's identity, and its purl is encoded"

# The bare spelling of the package gives the purl the qualified spelling gave.
cat > mcpp.lock <<'TOML'
version = 2

[package."cmdline"]
namespace = "mcpplibs"
version = "0.0.1"
source  = "index+mcpplibs@0.0.1"
hash    = "fnv1a:0000000000000001"
TOML
"$MCPP" emit sbom -o bare.json >/dev/null 2>&1 || { echo "FAIL: mcpp emit sbom (bare)"; exit 1; }
python3 - <<'PY' || exit 1
import json
one = {c["purl"] for c in json.load(open("bare.json"))["components"]}
all_ = {c["purl"] for c in json.load(open("sbom.json"))["components"]}
assert one == {"pkg:mcpp/mcpplibs/cmdline@0.0.1"}, one
assert one <= all_, (one, all_)
PY
echo "  ok  a bare and a qualified spelling are one purl"

# A lock from 2026.9.29.1 to .5 can hold one identity twice.
cat >> mcpp.lock <<'TOML'

[package."mcpplibs.cmdline"]
namespace = "mcpplibs"
version = "0.0.1"
source  = "index+mcpplibs@0.0.1"
hash    = "fnv1a:0000000000000001"
TOML
"$MCPP" emit sbom -o twice.json >/dev/null 2>&1 || { echo "FAIL: mcpp emit sbom (twice)"; exit 1; }
python3 - <<'PY' || exit 1
import json
refs = [c["bom-ref"] for c in json.load(open("twice.json"))["components"]]
assert refs == ["pkg:mcpp/mcpplibs/cmdline@0.0.1"], refs
PY
echo "  ok  one identity under two spellings is one component"

echo "PASS: the bill of materials states a package's identity"
