#!/usr/bin/env sh
# embed.sh <input> <output.cpp> <symbol-prefix>
# Emits a C++ file exposing the input's bytes as `<prefix>_data()`.
set -eu
in="$1"; out="$2"; sym="$3"
mkdir -p "$(dirname "$out")"
{
    printf 'extern "C" const char* %s_data() {\n    return\n' "$sym"
    # One string literal per line keeps the output readable and avoids any
    # length limit a single literal would hit.
    while IFS= read -r line || [ -n "$line" ]; do
        printf '        "%s\\n"\n' "$line"
    done < "$in"
    printf '    ;\n}\n'
} > "$out"
