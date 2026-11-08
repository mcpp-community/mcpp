#!/usr/bin/env bash
# ninja-edges.sh DIR [SINCE_LINES]: the edges `.ninja_log` files below DIR gained after their
# first SINCE_LINES lines (a snapshot of `ninja-edges.sh --count DIR`), and how many were objects.
set -euo pipefail
if [[ "$1" == --count ]]; then
    find "$2" -name .ninja_log -not -path '*/.build-mcpp/*' -exec wc -l {} + | awk '$2 != "total" { print $2 "\t" $1 }'
    exit 0
fi
dir=$1; base=${2:-}
total=0; objects=0
while IFS= read -r log; do
    skip=1
    [[ -n "$base" ]] && skip=$(awk -F'\t' -v f="$log" '$1 == f { print $2 }' "$base")
    skip=${skip:-1}
    n=$(tail -n +"$((skip + 1))" "$log" | awk -F'\t' 'NF == 5' | wc -l)
    o=$(tail -n +"$((skip + 1))" "$log" | awk -F'\t' 'NF == 5 && $4 ~ /\.o$/' | wc -l)
    total=$((total + n)); objects=$((objects + o))
done < <(find "$dir" -name .ninja_log -not -path '*/.build-mcpp/*')
echo "edges=$total objects=$objects"
