# COFF fixtures

`probe-amd64.obj` is a real amd64 COFF object, produced by mingw-cross GCC 16.1
from `probe-amd64.cpp.in`:

```
x86_64-w64-mingw32-g++ -c probe-amd64.cpp.in -o probe-amd64.obj -O1
```

It is committed rather than generated because the parser it exercises
(`mcpp.build.coff_exports`) must be correct on hosts that cannot produce COFF at
all — macOS CI has no mingw, and that is exactly where a byte-level reader is
most likely to be wrong and least likely to be noticed.

The `.cpp.in` extension is not decoration: `mcpp test` discovers `tests/**/*.cpp`
and would compile a `.cpp` here into a test binary with no `main`.

Its external defined symbols, per `x86_64-w64-mingw32-nm --defined-only
--extern-only`:

```
0000000000000000 R exported_const
0000000000000000 D exported_data
0000000000000000 T exported_fn
0000000000000004 T _Z3usev
000000000000000a T _ZN2ns10mangled_fnEi
```
