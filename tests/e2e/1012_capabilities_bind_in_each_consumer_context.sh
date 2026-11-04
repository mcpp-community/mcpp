#!/usr/bin/env bash
# requires: gcc unix-shell python3
# Independent products may select different providers. A library requirement
# still accepts a provider supplied by its final application's sibling edge.
set -e
MCPP="${MCPP:-mcpp}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export MCPP_HOME="$work/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
cd "$work"
fail() { echo "FAIL: $*"; cat out.log; exit 1; }
cat > mcpp.toml <<'TOML'
[workspace]
members = ["app-a", "app-b"]
TOML
for suffix in a b; do
    mkdir -p "provider-$suffix/src" "app-$suffix/src"
    cat > "provider-$suffix/mcpp.toml" <<EOF
[package]
name = "provider-$suffix"
version = "0.1.0"
provides = ["probe-interface"]
exclusive = ["probe-interface"]
[targets.provider-$suffix]
kind = "lib"
EOF
    echo "int provider_$suffix(void){return 0;}" > "provider-$suffix/src/anchor.c"
    cat > "app-$suffix/mcpp.toml" <<EOF
[package]
name = "app-$suffix"
version = "0.1.0"
[dependencies]
provider-$suffix = { path = "../provider-$suffix" }
helper = { path = "../helper" }
[targets.app-$suffix]
kind = "bin"
main = "src/main.cpp"
EOF
    echo 'int main(){return 0;}' > "app-$suffix/src/main.cpp"
done
mkdir -p helper/src
cat > helper/mcpp.toml <<'TOML'
[package]
name = "helper"
version = "0.1.0"
[features]
default = ["need-provider"]
need-provider = { requires = ["probe-interface"] }
[targets.helper]
kind = "lib"
TOML
echo 'int helper_anchor(void){return 0;}' > helper/src/anchor.c
"$MCPP" build --workspace > out.log 2>&1 || fail 'independent exclusive providers collided'
"$MCPP" run -p app-a > out.log 2>&1 || fail 'app-a did not run'
"$MCPP" run -p app-b > out.log 2>&1 || fail 'app-b did not run'

# Test link units and a shared provider keep the same per-consumer boundary.
for suffix in a b; do
    mkdir -p "app-$suffix/tests"
    echo 'int main(){return 0;}' > "app-$suffix/tests/context.cpp"
done
sed 's/kind = "lib"/kind = "shared"/' provider-a/mcpp.toml > changed.toml
mv changed.toml provider-a/mcpp.toml
"$MCPP" test --workspace > out.log 2>&1 || fail 'test/shared contexts crossed independent products'
"$MCPP" run -p app-a > out.log 2>&1 || fail 'shared provider consumer did not run'

# A provider elsewhere in the workspace cannot satisfy a missing dependency.
cp app-a/mcpp.toml app-a/saved.toml
sed '/^provider-a =/d' app-a/saved.toml > app-a/mcpp.toml
if "$MCPP" build --workspace > out.log 2>&1; then fail 'another product supplied a missing provider'; fi
grep -q "no package provides capability 'probe-interface'" out.log || fail 'wrong missing-provider diagnosis'
grep -q "consumer 'app-a'" out.log || fail 'missing consumer context'
# Host-only providers cannot satisfy the missing target-side requirement.
cat >> app-a/mcpp.toml <<'TOML'
[build-dependencies]
provider-a = { path = "../provider-a" }
TOML
echo 'int main(){return 0;}' > app-a/build.mcpp
if "$MCPP" build --workspace > out.log 2>&1; then fail 'a host-only provider satisfied a target requirement'; fi
grep -q "no package provides capability 'probe-interface'" out.log || fail 'wrong host/target scope diagnosis'
rm app-a/build.mcpp
cp app-a/saved.toml app-a/mcpp.toml

# A pin cannot remove the second implementation from one actual link closure.
cat >> app-a/mcpp.toml <<'TOML'
[capabilities]
probe-interface = "provider-a"
TOML
sed '/^helper =/a provider-b = { path = "../provider-b" }' app-a/mcpp.toml > changed.toml
mv changed.toml app-a/mcpp.toml
if "$MCPP" build --workspace > out.log 2>&1; then fail 'pin hid an exclusive collision'; fi
grep -qi exclusive out.log || fail 'wrong exclusive diagnosis'
grep -q "consumer 'app-a'" out.log || fail 'exclusive diagnosis omitted consumer'

# Without exclusivity both providers may coexist; the member pin resolves its
# requirement while the other member binds implicitly to its own provider.
for suffix in a b; do
    sed '/^exclusive =/d' "provider-$suffix/mcpp.toml" > changed.toml
    mv changed.toml "provider-$suffix/mcpp.toml"
done
"$MCPP" build --workspace > out.log 2>&1 || fail 'member pin did not resolve local ambiguity'
"$MCPP" build --workspace --cap probe-interface=provider-b > out.log 2>&1 \
    || fail 'CLI pin did not override the member pin'
if "$MCPP" build --workspace --cap probe-interface=provider-a > out.log 2>&1; then
    fail 'CLI pin escaped the second consumer closure'
fi
grep -q "consumer 'app-b'" out.log || fail 'invalid pin omitted its consumer'

# Qualified provider identities distinguish packages sharing a short name.
mkdir -p namespaced/src
for ns in first second; do
    mkdir -p "$ns/src"
    cat > "$ns/mcpp.toml" <<EOF
[package]
name = "implementation"
namespace = "$ns"
version = "0.1.0"
provides = ["namespaced-interface"]
[targets.implementation]
kind = "lib"
EOF
    echo "int ${ns}_anchor(void){return 0;}" > "$ns/src/anchor.c"
done
cat > namespaced/mcpp.toml <<'TOML'
[package]
name = "namespaced-consumer"
version = "0.1.0"
[features]
default = ["need"]
need = { requires = ["namespaced-interface"] }
[dependencies.first]
implementation = { path = "../first" }
[dependencies.second]
implementation = { path = "../second" }
[capabilities]
namespaced-interface = "implementation"
TOML
echo 'int main(){return 0;}' > namespaced/src/main.cpp
cd namespaced
if "$MCPP" build > out.log 2>&1; then fail 'ambiguous short provider identity was accepted'; fi
grep -q 'first:implementation' out.log || fail 'missing first qualified candidate'
grep -q 'second:implementation' out.log || fail 'missing second qualified candidate'
sed 's/= "implementation"$/= "first:implementation"/' mcpp.toml > changed.toml
mv changed.toml mcpp.toml
"$MCPP" build > out.log 2>&1 || fail 'qualified provider pin did not bind'

echo 'PASS: capabilities bind in each consumer context'
