# Bring-up

How far the lifted game gets, and each wall on the way, newest last. Every
entry is something the next reader would otherwise spend a day on.

Runs use the farm's build (`netlab build ron`; recipe `local/projects/ron.env`)
against the player's Steam install (`--game "<...>/Rise of Nations"`). The
game files are read in place and never copied into the repo.

## 0. Where the host is linked

The host links at `/BASE:0x20000000`, not tw's `0x60000000`: this exe is
390 MB, and from 0x60000000 it runs into WOW64's 32-bit system DLLs near
0x77000000, so Windows will not start it ("the subsystem needed ... is not
present"). The guest's 0x00400000..0x00FB4000 stays free either way.

## 1. A static initializer the catalog missed (run 1, 2026-11-08)

```
ron.exe --run --headless --watchdog 60 --game "<install>"
[bind] 0x00400000: 461 native, 0 guest, 10 shimmed, 0 unresolved
  lifted functions in dispatch: 73895
  mapped riseofnations.exe: 0x00400000-0x00FB4000
  entering 0x0055D699
[callback] Windows called guest 0x004048A0, which is not lifted
=== fault 0xC0000005 at 0x004048A0 ===
  in lifted sub_0055E12A, last native call api-ms-win-crt-runtime-l1-1-0.dll!_initterm
```

Imports bind cleanly, and the CRT startup runs into the C++ static
initializers. The entry point calls `_initterm(0x00AC57C8, 0x00AC6934)`
(at 0x0055D59D; `sub_0055E12A` is the import thunk). Of that table's 1114
pointers, the catalog had every one except `0x004048A0`, which nothing
else references. The `_initterm_e` table (0x00AC6938..0x00AC6948) is fully
lifted. Fix: `RUN_SEEDS` in run_lift.py; the lift is now 73,896 functions.

Exit 3 = the host's fault report.

### Where it ran

This run was meant for the test VM and ran on the workstation instead (our own
code, headless, with a 60 s watchdog), as the VM was unreachable.

## 2. Next wall: a null read in sub_00A2B530 (run 2, 2026-11-08)

With the seed in (73,896 lifted, farm build copied off builder-clangcl-140),
`_initterm` finishes and the game gets further into startup:

```
ron.exe --run --headless --watchdog 60 --game "<install>"
  lifted functions in dispatch: 73896
  entering 0x0055D699
=== fault 0xC0000005 at 0x321A81AC, thread 13300 ===
  read of 0x00000000 (null/low)
  in lifted sub_00A2B530, last native call (none)
  eax=00000001 ecx=692FC0AC edx=00000000 ebx=0182D440 esp=0746F2E8 ebp=0746F560 esi=00000000 edi=00000000
```

`sub_00A2B530` is a /GS + SEH function with a 0x25C-byte frame. Return
addresses on the guest stack point at its callers in `0x00B193D4` and
`0x00B19586` (the code around 0x00B19xxx, with 0x00EDEB78 as an argument).
The indirect-call ring is empty, so no import was involved. Not looked
into yet: the next run should use `--calltrace` and `--watch 0x00A2B530`
to find which pointer is null (most likely a global a missed initializer
or an import shim should have filled), and run on the test VM again once it's
reachable. No window yet, so no screenshot.

## 3. To the main menu and a battle (runs 3 to 79, 2026-11-11 to 11-18)

- **Guest TLS.** The null read in `sub_00A2B530` was `__declspec(thread)`
  data: native32's guest TIB had no TLS array (`[0x2C]` = 0). The toolkit now
  gives guest threads the exe's implicit TLS (pcrecomp#190); the exe reads TLS
  127 times.
- **C++ exceptions.** `_CxxThrowException` and the rethrow path are shimmed
  in the host, with the lifter's EH setup (pcrecomp `ron-build`, #193).
- **String-table popups.** The loader's main-module name is now the install's
  `riseofnations.exe`, so native DLLs resolve their files there and msxml
  finds `translated_string.xml`.
- **Remote Desktop.** `GetSystemMetrics(SM_REMOTESESSION)` answers 0; the
  renderer otherwise refuses to start over RDP.
- **The black world.** Menus drew, battles ran, the 3D world was black. The
  shaders compiled and the lifted SSE and x87 looked right; the cause was the
  CRT's `_libm_sse2_{sin,cos,tan,sqrt}_precise`, which pass their argument and
  result in `xmm0` and which the native bridge mangled, so the camera's
  matrices came out garbage. They are shimmed in `host.c`, and battles render.

Test runs from here on use `tools/hidden_run.py`: a private, never-shown
desktop, `--mute`, scripted clicks (`RON_CLICKS`) and back-buffer dumps
(`RON_SNAP`). The README's hero gif is one of those runs (run 79).
