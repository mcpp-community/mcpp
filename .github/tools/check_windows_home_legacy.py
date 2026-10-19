"""Prove locator round-trips under a genuinely non-UTF-8 Windows ACP."""
import os
from pathlib import Path
import re
import subprocess
import sys

if os.name != 'nt':
    raise SystemExit('This check requires Windows')
root = Path(__file__).resolve().parents[2]
manifest = root / 'mcpp.toml'
original = manifest.read_bytes()
marker = b'windows_code_page = "utf-8"'
assert original.count(marker) == 1, 'identify the test-only code-page setting explicitly'
try:
    manifest.write_bytes(original.replace(marker, b'windows_code_page = "legacy"', 1))
    result = subprocess.run([os.environ['MCPP'], '--offline', 'test', 'home'], cwd=root,
                            capture_output=True, text=True, encoding='utf-8', errors='replace',
                            timeout=600)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    assert result.returncode == 0, result.returncode
    output = re.sub(r'\x1b\[[0-9;]*m', '', result.stdout + result.stderr)
    assert ('[       OK ] HomeSelection.UnicodeLocatorRoundTripDoesNotNarrowTheSelectedPath'
            in output), 'the legacy ACP case did not pass; a UTF-8-only skip is not proof'
finally:
    manifest.write_bytes(original)
print('PASS: the locator round-trip ran under legacy Windows ACP; manifest restored')
