# make_test_fixture <dir> <row-table-text>
#
# A package whose tests measure three things on a device row:
#   runs            - the runner path works at all (exit 0);
#   reads_relative  - a deployed file is readable beside the test executable;
#   reads_host_path - the build machine's absolute path is readable.
# Sourced by the macOS and Android measurement scripts, after common.sh.
make_test_fixture() {
    local d="$1" row="$2"
    rm -rf "$d"; mkdir -p "$d/src" "$d/tests" "$d/share"
    echo "m634-data" > "$d/share/data.txt"
    cat > "$d/mcpp.toml" <<T
[package]
name    = "ttest"
version = "0.1.0"

[targets.ttest]
kind = "bin"
main = "src/main.cpp"

[runtime]
deploy = [ { from = "share/data.txt", to = "data" } ]

$row
T
    printf 'int main() { return 0; }\n' > "$d/src/main.cpp"
    printf 'int main() { return 0; }\n' > "$d/tests/runs.cpp"
    cat > "$d/tests/reads_relative.cpp" <<'C'
#include <cstdio>
#include <cstring>
#include <string>
int main(int, char** argv) {
    std::string p = argv[0];
    auto s = p.rfind('/');
    std::string f = (s == std::string::npos ? std::string(".") : p.substr(0, s)) + "/data/data.txt";
    FILE* fp = std::fopen(f.c_str(), "r");
    if (!fp) { std::printf("open failed: %s\n", f.c_str()); return 3; }
    char buf[64] = {0};
    std::fgets(buf, sizeof buf, fp);
    std::fclose(fp);
    std::printf("read %s: %s", f.c_str(), buf);
    return std::strncmp(buf, "m634-data", 9) == 0 ? 0 : 4;
}
C
    # The build machine's path, written into the source. Only `$d` expands.
    cat > "$d/tests/reads_host_path.cpp" <<C
#include <cstdio>
#include <cstring>
int main() {
    const char* f = "$d/share/data.txt";
    FILE* fp = std::fopen(f, "r");
    if (!fp) { std::printf("open failed: %s\n", f); return 3; }
    char buf[64] = {0};
    std::fgets(buf, sizeof buf, fp);
    std::fclose(fp);
    std::printf("read %s: %s", f, buf);
    return std::strncmp(buf, "m634-data", 9) == 0 ? 0 : 4;
}
C
}

# report_tests <ndjson> <id-prefix>: one reading per test record.
report_tests() {
    python3 - "$1" "$2" <<'PY' > "$1.readings"
import json, sys
path, prefix = sys.argv[1], sys.argv[2]
try:
    lines = open(path).read().splitlines()
except OSError as e:
    print(f"{prefix}.records\tunreadable {e}"); sys.exit(0)
n = 0
for line in lines:
    line = line.strip()
    if not line.startswith("{"):
        continue
    try:
        r = json.loads(line)
    except ValueError:
        continue
    if "summary" in r:
        print(f"{prefix}.summary\t{json.dumps(r['summary'])[:300]}")
        continue
    n += 1
    out = (r.get("run_output") or r.get("compile_output") or r.get("reason") or "")
    out = out.replace("\n", "|").replace("\t", " ")[:240]
    print(f"{prefix}.{r.get('test')}\tstatus={r.get('status')} exit={r.get('exit_code')} out={out}")
if n == 0:
    print(f"{prefix}.records\tnone")
PY
    while IFS=$'\t' read -r id text; do reading "$id" "$text"; done < "$1.readings"
}
