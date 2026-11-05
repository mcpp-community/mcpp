#!/usr/bin/env bash
# requires: gcc unix-shell
# A library used by build.mcpp may supply a host layer. Its direct or
# transitive host-only instance cannot select the final artifact's ABI.
set -e
MCPP="${MCPP:-mcpp}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export MCPP_HOME="$work/home"
source "$(dirname "$0")/_inherit_toolchain.sh"
mkdir -p "$work/app/src" "$work/provider/src" "$work/adapter/src"
cd "$work"
cat > provider/mcpp.toml <<'TOML'
[package]
name = "host-provider"
version = "0.1.0"
provides = ["mcpp:kernel-abi=openkal"]
[targets.host-provider]
kind = "lib"
TOML
cat > provider/src/provider.cppm <<'CPP'
export module host.provider;
export int host_probe(){return 7;}
CPP
cat > adapter/mcpp.toml <<'TOML'
[package]
name = "host-adapter"
version = "0.1.0"
[dependencies]
host-provider = { path = "../provider" }
[targets.host-adapter]
kind = "lib"
TOML
cat > adapter/src/adapter.cppm <<'CPP'
export module host.adapter;
import host.provider;
export int adapter_probe(){return host_probe();}
CPP
cat > app/mcpp.toml <<'TOML'
[package]
name = "target-consumer"
version = "0.1.0"
[build-dependencies]
host-provider = { path = "../provider", host-module = true }
TOML
cat > app/build.mcpp <<'CPP'
import host.provider;
int main(){return host_probe()==7?0:1;}
CPP
cat > app/src/main.cpp <<'CPP'
#ifdef __OPENKAL__
#error host_kernel_layer_leaked_to_target
#endif
int main(){return 0;}
CPP
cd app
"$MCPP" run > out.log 2>&1 || { cat out.log; echo 'FAIL: direct host layer leaked'; exit 1; }
sed 's/host-provider = { path = "..\/provider"/host-adapter = { path = "..\/adapter"/' mcpp.toml > changed.toml
mv changed.toml mcpp.toml
cat > build.mcpp <<'CPP'
import host.adapter;
int main(){return adapter_probe()==7?0:1;}
CPP
"$MCPP" run > out.log 2>&1 || { cat out.log; echo 'FAIL: transitive host layer leaked'; exit 1; }

# The same package reached on both axes retains a genuine target instance.
cat >> mcpp.toml <<'TOML'
[dependencies]
host-provider = { path = "../provider" }
TOML
cat > src/main.cpp <<'CPP'
#ifndef __OPENKAL__
#error target_kernel_layer_was_withdrawn
#endif
int main(){return 0;}
CPP
"$MCPP" run > out.log 2>&1 || { cat out.log; echo 'FAIL: target instance lost its layer'; exit 1; }
echo 'PASS: host-only layers do not select the target ABI'
