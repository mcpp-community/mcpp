#!/usr/bin/env bash
# requires: python3
# Real CLI processes prove persistence, one hop, repair and force data retention.
set -euo pipefail
python3 "$(dirname "$0")/_home_selection.py"
