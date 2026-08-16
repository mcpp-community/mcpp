#!/usr/bin/env python3
"""Resolve an MSI's File table to full install paths, offline.

Why: the VS channel manifest says nothing about what is *inside* a payload,
and cabinet entries are keyed names (filXXXX), so "does this MSI ship rc.exe,
and where does it land?" is only answerable from the MSI's own tables.

The schema is read from !_Columns rather than assumed -- the SDK's MSIs do
not all use the same column widths, and a guessed width silently misaligns
every column after it instead of failing.
"""
import struct, subprocess, sys, tempfile, pathlib, shutil

STRING = 0x0800          # MSITYPE_STRING


def streams(msi):
    d = pathlib.Path(tempfile.mkdtemp())
    subprocess.run(["7z", "x", f"-o{d}", str(msi), "-y"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return d


def string_pool(d):
    pool = (d / "!_StringPool").read_bytes()
    data = (d / "!_StringData").read_bytes()
    n = len(pool) // 4
    codepage = struct.unpack_from("<HH", pool, 0)[1]
    long_refs = bool(codepage & 0x8000)          # >64K strings -> 4-byte refs
    out, off, i = [""], 0, 1
    while i < n:
        size, refs = struct.unpack_from("<HH", pool, i * 4)
        if size == 0 and refs != 0 and i + 1 < n:  # large string: len spans 2 entries
            nsize, _ = struct.unpack_from("<HH", pool, (i + 1) * 4)
            size = (refs << 16) | nsize
            i += 1
        out.append(data[off:off + size].decode("cp1252", "replace"))
        off += size
        i += 1
    return out, long_refs


def _width(coltype, long_refs):
    if coltype & STRING:
        return 4 if long_refs else 2
    return 4 if (coltype & 0xFF) >= 4 else 2


def _decode(raw, coltypes, strings, long_refs):
    """MSI tables are column-major; every column is fixed width."""
    widths = [_width(t, long_refs) for t in coltypes]
    rows = len(raw) // sum(widths)
    cols, off = [], 0
    for t, w in zip(coltypes, widths):
        vals = []
        for r in range(rows):
            v = int.from_bytes(raw[off + r * w: off + (r + 1) * w], "little")
            if t & STRING:
                vals.append(strings[v] if v < len(strings) else "")
            else:                                # integers are stored sign-biased
                vals.append(v ^ (0x80000000 if w == 4 else 0x8000))
        cols.append(vals)
        off += rows * w
    return list(zip(*cols))


def schema(d, strings, long_refs):
    """_Columns has a fixed shape, and describes every other table."""
    raw = (d / "!_Columns").read_bytes()
    sw = 4 if long_refs else 2
    rows = len(raw) // (sw + 2 + sw + 2)
    out, off = {}, 0
    def take(w, isstr):
        nonlocal off
        vals = []
        for r in range(rows):
            v = int.from_bytes(raw[off + r * w: off + (r + 1) * w], "little")
            vals.append(strings[v] if isstr and v < len(strings) else (v ^ 0x8000))
        off += rows * w
        return vals
    tabs, nums, names, types = take(sw, True), take(2, False), take(sw, True), take(2, False)
    for t, n, nm, ty in zip(tabs, nums, names, types):
        out.setdefault(t, []).append((n, nm, ty))
    return {t: [c for _, _, c in sorted(v)] for t, v in out.items()}, \
           {t: [nm for _, nm, _ in sorted(v)] for t, v in out.items()}


def table(d, name, sch, strings, long_refs):
    f = d / f"!{name}"
    if not f.exists() or name not in sch:
        return []
    return _decode(f.read_bytes(), sch[name], strings, long_refs)


def longname(n):
    n = n if isinstance(n, str) else ""
    return n.split("|")[-1] if "|" in n else n


def resolve(msi):
    """-> list of (full_install_path, filename)"""
    d = streams(msi)
    try:
        strings, lr = string_pool(d)
        sch, _ = schema(d, strings, lr)
        files = table(d, "File", sch, strings, lr)
        comps = table(d, "Component", sch, strings, lr)
        dirs = table(d, "Directory", sch, strings, lr)

        parent = {r[0]: r[1] for r in dirs}
        dname = {r[0]: longname(r[2]) for r in dirs}
        compdir = {r[0]: r[2] for r in comps}

        def full(key, depth=0):
            if not key or depth > 32 or key not in dname:
                return ""
            here = "" if dname[key] in (".", "") else dname[key]
            up = full(parent.get(key), depth + 1)
            return f"{up}/{here}".strip("/") if up else here

        return [(full(compdir.get(r[1], "")), longname(r[2])) for r in files]
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main(msi, *needles):
    rows = resolve(msi)
    hits = 0
    for path, name in rows:
        if not needles or any(n.lower() == name.lower() for n in needles):
            print(f"  {path or '?'}/{name}")
            hits += 1
    if needles and not hits:
        print("  (none of " + ", ".join(needles) + ")")
    if not needles:
        print(f"  total {len(rows)} files")


if __name__ == "__main__":
    main(sys.argv[1], *sys.argv[2:])
