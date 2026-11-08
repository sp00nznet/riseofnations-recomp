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
ron.exe --run --headless --watchdog 60 --game "E:\SteamLibrary\steamapps\common\Rise of Nations"
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

### Where testbox was

This run was meant for testbox, and ended up on the workstation (it's our own
code, headless, with a 60 s watchdog). `netlab run ron --on testbox` started,
then testbox (VM 103 on dpiprox2) stopped answering on 10.77.0.2: the VM
runs and its desktop draws (another job's Strike Force window up front, from
`qm monitor screendump`), but ping and ssh from the NAT bridge time out.
Ops needs to look at it; the next run goes back to testbox.
