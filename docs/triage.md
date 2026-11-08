# Triage: the recomp-ideas rule

Scored against the four questions in
[recomp-ideas](https://github.com/sp00nznet/recomp-ideas#the-rule-everything-here-is-measured-against)
before any code was written. Every number comes from the commands in
[RECON.md](RECON.md), run on the Steam build (`riseofnations.exe`,
PE timestamp 0x6674863F, 2024-06-20).

| Question | Answer | Verdict |
|---|---|---|
| 1. Does the binary name anything? | No PDB ships (the exe names `rise.pdb`; no older or debug build with symbols is known). But **RTTI is on**: 1,339 classes, 1,716 vtables, 3,977 virtual methods named by class (`cpp/rtti.py`). Imports are C++-mangled for `MSVCP140`, `CrossplayNetLib`, `cpprest`. No export table. | Medium. RTTI-only, like tw (Tiberium Wars, 3,570 classes), which lifted with 0 errors. Better than civ3 (no RTTI), which plays. |
| 2. How much is somebody else's code? | Less than usual: the CRT is **not** linked in. It imports the UCRT, `VCRUNTIME140` and `MSVCP140`, so none of that is lifted. The renderer (`d3dgl.dll`, an OpenGL-on-Direct3D 11 layer), sound (`soundsys.dll`), Steam, PlayFab/Crossplay, cpprest and Twitch are all separate DLLs that run natively. | Good: the 7 MB of `.text` is mostly the game. |
| 3. Does a front end exist? | PE32, `disasm/disasm32.py`. | Solved. |
| 4. Has somebody already done it? | No source release, no reimplementation, no exe patcher. The community's fixes are data/balance mods (the Community Balance Patch) and NTCore's generic 4GB patch ([crashes.md](crashes.md)). | Nobody has. |

**DRM:** none. No SteamStub `.bind` section, no packer
(`pe/analyze_sections.py`: "No known protection/packer indicators found"). It
has relocations (`.reloc`), so it maps at its preferred base with no fight.

**Why recompile rather than run it:** the corollary of the rule is that a
recomp earns its keep where the original is bad. Here the original runs, but
crashes for many players ([crashes.md](crashes.md)), and the exe has no Large
Address Aware flag, which is the root of the most reported in-game crash
(`Cant create vertex buffer !:outofmem`). The host links
`/LARGEADDRESSAWARE`, so the recompiled game gets 4 GB of address space by
construction.

**Verdict: go.** Symbols are RTTI-only, not a stall: the same shape as tw.
