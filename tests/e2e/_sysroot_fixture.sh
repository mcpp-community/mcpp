#!/usr/bin/env bash
# _sysroot_fixture.sh — a glibc sysroot built from what this home already has.
#
# A test of a stated target base needs a directory that IS one: a C library in
# a sysroot layout, foreign to mcpp. Downloading a distribution's would make
# the suite depend on a mirror, so the fixture assembles one from the
# ecosystem's own glibc and linux-headers payloads, in crosstool-ng's layout
# (`usr/include`, `usr/lib64`, `lib64 -> usr/lib64`), with the linker scripts'
# absolute paths made sysroot-relative -- the shape a vendor sysroot has. It
# copies only the top-level libraries and objects, not the locale data.
#
# Usage:  source "$(dirname "$0")/_sysroot_fixture.sh"
#         make_glibc_sysroot <dest>     # 0 on success, 1 when no payload is installed
#         SYSROOT_GLIBC                 # the payload's version ("2.44"), set on success

_sysroot_store="${MCPP_HOME:-${HOME}/.mcpp}/registry/data/xpkgs"

make_glibc_sysroot() {
    local dest="$1" g h
    g=$(ls -d "$_sysroot_store"/xim-x-glibc/*/ 2>/dev/null | sort -V | tail -1)
    h=$(ls -d "$_sysroot_store"/xim-x-linux-headers/*/ 2>/dev/null | sort -V | tail -1)
    [[ -n "$g" && -n "$h" && -d "$g/include" ]] || return 1
    g=${g%/}; h=${h%/}
    rm -rf "$dest"
    mkdir -p "$dest/usr/include" "$dest/usr/lib64" "$dest/lib" "$dest/usr/lib"
    ln -sfn usr/lib64 "$dest/lib64"
    cp -a "$g/include/." "$h/include/." "$dest/usr/include/"
    find "$g/lib/" -maxdepth 1 \( -type f -o -type l \) -exec cp -a {} "$dest/usr/lib64/" \;
    chmod -R u+w "$dest"
    local f
    for f in $(grep -lI "GROUP\|INPUT" "$dest"/usr/lib64/*.so 2>/dev/null); do
        sed -i "s#$g/lib64/#/lib64/#g; s#$g/lib/#/lib64/#g" "$f"
    done
    SYSROOT_GLIBC=$(sed -n 's/^#define[[:space:]]*__GLIBC_MINOR__[[:space:]]*\([0-9]*\).*/2.\1/p' \
                    "$dest/usr/include/features.h" | head -1)
    return 0
}
