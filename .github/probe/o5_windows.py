# O5 (Windows half): LockFileEx waiting semantics, replacing a running
# executable, and what a concurrent CreateProcess sees while link.exe writes.
# Prints facts; never asserts.
import ctypes, ctypes.wintypes as wt, msvcrt, os, shutil, subprocess, sys, tempfile, threading, time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
LOCKFILE_EXCLUSIVE_LOCK, LOCKFILE_FAIL_IMMEDIATELY = 2, 1

class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD), ("hEvent", wt.HANDLE)]

def lock(fh, flags):
    ov = OVERLAPPED()
    ok = k32.LockFileEx(wt.HANDLE(msvcrt.get_osfhandle(fh.fileno())), flags, 0, 1, 0, ctypes.byref(ov))
    return bool(ok), ctypes.get_last_error()

d = tempfile.mkdtemp()
lk = os.path.join(d, "build.lock")
open(lk, "w").close()
holder = subprocess.Popen([sys.executable, "-c",
    "import ctypes,ctypes.wintypes as wt,msvcrt,time\n"
    "k=ctypes.WinDLL('kernel32')\n"
    "class O(ctypes.Structure): _fields_=[('a',ctypes.c_void_p),('b',ctypes.c_void_p),('c',wt.DWORD),('d',wt.DWORD),('e',wt.HANDLE)]\n"
    f"f=open({lk!r},'r+'); o=O(); print(k.LockFileEx(wt.HANDLE(msvcrt.get_osfhandle(f.fileno())),2,0,1,0,ctypes.byref(o)),flush=True); time.sleep(60)\n"],
    stdout=subprocess.PIPE, text=True)
print("O5.win holder locked:", holder.stdout.readline().strip())
f = open(lk, "r+")
print("O5.win LockFileEx FAIL_IMMEDIATELY while held -> (ok, err) =", lock(f, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY))
res = {}
def blocking():
    t = time.time(); res["r"] = lock(f, LOCKFILE_EXCLUSIVE_LOCK); res["t"] = time.time() - t
th = threading.Thread(target=blocking); th.start()
time.sleep(1.0)
holder.kill(); holder.wait()
th.join(10)
print(f"O5.win blocking LockFileEx after TerminateProcess of holder: {res.get('r')} after {res.get('t', -1):.2f}s (1.0s of it was the hold)")
f.close()

# Handle inheritance: Python's handles are non-inheritable; mcpp's CreateFileW
# passes no SECURITY_ATTRIBUTES, so the same holds there.
fh = open(lk)
print("O5.win os.get_handle_inheritable(lock) =", os.get_handle_inheritable(msvcrt.get_osfhandle(fh.fileno())))
fh.close()

# A running executable: overwrite, rename aside, rename into place, delete.
src = shutil.which("ping") or r"C:\Windows\System32\PING.EXE"
prog = os.path.join(d, "prog.exe")
shutil.copy(src, prog)
run = subprocess.Popen([prog, "-n", "30", "127.0.0.1"], stdout=subprocess.DEVNULL)
time.sleep(0.5)
shutil.copy(src, prog + ".tmp")
def attempt(label, fn):
    try:
        fn(); print(f"O5.win {label}: ok")
    except OSError as e:
        print(f"O5.win {label}: {type(e).__name__} winerror={getattr(e, 'winerror', None)}")
attempt("os.replace(tmp -> running exe)", lambda: os.replace(prog + ".tmp", prog))
attempt("open running exe for write", lambda: open(prog, "r+b").close())
attempt("rename running exe aside (.old-1)", lambda: os.rename(prog, prog + ".old-1"))
attempt("rename tmp into the freed name", lambda: os.replace(prog + ".tmp", prog))
attempt("run the new exe", lambda: subprocess.run([prog, "-n", "1", "127.0.0.1"], stdout=subprocess.DEVNULL, check=True))
attempt("delete .old-1 while it runs", lambda: os.remove(prog + ".old-1"))
run.kill(); run.wait(); time.sleep(0.5)
attempt("delete .old-1 after exit", lambda: os.remove(prog + ".old-1"))

# A loaded DLL: rename aside.
dll = os.path.join(d, "probe.dll")
shutil.copy(r"C:\Windows\System32\version.dll", dll)
h = ctypes.WinDLL(dll)
attempt("rename loaded DLL aside", lambda: os.rename(dll, dll + ".old-1"))
attempt("copy a new DLL into the freed name", lambda: shutil.copy(r"C:\Windows\System32\version.dll", dll))
