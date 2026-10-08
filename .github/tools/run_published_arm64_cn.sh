#!/usr/bin/env bash
# Only released bytes from public CN; backend packages are CI infrastructure.
set -euo pipefail
version=${1:?exact published mcpp version}
repo=${2:?checkout holding these tools and the canonical client pin}
backend=${3:-proot}
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || exit 2
[[ "$(uname -m)" == aarch64 ]] || exit 2
[[ "$backend" == proot || "$backend" == bwrap ]] || exit 2
base=${XLINGS_HOME:?cold isolated home required}
[[ ! -e "$base" ]] || { echo 'home must be cold'; exit 1; }
mkdir -p "$base/bootstrap" "$base/probes" "$base/reports"
cd "$base"
# Derive the client version from the repository's existing single pin.
xl_version=$(python3 - "$repo/src/xlings/xlings.cppm" <<'PY'
import pathlib,re,sys
s=pathlib.Path(sys.argv[1]).read_text()
m=re.search(r'kXlingsVersion\s*=\s*"([0-9.]+)"',s)
assert m,s[:100]
print(m[1])
PY
)
gh api "repos/mcpp-community/mcpp/releases/tags/v$version" > "$base/reports/release.json"
python3 - "$base/reports/release.json" "$version" <<'PY'
import json,sys
r=json.load(open(sys.argv[1])); assert not r['draft'] and r['published_at']
assert r['tag_name']=='v'+sys.argv[2]
PY
# Exact tag source is evidence and supplies the existing real e2e test.
git init -q "$base/reports/release-tag"
git -C "$base/reports/release-tag" remote add origin https://github.com/mcpp-community/mcpp
git -C "$base/reports/release-tag" fetch --depth 1 origin "refs/tags/v$version"
git -C "$base/reports/release-tag" rev-parse 'FETCH_HEAD^{commit}' | tee "$base/reports/release-tag-sha.txt"
archive="xlings-$xl_version-linux-aarch64.tar.gz"
expected=$(gh api "repos/xlings-res/xlings/releases/tags/$xl_version" --jq ".assets[] | select(.name==\"$archive\") | .digest")
[[ "$expected" == sha256:* ]] || { echo 'published client digest absent'; exit 1; }
curl -fL "https://gitcode.com/xlings-res/xlings/releases/download/$xl_version/$archive" -o "$base/bootstrap/$archive"
printf '%s  %s\n' "${expected#sha256:}" "$base/bootstrap/$archive" | sha256sum -c -
tar -xzf "$base/bootstrap/$archive" -C "$base/bootstrap"
xl="$base/bootstrap/xlings-$xl_version-linux-aarch64/bin/xlings"
"$xl" --version | tee "$base/reports/xlings-version.txt"
"$xl" config --mirror CN
"$xl" update
cp "$base/data/xim-pkgindex/.xlings-index-cache.json" "$base/reports/outer-published-index.json"
# Native backend is provided by distro infrastructure, never labeled an xim
# ARM package. The published bwrap package is x86-only. The client locates the
# system proot directly; bwrap's current locator needs this explicit private
# system-backend directory. Record the symlink and actual distro version.
backend_package="$backend"
[[ "$backend" != bwrap ]] || backend_package=bubblewrap
dpkg-query -W "$backend_package" | tee "$base/reports/backend-version.txt"
command -v "$backend" | tee "$base/reports/backend-path.txt"
sha256sum "$(command -v "$backend")" > "$base/reports/backend.sha256"
if [[ "$backend" == bwrap ]]; then
    mkdir -p "$base/data/xpkgs/xim-x-bwrap/ci-system-backend/bin"
    ln -s /usr/bin/bwrap "$base/data/xpkgs/xim-x-bwrap/ci-system-backend/bin/bwrap"
fi
# Independently prove the exact mcpp archive is available through public CN.
mcpp_archive="mcpp-$version-linux-aarch64.tar.gz"
mcpp_digest=$(gh api "repos/xlings-res/mcpp/releases/tags/$version" --jq ".assets[] | select(.name==\"$mcpp_archive\") | .digest")
[[ "$mcpp_digest" == sha256:* ]] || exit 1
curl -fL "https://gitcode.com/xlings-res/mcpp/releases/download/$version/$mcpp_archive" -o "$base/reports/$mcpp_archive"
printf '%s  %s\n' "${mcpp_digest#sha256:}" "$base/reports/$mcpp_archive" | sha256sum -c -
mkdir "$base/reports/mcpp-cn-archive"
tar -xzf "$base/reports/$mcpp_archive" -C "$base/reports/mcpp-cn-archive"
mapfile -t archive_binaries < <(find "$base/reports/mcpp-cn-archive" -path '*/bin/mcpp' -type f)
[[ "${#archive_binaries[@]}" == 1 ]] || exit 1
mcpp_binary_sha=$(sha256sum "${archive_binaries[0]}" | cut -d ' ' -f 1)
printf '%s\n' "$mcpp_binary_sha" > "$base/reports/mcpp-cn-binary.sha256"
"$xl" subos new cn-arm64-published
"$xl" subos runtime glibc@2.44.3 cn-arm64-published
cp "$repo/.github/tools/check_published_arm64_cn_ecosystem.sh" "$base/probes/ecosystem.sh"
printf -v command 'CN_HOST_CHECKOUT=%q CN_EXPECTED_BINARY_SHA=%q CN_PUBLISHED_XLINGS=%q bash %q release %q' "$repo" "$mcpp_binary_sha" "$xl" "$base/probes/ecosystem.sh" "$version"
"$xl" subos use cn-arm64-published --sandbox "$backend" --cmd "$command"
work=$(cat "$base/probes/last-run.txt")
[[ "$work" == "$base/probes/runs/"* ]] || exit 1
cmp "$base/reports/release-tag-sha.txt" "$work/mcpp-source-sha.txt"
cp "$base/reports/release-tag-sha.txt" "$work/cross/engine-release-sha.txt"
cp "$work/mcpp-home/registry/data/xim-pkgindex/.xlings-index-cache.json" "$base/reports/inner-published-index.json"
cp "$work/mcpp-home/registry/.xlings.json" "$base/reports/inner-config.json"
cp "$base/.xlings.json" "$base/reports/outer-config.json"
echo "CN_REPORT=$base/reports" >> "$GITHUB_ENV"
echo "CN_CONSUMER_REPORT=$work" >> "$GITHUB_ENV"
echo "CN_CROSS_ARTIFACTS=$work/cross" >> "$GITHUB_ENV"
