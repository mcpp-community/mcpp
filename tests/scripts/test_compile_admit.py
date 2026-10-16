"""Exercise the real compile-admit wrapper with bounded subprocess waits."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


MCPP = os.environ.get("MCPP", "")


@unittest.skipUnless(MCPP, "set MCPP to the freshly built executable")
class CompileAdmit(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="mcpp-admit-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.sem = self.root / "tokens"

    def argv(self, script, *args, baseline=0, bmi=None):
        cmd = [MCPP, "compile-admit", "--sem", str(self.sem),
               "--mem-baseline", str(baseline), "--mem-threshold", "1"]
        if bmi is not None:
            cmd += ["--bmi-restat", str(bmi)]
        return cmd + ["--", sys.executable, "-c", script, *map(str, args)]

    def run_compile(self, script, *args, **kwargs):
        return subprocess.run(self.argv(script, *args, **kwargs),
                              capture_output=True, text=True, timeout=10)

    def test_allowed_memory_does_not_acquire_counting_slots(self):
        (self.sem / "0").mkdir(parents=True)
        result = self.run_compile("pass")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(self.sem.iterdir()), [self.sem / "0"])

    def test_pressure_admits_one_and_releases_fallback(self):
        ready = self.root / "ready"
        release = self.root / "release"
        done = self.root / "done"
        # Keep the fixture paths in the Python source rather than passing them
        # after `-c`. On Windows compile-admit crosses a cmd.exe/CreateProcess
        # boundary; relying on a second round of positional-argument quoting
        # made this admission test fail before it exercised admission (the
        # child saw no sys.argv[1]). repr() is a Python literal and preserves
        # Windows backslashes and any punctuation in the temporary path.
        ready_literal = repr(str(ready))
        release_literal = repr(str(release))
        done_literal = repr(str(done))
        # The program is ONE LINE on purpose. `compile-admit` starts the child
        # through cmd.exe on Windows (`run_exec`'s documented shell path), and a
        # literal newline ends the command there: only the first line would run,
        # the child would touch `ready` and exit immediately, releasing the very
        # token this test needs it to hold. Whether that surfaced as a failure
        # or as the "metrics unavailable" skip was then a ~10ms race. The wait
        # loop therefore lives in an exec'd string, whose `\n` is an escape and
        # not a command separator.
        script = (f"import pathlib,time; ready=pathlib.Path({ready_literal}); "
                  f"release=pathlib.Path({release_literal}); ready.touch(); "
                  "deadline=time.monotonic()+10; "
                  "exec('while not release.exists() and time.monotonic()<deadline:"
                  " time.sleep(.01)')")
        first = subprocess.Popen(self.argv(script, baseline=10**18))
        second = None
        try:
            deadline = time.monotonic() + 10
            while not ready.exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue(ready.exists(), "pressure must still admit the first edge")
            # The holding edge must still be INSIDE the wrapper. Without this,
            # a child that exits at once reads as "memory metrics unavailable"
            # below and the test silently stops testing admission.
            self.assertIsNone(first.poll(), "the holding edge exited before the gate was read")
            if not (self.sem / "0").exists():
                self.skipTest("memory metrics unavailable on this host")
            second = subprocess.Popen(self.argv(
                f"import pathlib; pathlib.Path({done_literal}).touch()",
                baseline=10**18))
            time.sleep(.2)
            self.assertIsNone(second.poll())
            self.assertFalse(done.exists(), "only one pressure fallback may run")
            release.touch()
            self.assertEqual(first.wait(timeout=10), 0)
            self.assertEqual(second.wait(timeout=10), 0)
            self.assertTrue(done.exists())
            self.assertFalse((self.sem / "0").exists())
        finally:
            release.touch()
            for proc in (first, second):
                if proc is not None and proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=10)

    def test_malformed_numeric_option_is_named(self):
        # An omitted value keeps its default; a MALFORMED one must not be
        # silently indistinguishable from an omitted one.
        cases = (
            (["compile-admit", "--sem", str(self.sem), "--cap", "x",
              "--", sys.executable, "-c", "pass"], "--cap"),
            (["compile-admit", "--sem", str(self.sem), "--mem-baseline", "x",
              "--", sys.executable, "-c", "pass"], "--mem-baseline"),
            (["compile-admit", "--sem", str(self.sem), "--mem-threshold", "x",
              "--", sys.executable, "-c", "pass"], "--mem-threshold"),
            (["bmi-compile", "--cap", "x"], "--cap"),
        )
        for argv, flag in cases:
            with self.subTest(argv=argv):
                result = subprocess.run([MCPP, *argv], capture_output=True,
                                        text=True, timeout=10)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(flag, result.stderr)

    def test_missing_scanner_depfile_is_named(self):
        # The compile succeeded, so the exit 1 is the wrapper's; it must name
        # the file it could not read rather than look like a compiler failure.
        missing = self.root / "no-such.d"
        result = subprocess.run(
            [MCPP, "compile-admit", "--sem", str(self.sem), "--mem-baseline", "0",
             "--dep-raw", str(missing), "--dep-out", str(self.root / "out.d"),
             "--", sys.executable, "-c", "pass"],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(str(missing), result.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX BMI restat contract")
    def test_bmi_restore_and_failure_cleanup(self):
        bmi = self.root / "interface.pcm"
        old_time = 1_600_000_000_000_000_000
        writer = "import pathlib,sys; pathlib.Path(sys.argv[1]).write_bytes(sys.argv[2].encode())"
        for content, status, expected in (("same", 0, b"same"),
                                           ("changed", 0, b"changed"),
                                           ("failed", 7, b"failed")):
            with self.subTest(content=content):
                bmi.write_bytes(b"same")
                os.utime(bmi, ns=(old_time, old_time))
                result = self.run_compile(writer + f"; sys.exit({status})",
                                          bmi, content, bmi=bmi)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual(bmi.read_bytes(), expected)
                if content == "same":
                    self.assertEqual(bmi.stat().st_mtime_ns, old_time)
                else:
                    self.assertNotEqual(bmi.stat().st_mtime_ns, old_time)
                self.assertFalse(Path(str(bmi) + ".bak").exists())
        bmi.unlink()
        result = self.run_compile(writer, bmi, "new", bmi=bmi)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(bmi.read_bytes(), b"new")

    @unittest.skipIf(os.name == "nt", "POSIX BMI restat contract")
    def test_equivalent_bmi_does_not_cascade_in_ninja(self):
        ninja = shutil.which("ninja")
        if not ninja:
            self.skipTest("ninja unavailable")
        source = self.root / "source"
        source.write_text("same")
        helper = self.root / "compile.py"
        helper.write_text("from pathlib import Path\nPath('interface.pcm').write_bytes(b'same')\n")
        consumer = self.root / "consumer.py"
        consumer.write_text("from pathlib import Path\nPath('consumer').touch()\n")
        (self.root / "build.ninja").write_text(
            f'rule bmi\n  command = "{MCPP}" compile-admit --sem tokens '
            f'--mem-baseline 0 --bmi-restat "$out" -- "{sys.executable}" "{helper}"\n'
            '  restat = 1\n'
            f'rule consume\n  command = "{sys.executable}" "{consumer}"\n'
            'build interface.pcm: bmi source\n'
            'build consumer: consume interface.pcm\n')
        def build():
            result = subprocess.run([ninja, "-f", "build.ninja"], cwd=self.root,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
        build()
        before = (self.root / "consumer").stat().st_mtime_ns
        time.sleep(.05)
        source.touch()
        build()
        self.assertEqual((self.root / "consumer").stat().st_mtime_ns, before)


if __name__ == "__main__":
    unittest.main()
