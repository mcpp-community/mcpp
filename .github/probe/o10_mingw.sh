#!/usr/bin/env bash
# O10: the island conditions on PE. A DLL built by MinGW g++ (libstdc++) is
# used by programs built by llvm-mingw clang++ (libc++). Prints; never asserts.
set -u
W=$(mktemp -d); cd "$W"
GCCBIN=/c/msys64/ucrt64/bin
/c/msys64/usr/bin/bash -lc "pacman -S --noconfirm --needed mingw-w64-ucrt-x86_64-gcc" >/dev/null 2>&1
"$GCCBIN/g++" --version | head -1
url=$(curl -fsSL -H "Authorization: Bearer ${GITHUB_TOKEN:-}" https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest \
      | grep -o '"browser_download_url": *"[^"]*ucrt-x86_64\.zip"' | head -1 | sed 's/.*"\(http[^"]*\)"/\1/')
echo "llvm-mingw: $url"
curl -fsSL -o lm.zip "$url" && unzip -q lm.zip && LM=$(ls -d "$W"/llvm-mingw-*/ | head -1)
CLANG="$LM/bin/x86_64-w64-mingw32-clang++"
"$CLANG" --version | head -1
OBJDUMP="$LM/bin/llvm-objdump"

cat > b.cpp <<'EOF'
#include <stdexcept>
#include <string>
extern "C" __declspec(dllexport) int b_checked(int v) {
    try { if (v < 0) throw std::runtime_error("negative"); return v * 2; }
    catch (const std::exception&) { return -1; }
}
__declspec(dllexport) int b_throws(int v) {
    if (v < 0) throw std::runtime_error("negative from B (libstdc++)");
    return v;
}
EOF
cat > a.cpp <<'EOF'
#include <stdexcept>
__declspec(dllexport) int a_throws(int v) {
    if (v < 0) throw std::runtime_error("negative from A (libc++)");
    return v;
}
EOF
cat > use_c.cpp <<'EOF'
#include <cstdio>
#include <stdexcept>
extern "C" int b_checked(int);
int main() {
    int r = b_checked(-1);
    try { throw std::logic_error("local"); } catch (const std::exception& e) { std::printf("local typed catch ok: %s\n", e.what()); }
    std::printf("b_checked(-1) = %d (expect -1)\n", r);
    return r == -1 ? 0 : 1;
}
EOF
cat > use_cxx.cpp <<'EOF'
#include <cstdio>
#include <stdexcept>
int b_throws(int);
int main() {
    try { b_throws(-1); }
    catch (const std::exception& e) { std::printf("typed catch across runtimes: %s\n", e.what()); return 0; }
    catch (...) { std::printf("only catch(...) caught it\n"); return 2; }
    return 1;
}
EOF
cat > use_a.cpp <<'EOF'
#include <cstdio>
#include <stdexcept>
extern "C" int b_checked(int);
int a_throws(int);
int main() {
    int r = b_checked(-1);
    try { a_throws(-1); }
    catch (const std::exception& e) { std::printf("typed catch across OUR two images: %s; b_checked=%d\n", e.what(), r); return 0; }
    catch (...) { std::printf("only catch(...)\n"); return 2; }
    return 1;
}
EOF
export PATH="$LM/bin:$GCCBIN:$PATH"
"$GCCBIN/g++" -O1 -shared b.cpp -o B.dll -Wl,--out-implib,libB.dll.a; echo "O10 B.dll (g++/libstdc++) rc=$?"
"$OBJDUMP" -p B.dll | grep "DLL Name" | sed 's/^/  B imports: /'

run() { local label=$1; shift; "$@"; echo "O10 $label rc=$?"; }
imports() { "$OBJDUMP" -p "$1" | grep "DLL Name" | tr -d '\t' | sed "s/^/  $1 imports: /"; }

run "V1 build (C interface, shared libc++)" "$CLANG" use_c.cpp -L. -lB -o v1.exe
imports v1.exe; run "V1 run" ./v1.exe
run "V2 build (C++ interface, typed catch across runtimes)" "$CLANG" use_cxx.cpp -L. -lB -o v2.exe
imports v2.exe; run "V2 run" ./v2.exe
run "V3 A.dll (libc++)" "$CLANG" -shared a.cpp -o A.dll -Wl,--out-implib,libA.dll.a
run "V3 build (two of our images + B)" "$CLANG" use_a.cpp -L. -lA -lB -o v3.exe
imports v3.exe; run "V3 run" ./v3.exe
run "V4 build (static libc++ + B)" "$CLANG" -static-libstdc++ -static use_c.cpp -L. -lB -o v4.exe
imports v4.exe; run "V4 run" ./v4.exe
# The #838 shape on PE: libstdc++'s import library named on the link line
# BEFORE the driver appends libc++. Which runtime do __cxa_throw and the
# personality bind to, and does the local typed catch survive?
GL=/c/msys64/ucrt64/lib
run "V5 build (-l:libstdc++.dll.a before libc++)" "$CLANG" use_c.cpp -L. -lB -L$GL -l:libstdc++.dll.a -o v5.exe
imports v5.exe
"$OBJDUMP" -p v5.exe | awk '/DLL Name: libstdc\+\+-6.dll/{f=1;next} /DLL Name/{f=0} f' | head -20 | sed 's/^/  v5 from libstdc++-6.dll: /'
run "V5 run" ./v5.exe
run "V6 build (-l:libstdc++.dll.a AFTER libc++)" "$CLANG" use_c.cpp -L. -lB -lc++ -L$GL -l:libstdc++.dll.a -o v6.exe
imports v6.exe; run "V6 run" ./v6.exe
