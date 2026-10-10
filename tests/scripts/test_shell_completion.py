#!/usr/bin/env python3
"""Exercise completion through the fresh binary and actual shell adapters."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MCPP = Path(os.environ["MCPP"]).resolve() if os.environ.get("MCPP") else None


@unittest.skipUnless(MCPP, "set MCPP to a freshly built binary")
class ShellCompletionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mcpp-completion-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / "home with spaces"
        self.env = dict(os.environ, MCPP_HOME=str(self.home),
                        XDG_CACHE_HOME=str(self.root / "cache"),
                        XDG_CONFIG_HOME=str(self.root / "config"),
                        XDG_DATA_HOME=str(self.root / "data"))
        self.env.pop("MCPP_VERBOSE", None)
        self.run_command([str(MCPP), "self", "completion"])
        self.scripts = self.home / "config" / "shell"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        shutil.copy2(MCPP, self.bin / ("mcpp.exe" if os.name == "nt" else "mcpp"))
        self.env["PATH"] = str(self.bin) + os.pathsep + self.env["PATH"]

    def run_command(self, argv, **kwargs):
        result = subprocess.run(argv, env=self.env, cwd=self.root, text=True,
                                capture_output=True, timeout=30, **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.splitlines()

    def shell(self, name):
        path = shutil.which(name)
        if path is None:
            if os.environ.get("MCPP_REQUIRE_SHELLS"):
                self.fail(f"required shell missing: {name}")
            self.skipTest(f"shell not installed: {name}")
        return path

    def test_query_is_read_only_and_does_not_execute_partial_commands(self):
        self.env["MCPP_HOME"] = str(self.root / "absent")
        self.assertEqual(self.run_command([str(MCPP), "__complete", "self", "co"]),
                         ["words", "completion", "config"])
        self.assertEqual(self.run_command([str(MCPP), "__complete", "build", "--cache=l"]),
                         ["words", "--cache=local"])
        self.assertEqual(self.run_command([str(MCPP), "__complete", "build", "--jobs", ""]),
                         ["words"])
        self.assertEqual(self.run_command([str(MCPP), "__complete", "run", "--", "--cache"]),
                         ["files"])
        self.assertFalse((self.root / "absent").exists())

    def test_install_is_idempotent_and_rejects_unknown_shell(self):
        files = sorted(p.name for p in self.scripts.iterdir())
        self.assertEqual(files, ["mcpp.bash", "mcpp.fish", "mcpp.ps1", "mcpp.zsh"])
        before = {p.name: p.stat().st_mtime_ns for p in self.scripts.iterdir()}
        self.run_command([str(MCPP), "self", "completion"])
        self.assertEqual(before, {p.name: p.stat().st_mtime_ns for p in self.scripts.iterdir()})
        result = subprocess.run([str(MCPP), "self", "completion", "bad"], env=self.env,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("unsupported shell", result.stderr)

    def test_bash(self):
        code = r'''
source "$MCPP_HOME/config/shell/mcpp.bash"
source "$MCPP_HOME/config/shell/mcpp.bash"
COMP_WORDS=(mcpp self co); COMP_CWORD=2
_mcpp_complete; printf '%s\n' "${COMPREPLY[@]}"
COMP_WORDS=(mcpp build --cache = l); COMP_CWORD=4
_mcpp_complete; printf '%s\n' "${COMPREPLY[@]}"
COMP_WORDS=(mcpp build --cache=lo); COMP_CWORD=2
_mcpp_complete; printf '%s\n' "${COMPREPLY[@]}"
COMP_WORDS=(mcpp emit xpkg -o 'some path/fi'); COMP_CWORD=4
_mcpp_complete; printf '%s\n' "${COMPREPLY[@]}"
COMP_WORDS=(mcpp emit xpkg '--output=some path/fi'); COMP_CWORD=3
_mcpp_complete; printf '%s\n' "${COMPREPLY[@]}"
'''
        (self.root / "some path").mkdir()
        (self.root / "some path" / "file.lua").touch()
        self.assertEqual(self.run_command([self.shell("bash"), "--noprofile", "--norc", "-c", code]),
                         ["completion", "config", "local", "--cache=local", "some path/file.lua",
                          "--output=some path/file.lua"])

    def test_zsh(self):
        # Run the registered callback with the same words/CURRENT supplied by
        # the completion widget, capturing compadd's literal arguments.
        code = r'''
if [[ -n $MCPP_TEST_ZSH_ROOT ]]; then
    module_path=("$MCPP_TEST_ZSH_ROOT"/usr/lib/*/zsh/*(N/))
fi
compdef() { :; }
source "$MCPP_HOME/config/shell/mcpp.zsh"
source "$MCPP_HOME/config/shell/mcpp.zsh"
compadd() { shift; printf '%s\n' "$@"; }
_files() { printf 'files\n'; }
words=(mcpp self co ignored); CURRENT=3; _mcpp_complete
words=(mcpp build --cache=l); CURRENT=3; _mcpp_complete
words=(mcpp emit xpkg -o 'some path/fi'); CURRENT=5; _mcpp_complete
'''
        self.assertEqual(self.run_command([self.shell("zsh"), "-f", "-c", code]),
                         ["completion", "config", "--cache=local", "files"])

    def test_zsh_tab_widget(self):
        import fcntl
        import pty
        import select
        import termios
        import time

        shell = self.shell("zsh")
        (self.root / "artifact.lua").touch()
        master, slave = pty.openpty()

        def child_setup():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

        env = dict(self.env, TERM="xterm", ZDOTDIR=str(self.root))
        process = subprocess.Popen([shell, "-f"], cwd=self.root, env=env,
                                   stdin=slave, stdout=slave, stderr=slave,
                                   preexec_fn=child_setup)
        os.close(slave)

        def read_until(marker):
            output = b""
            deadline = time.monotonic() + 15
            while marker not in output and time.monotonic() < deadline:
                if select.select([master], [], [], 0.2)[0]:
                    output += os.read(master, 65536)
            self.assertIn(marker, output, output.decode(errors="replace"))

        setup = r'''
if [[ -n $MCPP_TEST_ZSH_ROOT ]]; then
    fpath=("$MCPP_TEST_ZSH_ROOT"/usr/share/zsh/functions/**/*(N/))
    module_path=("$MCPP_TEST_ZSH_ROOT"/usr/lib/*/zsh/*(N/))
fi
mkdir -p "$XDG_CACHE_HOME"
autoload -Uz compinit
compinit -u -d "$XDG_CACHE_HOME/zcompdump"
source "$MCPP_HOME/config/shell/mcpp.zsh"
mcpp-inspect() { print -r -- "MCPP_BUFFER:$BUFFER"; BUFFER=''; zle reset-prompt; }
zle -N mcpp-inspect
bindkey '^X' mcpp-inspect
print MCPP_READY
'''
        try:
            os.write(master, setup.encode())
            read_until(b"\r\nMCPP_READY\r\n")
            os.write(master, b"mcpp self conf\t\x18")
            read_until(b"MCPP_BUFFER:mcpp self config")
            os.write(master, b"mcpp emit xpkg --output=artifact\t\x18")
            read_until(b"MCPP_BUFFER:mcpp emit xpkg --output=artifact.lua")
        finally:
            process.kill()
            process.wait(timeout=5)
            os.close(master)

    def test_fish(self):
        code = r'''
if set -q MCPP_TEST_FISH_DATA_DIR
    set -g fish_function_path "$MCPP_TEST_FISH_DATA_DIR/functions" $fish_function_path
end
source "$MCPP_HOME/config/shell/mcpp.fish"
source "$MCPP_HOME/config/shell/mcpp.fish"
complete -C 'mcpp self co'
complete -C 'mcpp build --cache=l'
complete -C 'mcpp self completion p'
complete -C 'mcpp build --off'
complete -C 'mcpp emit xpkg --output=artifact'
'''
        (self.root / "artifact.lua").touch()
        result = self.run_command([self.shell("fish"), "--no-config", "-c", code])
        self.assertEqual([line.split("\t")[0] for line in result],
                         ["completion", "config", "--cache=local", "pwsh", "--offline", "--output=artifact.lua"])

    def test_powershell(self):
        code = r'''
. "$env:MCPP_HOME/config/shell/mcpp.ps1"
. "$env:MCPP_HOME/config/shell/mcpp.ps1"
foreach ($line in @('mcpp self co', 'mcpp build --cache=l', 'mcpp self completion p', 'mcpp self ')) {
    [System.Management.Automation.CommandCompletion]::CompleteInput($line, $line.Length, $null).CompletionMatches |
        ForEach-Object { $_.CompletionText }
}
$line = 'mcpp emit xpkg --output=artifact'
[System.Management.Automation.CommandCompletion]::CompleteInput($line, $line.Length, $null).CompletionMatches |
    ForEach-Object { $_.CompletionText }
'''
        (self.root / "artifact.lua").touch()
        result = self.run_command([self.shell("pwsh"), "-NoProfile", "-NonInteractive", "-Command", code])
        self.assertEqual(result[:4], ["completion", "config", "--cache=local", "pwsh"])
        self.assertIn("doctor", result[4:])
        self.assertTrue(result[-1].startswith("--output="), result)
        self.assertIn("artifact.lua", result[-1])

    def test_standalone_installer_quotes_paths_and_does_not_duplicate_rc_lines(self):
        payload = self.root / "payload" / "mcpp-test" / "bin"
        payload.mkdir(parents=True)
        shutil.copy2(MCPP, payload / "mcpp")
        archive = self.root / "release.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(payload.parent, arcname="mcpp-test")
        # Only downloads are substituted; extraction, binary execution and rc
        # editing all run through the production installer.
        curl = self.bin / "curl"
        curl.write_text('''#!/usr/bin/env bash
while (($#)); do
    if [[ $1 == -o ]]; then destination=$2; shift 2; else url=$1; shift; fi
done
[[ $url == *.tar.gz ]] || exit 22
cp "$MCPP_TEST_ARCHIVE" "$destination"
''')
        curl.chmod(0o755)
        prefix = self.root / "custom ' $prefix `literal`"
        self.env.update(HOME=str(self.root / "user"), SHELL="/bin/bash",
                        MCPP_PREFIX=str(prefix), MCPP_TEST_ARCHIVE=str(archive))
        self.run_command(["bash", str(ROOT / "install.sh")])
        rc = Path(self.env["HOME"]) / ".bashrc"
        before = rc.read_text()
        self.run_command(["bash", str(ROOT / "install.sh")])
        self.assertEqual(before, rc.read_text())
        output = self.run_command(["bash", "--noprofile", "--norc", "-c",
                                   f"source {shlex.quote(str(rc))}; complete -p mcpp"])
        self.assertEqual(output, ["complete -F _mcpp_complete mcpp"])
        self.assertTrue((prefix / "config" / "shell" / "mcpp.ps1").is_file())
        for name in ("zsh", "fish", "pwsh"):
            shell = self.shell(name)
            user = self.root / f"user-{name}"
            config = user / "config"
            self.env.update(HOME=str(user), SHELL=shell,
                            ZDOTDIR=str(user / "zsh"), XDG_CONFIG_HOME=str(config))
            if name == "zsh":
                rc = user / "zsh" / ".zshrc"
                source = r'''
if [[ -n $MCPP_TEST_ZSH_ROOT ]]; then
    module_path=("$MCPP_TEST_ZSH_ROOT"/usr/lib/*/zsh/*(N/))
fi
compdef() { :; }
source "$MCPP_TEST_RC"
(( $+functions[_mcpp_complete] ))
'''
                argv = [shell, "-f", "-c", source]
            elif name == "fish":
                rc = config / "fish" / "config.fish"
                argv = [shell, "--no-config", "-c",
                        'source "$MCPP_TEST_RC"; functions -q __mcpp_candidates']
            else:
                rc = Path(self.run_command([shell, "-NoProfile", "-Command",
                                           "$PROFILE.CurrentUserAllHosts"])[0])
                source = r'''
. $env:MCPP_TEST_RC
$line = 'mcpp self co'
$completionMatches = [System.Management.Automation.CommandCompletion]::CompleteInput($line, $line.Length, $null).CompletionMatches
if ('config' -notin $completionMatches.CompletionText) { exit 1 }
'''
                argv = [shell, "-NoProfile", "-NonInteractive", "-Command", source]
            self.run_command(["bash", str(ROOT / "install.sh")])
            before = rc.read_text()
            self.run_command(["bash", str(ROOT / "install.sh")])
            self.assertEqual(before, rc.read_text(), name)
            self.env["MCPP_TEST_RC"] = str(rc)
            self.run_command(argv)
        self.env.update(HOME=str(self.root / "optout"), MCPP_NO_PATH="1", MCPP_NO_COMPLETION="1")
        self.run_command(["bash", str(ROOT / "install.sh")])
        self.assertFalse((Path(self.env["HOME"]) / ".bashrc").exists())


if __name__ == "__main__":
    unittest.main()
