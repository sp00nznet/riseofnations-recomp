# Provenance

Everything in this repository is written by us. Nothing from the game is in
it: the player's own `riseofnations.exe` is read on their machine, and what is
derived from it (`work/`, `src/recomp/gen/`) is gitignored.

| Path | Origin |
|---|---|
| `src/runtime/host.c`, `run_lift.py`, `tools/`, `CMakeLists.txt`, `build.cmd`, `Setup.cmd` | ours, adapted from our tw (Tiberium Wars) recomp, MIT |
| `docs/` | ours |
| toolchain | [pcrecomp](https://github.com/sp00nznet/pcrecomp), fetched beside the repo, not vendored |

Community fixes named in [docs/crashes.md](docs/crashes.md) are cited by link
only; none of their code or data is used.
