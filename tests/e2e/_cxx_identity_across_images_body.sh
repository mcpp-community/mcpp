# tests/e2e/_cxx_identity_across_images_body.sh -- the measurement body shared
# by 704 (Mach-O) and 705 (PE). Sourced, not run: the caller sets `set -e`,
# `TMP`, `fail`, and `IMAGE_FORMAT` (a label for the READING lines).
#
# WHAT IS MEASURED (#646 F2, #649 E10). Each image that embeds a private copy
# of the C++ runtime holds its own type information and its own error
# categories. Whether a `std::runtime_error` thrown in a shared library is
# caught by its class in the program, and whether an `std::error_code` made in
# the library compares equal to an `std::errc` in the program, depends on how
# the platform compares those identities across images. libc++ documents
# address comparison for such types on Apple platforms; MSVC matches catch
# clauses by decorated name. This body builds one fixture under three runtime
# statements and PRINTS what each one does. It does not fail on the value it
# reads: the decision taken from the reading is recorded in the design record,
# not here. It fails only when the default leg cannot be built or run, since
# then nothing was measured.

mkdir -p "$TMP/fw/src" "$TMP/app/src"
cat > "$TMP/fw/mcpp.toml" <<'TOML'
[package]
name    = "fw"
version = "0.1.0"

[targets.fw]
kind = "shared"
TOML
cat > "$TMP/fw/src/fw.cppm" <<'CPP'
export module fw;
import std;
export struct fw_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
export [[gnu::visibility("default")]] void fw_throw_standard();
export [[gnu::visibility("default")]] void fw_throw_own();
export [[gnu::visibility("default")]] std::error_code fw_code();
CPP
cat > "$TMP/fw/src/fw.cpp" <<'CPP'
module fw;
import std;
void fw_throw_standard() { throw std::runtime_error("from fw"); }
void fw_throw_own() { throw fw_error("own"); }
std::error_code fw_code() { return std::make_error_code(std::errc::no_such_file_or_directory); }
CPP
cat > "$TMP/app/src/main.cpp" <<'CPP'
import std;
import fw;
int main() {
    const char* standard = "not-matched";
    try { fw_throw_standard(); }
    catch (const std::runtime_error&) { standard = "caught"; }
    catch (...) {}
    const char* own = "not-matched";
    try { fw_throw_own(); }
    catch (const fw_error&) { own = "caught"; }
    catch (...) {}
    const bool equal = fw_code() == std::errc::no_such_file_or_directory;
    std::println("runtime_error={} own_error={} errc={}", standard, own,
                 equal ? "equal" : "unequal");
}
CPP

measure_leg() {   # $1 = leg name, $2 = [build] lines
    cat > "$TMP/app/mcpp.toml" <<TOML
[package]
name    = "app"
version = "0.1.0"

[build]
$2

[dependencies]
fw = { path = "../fw" }
TOML
    (
        cd "$TMP/app"
        rm -rf target
        if ! "$MCPP" build > "build-$1.log" 2>&1; then
            echo "READING $IMAGE_FORMAT $1: build-failed $(grep -m1 -E 'error' "build-$1.log" | cut -c1-160)"
            exit 3
        fi
        local exe
        exe=$(find target -type f \( -name app -o -name app.exe \) -path '*bin*' | head -1)
        local out
        if out=$("$exe" 2>&1); then
            echo "READING $IMAGE_FORMAT $1: $out"
        else
            echo "READING $IMAGE_FORMAT $1: run-failed exit=$? $out"
            exit 4
        fi
        local contracts
        contracts=$(tr -d ' \n\r' < "$(find target -name resolution.json | head -1)" 2>/dev/null \
            | grep -o '"cxx_runtime_by_role":{[^}]*}' || true)
        echo "READING $IMAGE_FORMAT $1 record: ${contracts:-none}"
    )
}

set +e
measure_leg default ''
default_rc=$?
measure_leg host-coupled 'cxx_runtime = "host-coupled"'
measure_leg shared-host-coupled 'cxx_runtime = { shared = "host-coupled" }'
set -e
[ "$default_rc" -eq 0 ] || fail "the default leg could not be measured (rc $default_rc)" "$TMP/app/build-default.log"
