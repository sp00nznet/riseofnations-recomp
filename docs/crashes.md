# Known crashes in Rise of Nations: Extended Edition

The list the crash-fix work starts from. Collected 2026-11-07 from the
sources in each row; one row per distinct crash. **Repro** says whether it has
been reproduced on the owner's copy (Steam, build 0x6674863F). **Where** says
whether it lives in `riseofnations.exe` (the recomp can find and fix it) or
outside it (a driver, a runtime, a config file: out of scope for the recomp,
listed so nobody chases it in the code).

| # | Symptom | When | Where | Repro | Known workaround | Source |
|---|---|---|---|---|---|---|
| 1 | `BHG RTS run time ERROR, Cant create vertex buffer !:outofmem` | at random in long matches, on machines with plenty of RAM | **exe**: a 32-bit process out of address space. The exe is *not* Large Address Aware (FILE_HEADER characteristics 0x0102) | not yet (needs a long late-game match) | set the LAA bit (NTCore 4GB Patch). **The recomp host links `/LARGEADDRESSAWARE`, so ron.exe has 4 GB by construction**; to confirm with a late-game save | [Steam](https://steamcommunity.com/app/287450/discussions/0/3827540930023877649/) |
| 2 | `R6025 pure virtual function call`, or every player drops at once | multiplayer, mostly 4v4 / FFA, minutes in | **exe** (a C++ runtime abort in game code) | no (multiplayer is out of scope for now) | none known | [Steam](https://steamcommunity.com/app/287450/discussions/0/540744934567876480/) |
| 3 | Auto-resign, desync or crash to desktop on the loading screen, 20 to 30 minutes in | multiplayer; one report ties it to Inca reaching Gunpowder | exe or network, unclear | no | firewall exceptions (unconfirmed) | [Steam](https://steamcommunity.com/app/287450/discussions/0/540744474802159986/) |
| 4 | Crash on loading a save | a save from another version, or after a reinstall | **exe**: saves are tied to the build that wrote them; the loader should refuse, not crash | not yet | switch to the matching Steam beta (`v1.10`) | [Steam](https://steamcommunity.com/app/287450/discussions/0/1733216261944332577/) |
| 5 | "Stopped working" right after the intro | after a game update; the old build worked | **exe** (an update regression) | not yet | the `v1.10` beta | [Steam](https://steamcommunity.com/app/287450/discussions/1/2765630416821921635) |
| 6 | Crash at startup with no message: after the intro, at the title, after entering a name, or a second into loading | at launch / first new game | mixed: runtimes or corrupt files for most; profile data for some (see 12) | not yet | verify files; reinstall runtimes (Steam support 3964-AGKL-7601) | [Steam](https://steamcommunity.com/app/287450/discussions/0/626329820941798532/) |
| 7 | "Preparing to launch..." then nothing; `Rise.exe` plays the intro then sits black | at launch | outside: DirectX June 2010 and VC++ 2012 runtimes | no | `_CommonRedist\DirectX\Jun 2010\DXSETUP.exe`, VC++ 2012 | [Steam](https://steamcommunity.com/app/287450/discussions/0/540744474973293679/) |
| 8 | `patriots.exe`: `mfc110.dll missing`, then 0xc000007b | the launcher | outside: VC++ 2012 (MFC) runtime; the launcher is not the target | no | install VC++ 2012 from `_CommonRedist` | same as 7 |
| 9 | Crash just before the main menu | first load after the intro | outside: VC++ runtimes / DirectX | no | reinstall both, run `riseofnations.exe` directly | [Steam](https://steamcommunity.com/app/287450/discussions/1/135512625252446821/) |
| 10 | Black screen crash after the intro | with MSI Afterburner / RTSS running | outside: overlay hooking the renderer | no | quit the overlay | [Steam](https://steamcommunity.com/app/287450/discussions/0/3959287799153072676/) |
| 11 | `BHG RTS run time error: Could not create textures`, green flicker in the intro | old GPU or driver (Radeon HD 3650, Vista) | outside: GPU below D3D 10 feature level. The exe could fail cleanly instead of crashing | no | none | [Steam](https://steamcommunity.com/app/287450/discussions/0/540744934581399207/) |
| 12 | DirectX errors at start, early 2025 | Windows 11 26100 with Nvidia 572.83 | outside: driver regression | no | Nvidia 566.36 (unverified) | r/riseofnations thread 1jo3x5i (search snippet only) |
| 13 | Crash fixed by deleting the profile | corrupt `%APPDATA%\Microsoft Games\Rise of Nations` (holds `rise2.ini`) | outside, but **the exe should survive a bad `rise2.ini`**: worth a look once it boots | not yet; QA's fresh-profile run covers it | back up, then delete the folder (unverified) | search snippet only |

Linux only, for completeness: WineHQ [31132](https://bugs.winehq.org/show_bug.cgi?id=31132)
(the 2003 original; fixed in Wine 1.5.9) and
[44405](https://bugs.winehq.org/show_bug.cgi?id=44405) (crypt32, fixed in Wine
3.18). ProtonDB rates the game Gold.

## Not verified

Some sources could not be read (the fandom Game versions page, Reddit, one
Workshop guide): rows 12 and 13 rest on search snippets. Alt-tab, high-DPI and
non-native-resolution crashes were not found in what was read; QA covers
them on testbox.

## What the owner hits

Not reported yet. The first crash job reproduces it and adds it here.

## Community fixes

Cited, not used. No code or data from them is in this repository.

- **Community Balance Patch** (MHLoppy, [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=2287791153)):
  balance changes with bug fixes folded in, plus a setup tool. Whether it
  changes the exe was not verified.
- **4GB Patch** (NTCore, [ntcore.com/4gb-patch](https://ntcore.com/4gb-patch/)):
  sets the Large Address Aware bit on an exe. The recomp gets the same effect
  from its own linker flag.
- `rise2.ini` settings (SkipIntroMovies, Fullscreen, VSync) recommended in
  forum posts, not verified for EE.

No exe patcher, widescreen or DPI fix for EE was found.
