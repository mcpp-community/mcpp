#!/usr/bin/env bash
# requires: python3
# 774 -- a member inside an index package's archive receives its workspace's
# inheritance, as the same commit does from its own checkout and from a git
# clone (#690, design record F11).
#
# The index descriptor points at a member manifest (`mcpp = "*/libs/wlib/mcpp.toml"`)
# inside an archive whose root is a workspace declaring `[workspace.package]
# version` and `[workspace.build] defines`. The member omits `version` and
# guards the define with `#error`. Before #690 the member was loaded as a
# stand-alone manifest and refused for the missing version.
#
# The archive is seeded into the install path, so the test needs no network:
# the resolver accepts an installed tree whose layout matches the descriptor.
set -e

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export MCPP_HOME="$T/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

X="$MCPP_HOME/registry/data/xpkgs/probe774-x-wlib/1.0.0/wrepo-1.0.0"
mkdir -p "$X/libs/wlib" "$T/idx/pkgs/p" "$T/app/src"
printf '[workspace]\nmembers = ["libs/wlib"]\n\n[workspace.package]\nversion = "1.0.0"\n\n[workspace.build]\ndefines = ["REPO_DEF=4"]\n' > "$X/mcpp.toml"
printf '[package]\nnamespace = "probe774"\nname = "wlib"\n\n[targets.wlib]\nkind = "lib"\n\n[build]\nsources = ["w.cpp"]\n' > "$X/libs/wlib/mcpp.toml"
printf '#ifndef REPO_DEF\n#error "the archive workspace did not reach its member"\n#endif\nint w_v() { return REPO_DEF; }\n' > "$X/libs/wlib/w.cpp"
cat > "$T/idx/pkgs/p/probe774.wlib.lua" <<'EOF'
package = {
    spec = "1",
    namespace = "probe774",
    name = "probe774.wlib",
    description = "Form A member inside a workspace archive",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["1.0.0"] = { url = "https://example.invalid/wrepo-1.0.0.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        macosx  = { ["1.0.0"] = { url = "https://example.invalid/wrepo-1.0.0.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        windows = { ["1.0.0"] = { url = "https://example.invalid/wrepo-1.0.0.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
    },
    mcpp = "*/libs/wlib/mcpp.toml",
}
EOF
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[indices]\nprobe774 = { path = "../idx" }\n\n[dependencies.probe774]\nwlib = "1.0.0"\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$T/app/mcpp.toml"
printf 'int w_v();\nint main() { return w_v() == 4 ? 0 : 1; }\n' > "$T/app/src/main.cpp"
cd "$T/app"
"$MCPP" run > run.log 2>&1 || fail "the member inside the archive did not build or run" run.log
cdb=compile_commands.json
n=$(python3 - "$cdb" <<'EOF'
import json, sys
for e in json.load(open(sys.argv[1])):
    if e["file"].replace("\\", "/").endswith("libs/wlib/w.cpp"):
        print(e["arguments"].count("-DREPO_DEF=4")); sys.exit(0)
print("missing")
EOF
)
[ "$n" = 1 ] || fail "-DREPO_DEF=4 occurs $n times in the member's unit"

echo "PASS: 774_an_index_member_inherits_its_archive_workspace"
