# Repository rules that let bazel reach the two things it cannot glob: the
# pinned xlings tree and mcpp's package registry.
#
# WHY ANY OF THIS EXISTS. bazel will not read sources from outside its
# workspace, and the xlings arm needs two kinds of outside:
#
#   1. THE TREE UNDER MEASUREMENT. The harness names it with --project and
#      exports BENCH_PROJECT_ROOT (bench/src/main.cpp), exactly as the cmake and
#      xmake arms consume it. A `bazel build //...` must build THAT tree and no
#      other: declaring both pinned submodules as ordinary targets in this
#      package would make every cell measure two builds and report one.
#
#   2. THE DEPENDENCIES, which mcpp resolves into ~/.mcpp/registry as SOURCE.
#      Nine C/C++ libraries and four C++23 module packages, none of them in this
#      repository, none of them with a bazel description upstream.
#
# Both are `repository_rule`s because that is the one place bazel lets you name
# an absolute path and compute it from the environment. Everything downstream is
# ordinary cc_library/cc_binary.
#
# ⚠️ THE SOURCE LISTS ARE DERIVED, NOT COPIED. Every package in mcpp's registry
# carries a `.xpkg.lua` manifest naming the EXACT files, include dirs and flags
# mcpp compiles it with, and this file parses that manifest instead of restating
# it. Globbing would be wrong and quietly so:
#
#     libarchive-3.8.7/libarchive/*.c   132 files; the manifest names 127
#     lua-5.4.7/src/*.c                  34 files; the manifest names  32
#
# and the two extra lua files are `lua.c` and `luac.c`, each with its own
# `main()` — a glob links the interpreter into xlings and fails on a duplicate
# symbol, or worse, does not. The same reasoning as
# bench/projects/xlings/embed_lua_stdlib.cmake: a rule cannot drift from itself,
# a copy of its output can.

# ---------------------------------------------------------------------------
# A Lua-table reader, just big enough for `.xpkg.lua`.
#
# These manifests are hand-written Lua with line comments, single- and
# double-quoted strings and nested tables. Anything that scans them has to skip
# comments BEFORE it looks at quotes, because the comments contain apostrophes
# ("-- xmake's mbedtls package") and a scanner that sees `'` first reads the
# rest of the file as one string and silently returns nothing.
# ---------------------------------------------------------------------------

_IDENT = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"

def _skip_ws(s, i):
    for _ in range(len(s) + 1):
        if i >= len(s):
            return i
        if s[i] == " " or s[i] == "\t" or s[i] == "\n" or s[i] == "\r":
            i += 1
        elif s[i:i + 2] == "--":
            nl = s.find("\n", i)
            i = len(s) if nl < 0 else nl + 1
        else:
            return i
    return i

def _skip_string(s, i, where):
    quote = s[i]
    i += 1
    for _ in range(len(s) + 1):
        if i >= len(s):
            fail("unterminated string in " + where)
        if s[i] == "\\":
            i += 2
        elif s[i] == quote:
            return i + 1
        else:
            i += 1
    fail("unterminated string in " + where)

def _match_table(s, start, where):
    """s[start] must be '{'. Returns the index just past the matching '}'."""
    depth = 0
    i = start
    for _ in range(len(s) + 1):
        if i >= len(s):
            fail("unbalanced table in " + where)
        if s[i:i + 2] == "--":
            nl = s.find("\n", i)
            i = len(s) if nl < 0 else nl + 1
        elif s[i] == '"' or s[i] == "'":
            i = _skip_string(s, i, where)
        elif s[i] == "{":
            depth += 1
            i += 1
        elif s[i] == "}":
            depth -= 1
            i += 1
            if depth == 0:
                return i
        else:
            i += 1
    fail("unbalanced table in " + where)

def _find_value(s, key, where):
    """Index of the value of `key = ...`, or -1. Comments and strings skipped."""
    i = 0
    for _ in range(len(s) + 1):
        if i >= len(s):
            return -1
        if s[i:i + 2] == "--":
            nl = s.find("\n", i)
            i = len(s) if nl < 0 else nl + 1
            continue
        if s[i] == '"' or s[i] == "'":
            i = _skip_string(s, i, where)
            continue
        if s[i:i + len(key)] == key:
            end = i + len(key)
            before_ok = i == 0 or s[i - 1] not in _IDENT
            after_ok = end >= len(s) or s[end] not in _IDENT
            if before_ok and after_ok:
                after = _skip_ws(s, end)
                if after < len(s) and s[after] == "=" and s[after + 1:after + 2] != "=":
                    return _skip_ws(s, after + 1)
        i += 1
    return -1

def _unescape(s):
    out = ""
    i = 0
    for _ in range(len(s) + 1):
        if i >= len(s):
            break
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            out += "\n" if c == "n" else ("\t" if c == "t" else c)
            i += 2
        else:
            out += s[i]
            i += 1
    return out

def _string_list(s, start, where):
    """Every string literal directly inside the table at s[start]."""
    end = _match_table(s, start, where)
    body = s[start + 1:end - 1]
    out = []
    i = 0
    for _ in range(len(body) + 1):
        if i >= len(body):
            break
        if body[i:i + 2] == "--":
            nl = body.find("\n", i)
            i = len(body) if nl < 0 else nl + 1
        elif body[i] == '"':
            j = _skip_string(body, i, where)
            out.append(_unescape(body[i + 1:j - 1]))
            i = j
        else:
            i += 1
    return out

def _field_strings(seg, key, where):
    at = _find_value(seg, key, where)
    if at < 0 or seg[at] != "{":
        return []
    return _string_list(seg, at, where)

def _field_scalar(seg, key, where):
    at = _find_value(seg, key, where)
    if at < 0 or seg[at] != '"':
        return ""
    end = _skip_string(seg, at, where)
    return _unescape(seg[at + 1:end - 1])

def _read_manifest(rctx, verdir, inner, label):
    """The `mcpp = { ... }` segment of a package's .xpkg.lua, or None.

    None means the descriptor says nothing about how to compile the package —
    either it has no `mcpp` key at all (tinyhttps, xpkg) or it points at the
    package's own manifest (`mcpp = "*/mcpp.toml"`). Both mean the same thing:
    the package ships an mcpp.toml and mcpp reads its source set off the default
    convention, src/**/*.{cppm,cpp}.

    That mcpp.toml is checked to EXIST rather than assumed, because "no build
    information anywhere" and "build information mcpp reads from a file this
    parser does not" look identical from here and produce very different builds.
    """
    path = rctx.path(verdir + "/.xpkg.lua")
    if not path.exists:
        fail("no .xpkg.lua for {}: mcpp's registry has not unpacked {}. Run a ".format(label, verdir) +
             "`mcpp build` of the xlings tree first; this arm builds what mcpp resolved, " +
             "it does not resolve packages itself.")

    # watch = "no": the registry is outside the workspace and bazel refuses to
    # watch paths it does not own. The cost is that a registry change needs
    # `bazel fetch --force`; the alternative is not fetching at all.
    text = rctx.read(path, watch = "no")
    at = _find_value(text, "mcpp", label)
    if at >= 0 and text[at] == "{":
        return text[at:_match_table(text, at, label)]
    if not rctx.path(verdir + "/" + inner + "/mcpp.toml").exists:
        fail("{}: .xpkg.lua carries no inline `mcpp = {{...}}` segment and the package ".format(label) +
             "ships no mcpp.toml either, so nothing here says which files to compile")
    return None

# ---------------------------------------------------------------------------
# Path shapes in the registry.
# ---------------------------------------------------------------------------

def _xpkgs_root(rctx):
    home = rctx.os.environ.get("MCPP_HOME")
    if not home:
        userhome = rctx.os.environ.get("HOME") or rctx.os.environ.get("USERPROFILE")
        if not userhome:
            fail("neither MCPP_HOME nor HOME is set; cannot find mcpp's registry")
        home = userhome + "/.mcpp"
    return home + "/registry/data/xpkgs"

def _inner_dir(rctx, verdir, label):
    """What `*` means in a manifest path.

    Every package is a tarball unpacked one level below the version directory —
    `compat-x-ftxui/6.1.9/FTXUI-6.1.9/` — and the manifests write that wrap layer
    as a leading `*/`. `include_dirs` needs a concrete directory (bazel's
    `includes` attribute takes literal paths, not patterns), so it is resolved
    here rather than left as a glob: the version directory holds exactly one
    subdirectory besides mcpp's own `mcpp_generated/`.
    """
    dirs = [
        p.basename
        for p in rctx.path(verdir).readdir()
        if p.is_dir and p.basename != "mcpp_generated"
    ]
    if len(dirs) != 1:
        fail("{}: expected one unpacked tree under {}, found {}".format(label, verdir, dirs))
    return dirs[0]

# bazel's own marker files, which the repo rule must not import from a source
# tarball. See _link_package.
_PACKAGE_MARKERS = ["BUILD", "BUILD.bazel", "REPO.bazel", "MODULE.bazel", "WORKSPACE", "WORKSPACE.bazel"]

def _link_package(rctx, verdir, inner, name):
    """Mirror one registry package into this repo, minus bazel's marker files.

    ⚠️ THE INNER TREE IS LINKED CHILD BY CHILD, AND THAT IS THE WHOLE POINT.
    zlib and FTXUI both ship an upstream BUILD.bazel — both are in the Bazel
    Central Registry — and one `rctx.symlink(verdir, name)` brings it along.
    A stray BUILD file makes that directory a bazel PACKAGE, which is invisible
    everywhere except in the errors it causes somewhere else:

        glob pattern 'ftxui/FTXUI-6.1.9/src/ftxui/**/*.cpp' didn't match anything
        Label '...//:zlib/zlib-1.3.2/adler32.c' is invalid because
          '...//zlib/zlib-1.3.2' is a subpackage

    Neither message names a BUILD file. Deferring to the vendored descriptions
    instead is not an option: they compile a different source set with different
    flags than mcpp does, and "both engines compile the same code" is the only
    thing that makes this arm a measurement.

    Only DIRECTORIES are linked at the version level: it also holds the source
    tarball the package was unpacked from, and a 30 MB archive bazel has to
    stat and hash is not an input to anything.
    """
    for entry in rctx.path(verdir).readdir():
        if entry.is_dir and entry.basename != inner:
            rctx.symlink(entry, name + "/" + entry.basename)
    for entry in rctx.path(verdir + "/" + inner).readdir():
        if entry.basename not in _PACKAGE_MARKERS:
            rctx.symlink(entry, name + "/" + inner + "/" + entry.basename)

# ---------------------------------------------------------------------------
# Manifest paths -> bazel labels.
# ---------------------------------------------------------------------------

def _resolve_star(pattern, inner):
    """A leading `*` segment is the unpacked tree; other wildcards stay globs."""
    if pattern == "*":
        return inner
    if pattern.startswith("*/"):
        return inner + pattern[1:]
    return pattern

def _split_sources(rctx, verdir, prefix, inner, patterns, label):
    """(literal labels, glob patterns, glob exclusions).

    Patterns with no wildcard become literal file names and are checked to
    exist HERE, at fetch time, with the package that is missing them named. A
    glob would drop them silently and the build would fail hundreds of files
    later on an undefined symbol.
    """
    literals = []
    globs = []
    excludes = []
    for pattern in patterns:
        negated = pattern.startswith("!")
        rel = _resolve_star(pattern[1:] if negated else pattern, inner)
        if negated:
            excludes.append(prefix + "/" + rel)
        elif "*" in rel:
            globs.append(prefix + "/" + rel)
        else:
            if not rctx.path(verdir + "/" + rel).exists:
                fail("{}: .xpkg.lua names {} but it is not in the unpacked tree".format(label, rel))
            literals.append(prefix + "/" + rel)
    return literals, globs, excludes

def _copts(cflags, standard):
    """mcpp's cflags are shell words, and so are bazel's copts.

    ⚠️ THE BACKSLASHES IN libarchive's
    `-DPLATFORM_CONFIG_H=\\"mcpp_libarchive_config.h\\"` ARE LOAD-BEARING AND MUST
    SURVIVE. `copts` is documented as subject to "Bourne shell tokenization", so
    an unescaped `"` is eaten exactly as a shell would eat it and the compiler
    receives

        -DPLATFORM_CONFIG_H=mcpp_libarchive_config.h

    Nothing complains. `#include PLATFORM_CONFIG_H` then finds no header, every
    HAVE_* stays undefined, and the build fails 60 files later with 20 errors of
    the form `call to undeclared library function 'strcmp'` in
    archive_write_set_format_zip.c — a file that has nothing to do with it. The
    "helpful" unescaping this function used to do is what produced that.

    Splitting on spaces is redundant with the same tokenization, and kept
    because it makes `-include mcpp_lua_platform_config.h` visibly two flags
    rather than one that bazel happens to split.
    """
    out = []
    if standard:
        out.append("-std=" + standard)
    for flag in cflags:
        for piece in flag.split(" "):
            if piece:
                out.append(piece)
    return out

# ---------------------------------------------------------------------------
# The dependency set, in xlings' own resolution order.
#
# VERSIONS ARE PINNED FROM xlings' mcpp.toml AND ITS TRANSITIVE MANIFESTS, never
# "the newest directory in the registry": the registry holds several versions at
# once and taking the last one would mean the bazel arm compiles different code
# than the mcpp arm it is being compared against.
#
# The deps edges below are the one thing NOT derived from the manifests: mcpp
# resolves `compat.zlib = "1.3.2"` to a package, bazel needs a label, and the
# mapping between the two is this file. They are checked by the link.
# ---------------------------------------------------------------------------

_C_LIBS = [
    # name          xpkg                    version   language  deps
    ("zlib", "compat-x-zlib", "1.3.2", "c", []),
    ("bzip2", "compat-x-bzip2", "1.0.8", "c", []),
    ("lz4", "compat-x-lz4", "1.10.0", "c", []),
    ("zstd", "compat-x-zstd", "1.5.7", "c", []),
    ("xz", "compat-x-xz", "5.8.3", "c", []),
    # libarchive's own manifest names these five; its generated config header
    # sets HAVE_LIBZ/HAVE_LIBLZ4/... unconditionally, so they are link-time
    # requirements rather than options.
    ("libarchive", "compat-x-libarchive", "3.8.7", "c", ["zlib", "bzip2", "lz4", "zstd", "xz"]),
    ("lua", "compat-x-lua", "5.4.7", "c", []),
    ("mbedtls", "compat-x-mbedtls", "3.6.1", "c", []),
    ("ftxui", "compat-x-ftxui", "6.1.9", "c++", []),
]

# lz4hc.c does `#include "lz4.c"` under LZ4_COMMONDEFS_ONLY to pull in the
# static helpers. Both files are compiled in their own right, so lz4.c is a
# source AND a textual header — bazel needs it named in both places or the
# compile of lz4hc.c fails with `'lz4.c' file not found`, since a sibling source
# is not an input to a compile action.
_TEXTUAL_SOURCES = {
    "lz4": ["lib/lz4.c", "lib/xxhash.c"],
}

# The C++23 module packages. `deps` here are bazel labels in this same repo;
# the module graph itself (who imports whom) is discovered by bazel's ddi
# scanner, so only the package-level edges have to be written down.
_MODULE_LIBS = [
    ("cmdline", "mcpplibs-x-cmdline", "0.0.2", ["std"]),
    ("capi_lua", "mcpplibs.capi-x-lua", "0.0.3", ["std", "lua"]),
    ("tinyhttps", "mcpplibs-x-tinyhttps", "0.2.9", ["std", "mbedtls"]),
    ("xpkg", "mcpplibs-x-xpkg", "0.0.57", ["std", "capi_lua"]),
]

_HDR_PATTERNS = ["*.h", "*.hh", "*.hpp", "*.hxx", "*.inc", "*.def", "*.ipp"]

def _lit(values):
    # Escaped, because the values are compiler flags and one of them is
    # libarchive's -DPLATFORM_CONFIG_H="mcpp_libarchive_config.h". Emitted raw it
    # closes the Starlark string early and the generated BUILD file fails to
    # parse with `syntax error at 'mcpp_libarchive_config': expected ]` — an
    # error that names the flag's contents and nothing about quoting.
    return "[" + ", ".join([
        '"{}"'.format(v.replace("\\", "\\\\").replace('"', '\\"'))
        for v in values
    ]) + "]"

def _glob(patterns, exclude = [], allow_empty = False):
    out = "glob({}".format(_lit(patterns))
    if exclude:
        out += ", exclude = {}".format(_lit(exclude))
    if allow_empty:
        out += ", allow_empty = True"
    return out + ")"

def _join(*parts):
    """`a + b` over the non-empty pieces, so the generated file has no `[] + `."""
    return " + ".join([p for p in parts if p]) or "[]"

def _hdrs_glob(prefix):
    return _glob([prefix + "/**/" + p for p in _HDR_PATTERNS], allow_empty = True)

# ---------------------------------------------------------------------------
# @mcpp_deps — everything xlings links that is not xlings.
# ---------------------------------------------------------------------------

def _libcxx_root(rctx, xpkgs):
    """The libc++ that goes with the compiler being measured.

    DERIVED FROM $CC, not globbed. The registry can hold llvm 20.1.7 and 22.1.8
    at once; compiling libc++'s std.cppm out of one of them while the driver
    ships the headers of the other is the two-standard-libraries failure that
    ../common/cmake/hermetic_payload.cmake documents, and it surfaces as an
    error inside <format> rather than as a version mismatch.
    """
    cc = rctx.os.environ.get("CC", "")
    if cc:
        root = cc.rsplit("/bin/", 1)[0] if "/bin/" in cc else ""
        if root and rctx.path(root + "/share/libc++/v1/std.cppm").exists:
            return root
        fail(
            "CC={} has no libc++ std module at <root>/share/libc++/v1/std.cppm ".format(cc) +
            "(CC must be an absolute path to a <root>/bin/ compiler).\n" +
            "bazel has no counterpart to CMake's CXX_MODULE_STD: `import std;` is " +
            "supplied here by compiling libc++'s own std.cppm, so the compiler must " +
            "be a clang that ships it. bazel also cannot build modules with GCC at " +
            "all (its ddi aggregator rejects GCC's P1689 output), which is why this " +
            "arm is clang-only.",
        )
    llvm = rctx.path(xpkgs + "/xim-x-llvm")
    if llvm.exists:
        versions = sorted([p.basename for p in llvm.readdir() if p.is_dir])
        for version in reversed(versions):
            if rctx.path(xpkgs + "/xim-x-llvm/" + version + "/share/libc++/v1/std.cppm").exists:
                return xpkgs + "/xim-x-llvm/" + version
    fail("no clang with a libc++ std module in {}; set CC to one".format(xpkgs))

def _c_lib_rule(rctx, xpkgs, name, xpkg, version, language, deps):
    verdir = "{}/{}/{}".format(xpkgs, xpkg, version)
    label = "{} {}".format(xpkg, version)
    inner = _inner_dir(rctx, verdir, label)
    segment = _read_manifest(rctx, verdir, inner, label)
    if segment == None:
        fail("{}: expected an inline `mcpp = {{...}}` manifest naming its sources".format(label))
    _link_package(rctx, verdir, inner, name)

    sources = _field_strings(segment, "sources", label)
    if not sources:
        fail("{}: .xpkg.lua names no sources".format(label))
    literals, globs, excludes = _split_sources(rctx, verdir, name, inner, sources, label)

    includes = [name + "/" + _resolve_star(d, inner) for d in _field_strings(segment, "include_dirs", label)]

    # `c_standard = "c11"` and `language = "c++23"` are already the -std spelling,
    # so they are used verbatim rather than looked up in a table. A table that
    # does not know a value has to choose between failing and defaulting, and the
    # default is the dangerous half: compiling lua at the compiler's default
    # standard instead of the manifest's c99 is a build that works and is not the
    # one mcpp ran.
    key = "language" if language == "c++" else "c_standard"
    standard = _field_scalar(segment, key, label)
    if not standard:
        fail("{}: .xpkg.lua names no `{}`, so this arm would compile it at the ".format(label, key) +
             "compiler's default standard rather than mcpp's")
    copts = _copts(_field_strings(segment, "cflags", label), standard)

    # `-x c` because bazel's autoconfigured toolchain has ONE compiler for both
    # languages and this arm has to point it at clang++ (the C driver's config
    # file carries no libc++ include chain, so C++ would compile against the
    # host's libstdc++ headers and die in std.cppm on a missing <__config>).
    # clang++ then reads a .c file as C++, which libarchive does not survive.
    # mcpp has the same split and solves it by spawning the sibling `clang`.
    if language == "c":
        copts = ["-x", "c"] + copts

    if excludes and not globs:
        fail("{}: exclusion patterns with no glob to exclude from".format(label))
    srcs = _join(_lit(literals) if literals else "", _glob(globs, excludes) if globs else "")

    textual = _TEXTUAL_SOURCES.get(name, [])
    rule = [
        "cc_library(",
        '    name = "{}",'.format(name),
        "    srcs = {},".format(srcs),
        "    hdrs = {},".format(_hdrs_glob(name)),
    ]
    if textual:
        for rel in textual:
            if not rctx.path("{}/{}/{}".format(verdir, inner, rel)).exists:
                fail("{}: _TEXTUAL_SOURCES names {} and the tree has no such file".format(label, rel))
        rule.append("    textual_hdrs = {},".format(
            _lit(["{}/{}/{}".format(name, inner, rel) for rel in textual]),
        ))
    rule += [
        "    includes = {},".format(_lit(includes)),
        "    copts = {},".format(_lit(copts)),
        "    deps = {},".format(_lit([":" + d for d in deps])),
        "    linkstatic = True,",
        ")",
    ]
    return "\n".join(rule)

def _module_lib_rule(rctx, xpkgs, name, xpkg, version, deps, extra_interfaces = []):
    verdir = "{}/{}/{}".format(xpkgs, xpkg, version)
    label = "{} {}".format(xpkg, version)
    inner = _inner_dir(rctx, verdir, label)
    segment = _read_manifest(rctx, verdir, inner, label)
    _link_package(rctx, verdir, inner, name)

    # Four packages, THREE manifest shapes, and the difference is not cosmetic:
    #   capi.lua  names its two files outright ("*/src/capi/lua.cppm", ".cpp")
    #   cmdline   names a pattern         ("*/src/**/*.cppm")
    #   xpkg, tinyhttps  carry `mcpp = "*/mcpp.toml"`, deferring to the
    #             package's own manifest, whose source set is mcpp's default
    #             convention (src/**/*.{cppm,cpp}).
    # Interface units and implementation units go to different attributes, so
    # every shape has to be sorted by extension rather than by position.
    base = "{}/{}/src".format(name, inner)
    if segment:
        sources = _field_strings(segment, "sources", label)
        literals, globs, excludes = _split_sources(rctx, verdir, name, inner, sources, label)
        if excludes:
            fail("{}: exclusions in a module package are not handled".format(label))
        includes = [name + "/" + _resolve_star(d, inner) for d in _field_strings(segment, "include_dirs", label)] or [base]
        for pattern in globs:
            if not pattern.endswith(".cppm") and not pattern.endswith(".cpp"):
                fail("{}: source pattern {} names no extension, so it cannot be sorted into ".format(label, pattern) +
                     "module_interfaces vs srcs")
    else:
        literals = []
        globs = [base + "/**/*.cppm", base + "/**/*.cpp"]
        includes = [base]

    interfaces = [f for f in literals if f.endswith(".cppm")]
    interface_globs = [p for p in globs if p.endswith(".cppm")]
    srcs_literals = [f for f in literals if not f.endswith(".cppm")]
    src_globs = [p for p in globs if not p.endswith(".cppm")]

    named = interfaces + extra_interfaces
    interface_expr = _join(
        _lit(named) if named else "",
        _glob(interface_globs) if interface_globs else "",
    )

    # allow_empty on the IMPLEMENTATION units only: a module package with no
    # .cpp is ordinary (tinyhttps and xpkg are interface-only), a module package
    # with no .cppm is a resolution that went wrong.
    srcs = _join(
        _lit(srcs_literals) if srcs_literals else "",
        _glob(src_globs, allow_empty = True) if src_globs else "",
    )

    return "\n".join([
        "cc_library(",
        '    name = "{}",'.format(name),
        "    srcs = {},".format(srcs),
        "    hdrs = {},".format(_hdrs_glob(name)),
        "    module_interfaces = {},".format(interface_expr),
        "    includes = {},".format(_lit(includes)),
        '    copts = ["-std=c++23"],',
        # See :module_sources — a clang BMI re-opens the sources it was built
        # from, so every compile that loads one needs them staged.
        '    additional_compiler_inputs = [":module_sources"],',
        "    deps = {},".format(_lit([":" + d for d in deps])),
        "    linkstatic = True,",
        ")",
    ])

# Every C++ source a BMI in this repo was built from.
#
# ⚠️ NOT A CONVENIENCE. A clang BMI records the absolute-ish paths of its input
# files and re-opens them whenever a dependent loads it, so a compile that
# imports `mcpplibs.xpkg` needs xpkg's .cppm on disk even though it only reads
# the .pcm. bazel stages declared inputs and nothing else, and the modmap
# declares .pcm files, so the compile dies with
#     fatal error: cannot open file '.../xpkg.cppm': No such file or directory
# naming a file that is right there in the execroot. Every target that can load
# a BMI from this repo lists this filegroup.
def _module_sources_rule(module_names):
    return "\n".join([
        "filegroup(",
        '    name = "module_sources",',
        "    srcs = {} + [\":xpkg_lua_stdlib\"],".format(
            _glob([n + "/**/*.cppm" for n in module_names] + ["libcxx_module/**"], allow_empty = True),
        ),
        ")",
    ])

# `mcpplibs.xpkg.lua_stdlib` is not a checked-in file: libxpkg's build.mcpp
# generates it, embedding every .lua under src/lua-stdlib as a string_view named
# after the file. That is small and fully specified, so "mcpp runs a build
# program" is not by itself a boundary — a genrule reproduces it, and both arms
# then compile the same set of translation units.
#
# The list is DERIVED (a glob of the directory, which IS the contract build.mcpp
# implements) rather than copied. embed_lua_stdlib.cmake records what a copy
# cost: a regex that caught ten of eleven files failed three files away as
# `error: 'base64_lua' is not a member of ...detail`.
_LUA_STDLIB_GENRULE = '''
genrule(
    name = "xpkg_lua_stdlib",
    srcs = {srcs},
    outs = ["generated/xpkg-lua-stdlib.cppm"],
    cmd = """
set -eu
{{
  echo '// Generated by bench/projects/xlings/mcpp_registry.bzl - do not edit.'
  echo '// Mirrors what libxpkg build.mcpp produces; edit the .lua sources.'
  echo 'module;'
  echo 'export module mcpplibs.xpkg.lua_stdlib;'
  echo 'import std;'
  echo ''
  echo 'export namespace mcpplibs::xpkg::detail {{'
  echo ''
  for f in $(SRCS); do
    stem=$$(basename "$$f" .lua)
    printf 'inline const std::string_view %s_lua = R"XLUA(' "$$stem"
    cat "$$f"
    printf ')XLUA";\\n\\n'
  done
  echo '}} // namespace mcpplibs::xpkg::detail'
}} > $@
""",
)
'''

_STD_RULE = '''
# `import std;` HAS NO BAZEL SPELLING. There is no counterpart to CMake's
# CXX_MODULE_STD, and bazel's own modmap generator fails with
#     ERROR: Module not found: std
# What makes it work anyway is that libc++ ships the std module as ORDINARY
# SOURCE, so it compiles like any other interface unit. The 110 .inc files it
# textually includes have to be inputs too, and they resolve relative to
# std.cppm's own directory, which is why they are listed rather than reached
# through `includes`.
#
# -Wno-reserved-module-identifier: naming a module `std` is reserved to the
# implementation, and libc++ is the implementation.
cc_library(
    name = "std",
    srcs = glob(["libcxx_module/std/**"]),
    module_interfaces = ["libcxx_module/std.cppm"],
    copts = ["-std=c++23", "-Wno-reserved-module-identifier"],
    linkstatic = True,
)
'''

def _mcpp_deps_impl(rctx):
    xpkgs = _xpkgs_root(rctx)
    if not rctx.path(xpkgs).exists:
        fail("mcpp's registry is not at {} — set MCPP_HOME".format(xpkgs))

    rctx.symlink(_libcxx_root(rctx, xpkgs) + "/share/libc++/v1", "libcxx_module")

    parts = [
        "# GENERATED by //:mcpp_registry.bzl from the .xpkg.lua manifests in",
        "# mcpp's registry. Do not edit; edit the rule.",
        'load("@rules_cc//cc:defs.bzl", "cc_library")',
        '',
        'package(default_visibility = ["//visibility:public"])',
        _STD_RULE,
    ]
    for name, xpkg, version, language, deps in _C_LIBS:
        parts.append(_c_lib_rule(rctx, xpkgs, name, xpkg, version, language, deps))

    lua_stdlib_verdir = None
    for name, xpkg, version, deps in _MODULE_LIBS:
        extra = []
        if name == "xpkg":
            lua_stdlib_verdir = "{}/{}/{}".format(xpkgs, xpkg, version)
            extra = [":xpkg_lua_stdlib"]
        parts.append(_module_lib_rule(rctx, xpkgs, name, xpkg, version, deps, extra))

    inner = _inner_dir(rctx, lua_stdlib_verdir, "mcpplibs-x-xpkg")
    stdlib_dir = "xpkg/{}/src/lua-stdlib".format(inner)
    if not rctx.path(lua_stdlib_verdir + "/" + inner + "/src/lua-stdlib").exists:
        fail("libxpkg has no src/lua-stdlib; mcpplibs.xpkg.lua_stdlib cannot be reproduced")
    parts.append(_LUA_STDLIB_GENRULE.format(
        srcs = _glob([stdlib_dir + "/**/*.lua"]),
    ))
    parts.append(_module_sources_rule([name for name, _, _, _ in _MODULE_LIBS]))

    rctx.file("BUILD.bazel", "\n".join(parts) + "\n")

mcpp_deps = repository_rule(
    implementation = _mcpp_deps_impl,
    doc = "xlings' dependency set, compiled from the source mcpp resolved into ~/.mcpp/registry.",
    environ = ["MCPP_HOME", "HOME", "USERPROFILE", "CC"],
)

# ---------------------------------------------------------------------------
# @xlings_tree — the tree under measurement.
# ---------------------------------------------------------------------------

_XLINGS_BUILD = '''# GENERATED by //:mcpp_registry.bzl for {root}
load("@rules_cc//cc:defs.bzl", "cc_binary")

# BOTH .cppm AND .cpp, which is what lets one description measure xlings' two
# code styles:
#
#   2026.8.11.2   110 .cppm +  2 .cpp   implementation inside the interface unit
#   2026.8.13.1   110 .cppm + 92 .cpp   interface and implementation split
#
# `srcs = ["src/main.cpp"]` alone is the trap: against the split tree it
# compiles 110 interfaces, links nothing, and still reports a time. Same rule
# mcpp infers from its own manifest, same rule ../CMakeLists.txt globs.
cc_binary(
    name = "xlings",
    srcs = glob(["src/**/*.cpp"]) + glob(["src/**/*.h", "src/**/*.hpp"], allow_empty = True),
    module_interfaces = glob(["src/**/*.cppm"]),
    # ⚠️ THE INTERFACE SOURCES ARE ALSO INPUTS TO EVERY COMPILE, and they have to
    # be said twice. A clang BMI records the paths of the sources it was built
    # from and re-opens them when a dependent loads it, so compiling the module
    # implementation unit src/runtime/event_stream.cpp loads event_stream's BMI,
    # which loads cancellation's BMI, which reaches for
    #     fatal error: cannot open file '.../src/runtime/cancellation.cppm'
    # bazel stages only declared inputs, and the modmap declares .pcm files.
    #
    # THE SANDBOX IS WHAT MAKES THIS VISIBLE, not what makes it wrong:
    # `--spawn_strategy=local` builds and links this target clean, because the
    # execroot happens to have every source in it. That is an undeclared
    # dependency either way, and the one form of it a benchmark cannot tolerate —
    # it decides whether a cell builds at all.
    additional_compiler_inputs = glob(["src/**/*.cppm"]) + ["@mcpp_deps//:module_sources"],
    # [build] include_dirs — src/libs/json.cppm reaches for <json.hpp> from its
    # global module fragment.
    includes = ["src/libs/json"],
    # [build] cxxflags
    defines = ["LIBARCHIVE_STATIC", "UNICODE", "_UNICODE"],
    copts = ["-std=c++23"],
    deps = [
        "@mcpp_deps//:std",
        "@mcpp_deps//:cmdline",
        "@mcpp_deps//:capi_lua",
        "@mcpp_deps//:tinyhttps",
        "@mcpp_deps//:xpkg",
        "@mcpp_deps//:ftxui",
        "@mcpp_deps//:libarchive",
    ],
    visibility = ["//visibility:public"],
)
'''

def _xlings_tree_impl(rctx):
    # BENCH_PROJECT_ROOT is what bench/src/main.cpp exports for every --project
    # run; the default is the newer pin so a bare `bazel build //...` in this
    # directory still builds something real.
    root = rctx.os.environ.get("BENCH_PROJECT_ROOT")
    if not root:
        root = str(rctx.workspace_root) + "/" + rctx.attr.default_tree
    if not rctx.path(root + "/mcpp.toml").exists:
        fail(
            "no xlings tree at {}: BENCH_PROJECT_ROOT must name a checkout with a ".format(root) +
            "mcpp.toml. The pinned trees are the submodules " +
            "bench/projects/xlings/xlings-<version>/ — run `git submodule update --init`.",
        )
    if not rctx.path(root + "/src").exists:
        fail("{} has no src/".format(root))
    rctx.symlink(root + "/src", "src")
    rctx.file("BUILD.bazel", _XLINGS_BUILD.format(root = root))

xlings_tree = repository_rule(
    implementation = _xlings_tree_impl,
    doc = "The pinned xlings checkout named by --project / BENCH_PROJECT_ROOT.",
    attrs = {"default_tree": attr.string(mandatory = True)},
    environ = ["BENCH_PROJECT_ROOT"],
)
