# tests/e2e/_host_path.sh — convert a shell-side path into the spelling the
# mcpp BINARY will understand when it reads that path out of a file.
#
# THE BUG THIS EXISTS TO PREVENT (2026-08-09, Windows E2E 2/2 red for a day):
#
#   TMP=$(mktemp -d)                       # Git Bash → /tmp/tmp.XXXXXXXX
#   cat > mcpp.toml <<EOF
#   [indices]
#   acme = { path = "$TMP/myapp/index" }   # ← written as FILE CONTENT
#   EOF
#
# MSYS rewrites POSIX paths into Windows paths when it hands argv and the
# environment to a native process. It does NOT touch file content. So a native
# mcpp.exe reads the literal `/tmp/tmp.XXXXXXXX/myapp/index`, and Windows reads
# a leading `/` as "root of the current drive" — the fixture resolves to
# `C:\tmp\...`, which does not exist. The failure surfaces as a package that
# "is not in any configured index", four steps away from its cause.
#
# It cost a day because the two obvious ways to reproduce it both lie:
#   * on Linux/macOS the fixture is a real path, so the test passes;
#   * under Wine `Z:` is mapped to `/`, so `\tmp\...` lands back on the real
#     /tmp and the fixture passes there too. Wine can tell you a PE runs. It
#     cannot tell you anything about path SEMANTICS.
#
# Rule for every E2E fixture: a path that is written INTO a manifest, a
# descriptor, or any other file mcpp will parse must go through host_path
# first, and the result must be stored in a variable whose name ends in
# `_HOST`. `00_fixture_path_hygiene.sh` enforces both halves.
#
# `cygpath -m` (mixed mode, `C:/Users/...`) rather than `-w`: Windows accepts
# forward slashes everywhere, and unlike backslashes they need no escaping
# inside a TOML basic string.

host_path() {
    # A relative path carries no drive/root semantics, so both spellings already
    # agree and Windows accepts `/` as a separator. Passing one to `cygpath -m`
    # would silently ANCHOR it at the current directory — which is not where the
    # manifest being written lives — so relative input is returned untouched.
    case "$1" in
        /*) ;;
        *) printf '%s' "$1"; return 0 ;;
    esac
    case "$(uname -s)" in
        MINGW* | MSYS* | CYGWIN*)
            command -v cygpath >/dev/null 2>&1 || {
                echo "FATAL: cygpath is required to build Windows fixtures" >&2
                return 1
            }
            cygpath -m "$1"
            ;;
        *)
            printf '%s' "$1"
            ;;
    esac
}
