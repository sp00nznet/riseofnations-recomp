# Rise of Nations: Extended Edition — Static Recompilation

A static recompilation of **Rise of Nations: Extended Edition** (Big Huge
Games / Microsoft, 2003; the 2014 Extended Edition, Steam build of
2024-06-20) from the shipping Win32 executable to C, compiled fresh and run
natively on Windows, on the [pcrecomp](https://github.com/sp00nznet/pcrecomp)
toolchain. It is set up the way [tw](https://github.com/sp00nznet/tiberiumwars-recomp)
(Tiberium Wars) and [civ3](https://github.com/sp00nznet/civ3) are.

The point is not just parity. The Extended Edition crashes for a lot of
players; with the whole exe as C we can find out why and fix it
([docs/crashes.md](docs/crashes.md)).

**This is an unofficial fan project, not affiliated with Microsoft or Big Huge
Games.** No game files are in this repository, and neither is the generated
C: you supply your own copy and the pipeline builds everything locally.

## Status: bring-up

| Stage | State |
|---|---|
| Triage (symbols, front end, prior art) | go: RTTI-only, PE32, nobody has done it ([triage.md](docs/triage.md)) |
| The build | Steam `riseofnations.exe`, PE timestamp 0x6674863F, no DRM ([RECON.md](docs/RECON.md)) |
| RTTI | 1,339 classes, 1,716 vtables, 3,977 virtual methods |
| Function catalog (`disasm32`) | 73,869 functions, 97.0% of `.text` |
| Lift (`run_lift.py --all`) | **73,895 functions, 19.7M lines of C, 0 lift errors** |
| Host (`build/ron.exe`, 32-bit, pcrecomp `native32`) | builds on the farm; boot status in [bringup.md](docs/bringup.md) |
| Known crashes | 13 collected with sources ([crashes.md](docs/crashes.md)) |

### Already better than the original

`riseofnations.exe` is not Large Address Aware, so it has 2 GB of address
space, and the most reported in-game crash, `Cant create vertex buffer
!:outofmem` in long matches, is that limit. `ron.exe` is linked
`/LARGEADDRESSAWARE`: 4 GB on 64-bit Windows, by construction.

## Getting started

You need **your own copy of Rise of Nations: Extended Edition** from Steam.
Nothing from the game is in this repository and nothing is downloaded for you.

1. Download this repository and unzip it somewhere with 8 GB free.
2. Double-click **`Setup.cmd`**.

It checks for Python 3.10+ (`pefile`, `capstone`), the pcrecomp toolkit,
Visual Studio 2022 with the C++ x86 tools, CMake and Ninja, and **asks**
before installing anything. It finds the game in your Steam library (or asks
for the folder), copies only `riseofnations.exe` into `work\`, builds the
function catalog, lifts and builds. The game itself stays where Steam put it.
It ends with `Rise of Nations (recomp).cmd` in this folder.

### Step by step

With pcrecomp cloned beside this repository as `tools\`:

```
py -3 -m pip install --user pefile capstone
copy "<install>\riseofnations.exe" work\
py -3 ..\tools\tools\pe\pe_analyze.py work\riseofnations.exe --json work\pe_analysis.json
py -3 ..\tools\tools\cpp\rtti.py work\riseofnations.exe -o work\rtti.json --seeds work\rtti_seeds.json
py -3 ..\tools\tools\disasm\disasm32.py work\riseofnations.exe -o work\functions.json --seed-functions work\rtti_seeds.json
py -3 run_lift.py --all
build.cmd
build\ron.exe --run --game "<install>"
```

### Running

```
build\ron.exe --run --game "C:\...\steamapps\common\Rise of Nations"
build\ron.exe --run --headless --game "<install>" --watchdog 60   # hidden window, bounded run
py -3 tools\conformance.py     # boot milestones and lift health against conformance.json
```

## Not in scope yet

Multiplayer and the online stack (PlayFab, Crossplay, SignalR), Twitch, the
`patriots.exe` launcher, mods and the Workshop, Linux ([ROADMAP](ROADMAP.md)).

## Credits

Built on [pcrecomp](https://github.com/sp00nznet/pcrecomp). Idea notes:
[recomp-ideas](https://github.com/sp00nznet/recomp-ideas).

## License

The host code and tools are MIT ([LICENSE](LICENSE)). Rise of Nations is
(c) Microsoft; you need your own copy.
