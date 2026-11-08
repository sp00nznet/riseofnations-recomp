# Reconnaissance

What the binary is and what the first pipeline pass found. Every number is
from the commands shown; rerun them on your own copy.

## The build

The Steam Extended Edition (app 287450), `riseofnations.exe`:

```
$ py -3 ..\tools\tools\pe\pe_analyze.py work\riseofnations.exe
  Machine:       i386            Linker: 14.00 (Visual Studio 2015 toolset)
  Image Base:    0x00400000      Entry Point: 0x0055D699
  Timestamp:     2024-06-20 19:42:55 UTC (0x6674863F)
  Code Range:    0x00401000 - 0x00AC4400 (7,091,200 bytes)
  .text .rdata .data .gfids .tls _RDATA .rsrc .reloc
  Imports: 471 functions from 26 DLLs
$ py -3 ..\tools\tools\pe\analyze_sections.py work\riseofnations.exe
  No known protection/packer indicators found.
```

SHA-1 `2905c77f3b4eff3cd0bfe7e465aa5061dd926487`. Not Large Address Aware
(characteristics 0x0102). `patriots.exe` beside it is the launcher, not the
game.

## Imports

The CRT is dynamic: the UCRT (`api-ms-win-crt-*`), `VCRUNTIME140`,
`MSVCP140`. Only 69 `KERNEL32` and 23 `USER32` imports: the window, the message
loop and most of Win32 are found at run time through `GetProcAddress` (the
host shims that too). Other DLLs: `steam_api`, `CrossplayNetLib`,
`CrossplayProxy`, `cell_v140` (PlayFab party), `cpprest140_2_9`, `dbghelp`
(the game's own crash logger), `WINMM`, `ole32`. The renderer is `d3dgl.dll`
(an OpenGL-style layer on Direct3D 11, export `get_graphsy_object_ptr`),
loaded at run time; there are no Direct3D imports in the exe.

## Pipeline numbers

| Step | Command | Result |
|---|---|---|
| RTTI | `cpp/rtti.py` | 1,339 classes, 1,716 vtables, 3,977 virtual methods |
| Catalog | `disasm/disasm32.py --seed-functions work\rtti_seeds.json` | 73,869 functions, 13.2M instructions, 97.0% of `.text`, about 25 minutes |
| Lift | `run_lift.py --all` | **73,895 functions, 0 errors**, 535 without a terminator, 19.7M lines of C in 185 files, about 5 minutes |
