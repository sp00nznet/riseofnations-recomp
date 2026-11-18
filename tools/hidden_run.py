"""Run ron.exe on a private, never-shown Windows desktop, so a test run can't
show a window, take focus, play its intro over the user's screen, or go
exclusive fullscreen (DXGI can't on a desktop that isn't the input one).

  python tools/hidden_run.py SECONDS LOG_PREFIX -- ron.exe args...

stdout/stderr go to LOG_PREFIX.out / .err; the process is killed after
SECONDS. Refuses to run without --mute in the ron.exe args.
"""
import ctypes, ctypes.wintypes as w, subprocess, sys, time

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32.CreateDesktopW.restype = w.HANDLE
u32.CreateDesktopW.argtypes = [w.LPCWSTR, w.LPCWSTR, ctypes.c_void_p, w.DWORD, w.DWORD, ctypes.c_void_p]
k32.CreateFileW.restype = w.HANDLE


class SI(ctypes.Structure):
    _fields_ = [('cb', w.DWORD), ('lpReserved', w.LPWSTR), ('lpDesktop', w.LPWSTR), ('lpTitle', w.LPWSTR),
                ('x', w.DWORD), ('y', w.DWORD), ('cx', w.DWORD), ('cy', w.DWORD), ('cxc', w.DWORD),
                ('cyc', w.DWORD), ('fill', w.DWORD), ('dwFlags', w.DWORD), ('show', w.WORD), ('r2', w.WORD),
                ('lpr2', ctypes.c_void_p), ('hIn', w.HANDLE), ('hOut', w.HANDLE), ('hErr', w.HANDLE)]


class PI(ctypes.Structure):
    _fields_ = [('hProcess', w.HANDLE), ('hThread', w.HANDLE), ('pid', w.DWORD), ('tid', w.DWORD)]


class SA(ctypes.Structure):
    _fields_ = [('n', w.DWORD), ('sd', ctypes.c_void_p), ('inherit', w.BOOL)]


def out_file(path):
    sa = SA(ctypes.sizeof(SA), None, True)
    return k32.CreateFileW(path, 0x40000000, 3, ctypes.byref(sa), 2, 0x80, None)   # GENERIC_WRITE, CREATE_ALWAYS


def main():
    secs, prefix = int(sys.argv[1]), sys.argv[2]
    cmd = sys.argv[sys.argv.index('--') + 1:]
    if '--mute' not in cmd:
        sys.exit('refusing to run without --mute')
    name = 'ron-test-%d' % int(time.time())
    desk = u32.CreateDesktopW(name, None, None, 0, 0x10000000, None)   # GENERIC_ALL
    if not desk:
        sys.exit('CreateDesktop failed: %d' % ctypes.get_last_error())
    si = SI(cb=ctypes.sizeof(SI), lpDesktop='WinSta0\\' + name, dwFlags=0x100)   # STARTF_USESTDHANDLES
    si.hOut, si.hErr = out_file(prefix + '.out'), out_file(prefix + '.err')
    pi = PI()
    line = ctypes.create_unicode_buffer(subprocess.list2cmdline(cmd))
    if not k32.CreateProcessW(None, line, None, None, True, 0, None, None, ctypes.byref(si), ctypes.byref(pi)):
        sys.exit('CreateProcess failed: %d' % ctypes.get_last_error())
    if k32.WaitForSingleObject(pi.hProcess, secs * 1000) != 0:
        k32.TerminateProcess(pi.hProcess, 1)
        k32.WaitForSingleObject(pi.hProcess, 10000)
    code = w.DWORD()
    k32.GetExitCodeProcess(pi.hProcess, ctypes.byref(code))
    for h in (pi.hProcess, pi.hThread, si.hOut, si.hErr):
        k32.CloseHandle(h)
    u32.CloseDesktop(desk)
    print('exit 0x%X' % code.value)


if __name__ == '__main__':
    main()
