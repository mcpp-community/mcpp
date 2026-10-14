#!/usr/bin/env bash
# requires: elf python3
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
python3 "$ROOT/tests/scripts/test_shell_completion.py"
echo "PASS: shell completion"
