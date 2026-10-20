# Shared native-Windows and MinGW-cross resource ownership checks (#826).
set -e
source "$(dirname "${BASH_SOURCE[0]}")/_host_path.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MCPP_HOME="$TMP/home"
source "$(dirname "${BASH_SOURCE[0]}")/_inherit_toolchain.sh"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do cat "$f" 2>/dev/null; done; exit 1; }
cd "$TMP"
mkdir -p app updater library
printf '[workspace]\nmembers = ["app", "updater", "library"]\n' > mcpp.toml
cat > app/mcpp.toml <<'TOML'
[package]
namespace = "fixture"
name = "app"
version = "0.1.0"
[dependencies]
"fixture.updater" = {path = "../updater", artifacts = ["updater"]}
"fixture.library" = {path = "../library"}
[resources]
files = ["owner.rc"]
extra-inputs = ["owner.manifest"]
[targets.image]
kind = "bin"
main = "main.cpp"
TOML
printf 'int main() { return 0; }\n' > app/main.cpp
cp app/main.cpp updater/main.cpp
cat > updater/mcpp.toml <<'TOML'
[package]
namespace = "fixture"
name = "updater"
version = "0.1.0"
[resources]
files = ["owner.rc"]
extra-inputs = ["owner.manifest"]
[targets.updater]
kind = "bin"
main = "main.cpp"
windows_code_page = "legacy"
TOML
cat > library/mcpp.toml <<'TOML'
[package]
namespace = "fixture"
name = "library"
version = "0.1.0"
[build]
sources = ["lib.cpp"]
[resources]
files = ["owner.rc"]
extra-inputs = ["owner.manifest"]
[targets.image]
kind = "shared"
TOML
printf 'extern "C" __declspec(dllexport) int library_value() { return 42; }\n' > library/lib.cpp
for owner in app updater library; do
    printf '1 24 "owner.manifest"\n' > "$owner/owner.rc"
    printf '<?xml version="1.0" encoding="UTF-8"?>\n<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"><assemblyIdentity version="1.0.0.0" name="Fixture.Owner.%s" type="win32"/></assembly>\n' "$owner" > "$owner/owner.manifest"
done
# The app and library targets share a name; it cannot determine ownership.
"$MCPP" build -p app $RESOURCE_TARGET > app.log 2>&1 || fail 'dependency images' app.log
check_images() {
    python3 - "$1" <<'PY'
import pathlib, struct, sys
root = pathlib.Path('target')
expected = {'app': 'Fixture.Owner.app', 'updater': 'Fixture.Owner.updater', 'library': 'Fixture.Owner.library'}
seen = set()
for f in root.rglob('*'):
    if f.name not in ('image.exe', 'image.dll', 'libimage.dll', 'updater.exe'): continue
    b = f.read_bytes()
    if b[:2] != b'MZ': continue
    pe = struct.unpack_from('<I', b, 60)[0]
    count = struct.unpack_from('<H', b, pe + 6)[0]
    section = pe + 24 + struct.unpack_from('<H', b, pe + 20)[0]
    data = b''
    for i in range(count):
        h = section + 40*i
        if b[h:h+8].rstrip(b'\0') == b'.rsrc':
            size, off = struct.unpack_from('<II', b, h + 16)
            data = b[off:off+size]
    path = str(f).replace('\\', '/')
    if f.suffix == '.dll': owner = 'library'
    elif f.name == 'updater.exe': owner = 'updater'
    elif '/bin/app/' in path or '/bin/fixture.app/' in path: owner = 'app'
    else: continue
    # Deployed artifacts may share the app's product directory. The primary
    # updater lives in obj/fixture_updater or its own product directory.
    assert expected[owner].encode() in data, (owner, str(f), 'missing own manifest')
    for other, marker in expected.items():
        if other != owner: assert marker.encode() not in data, (owner, other, str(f))
    seen.add(owner)
assert seen == set(expected), seen
PY
}
check_images first || fail 'resources do not follow their package' app.log
# Direct selection reuses the same sources and must preserve ownership.
"$MCPP" build -p updater $RESOURCE_TARGET > direct.log 2>&1 || fail 'direct updater' direct.log
check_images direct || fail 'direct and dependency images differ' direct.log
# A tracked manifest change must reach the linked artifact and deployment.
python3 - <<'PY'
from pathlib import Path
p = Path('updater/owner.manifest')
p.write_text(p.read_text().replace('Fixture.Owner.updater', 'Fixture.Owner.updater.updated'))
PY
"$MCPP" build -p app $RESOURCE_TARGET > changed.log 2>&1 || fail 'manifest rebuild' changed.log
python3 - <<'PY' || fail 'artifact manifest stayed stale' changed.log
from pathlib import Path
files = list(Path('target').glob('*/**/obj/fixture_updater/updater.exe'))
assert files and all(b'Fixture.Owner.updater.updated' in f.read_bytes() for f in files), files
PY
echo 'PASS: dependency EXE and DLL embed only their own resources and rebuild'
