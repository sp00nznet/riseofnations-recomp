# Roadmap

1. ~~Boot to the main menu with input~~ and play a battle: done (2026-11-18).
2. **Crash fixes, one job per crash**, from [docs/crashes.md](docs/crashes.md),
   in order: the owner's crash (not reported yet); #1 out-of-memory (confirm
   the Large Address Aware host fixes it with a late-game save); #13 a bad
   `rise2.ini` or profile; #4 loading another version's save; #2 R6025 pure
   virtual call.
3. Conformance: more boot milestones (main menu reached, first frame).
4. Land pcrecomp#190 and #193 on main, and drop setup's pin to `ron-build`.
5. Installer per push once the repo is public (recomp-launcher; the workflow
   is manual-only while private).

Not planned yet: multiplayer and the online stack (PlayFab, Crossplay,
SignalR), Twitch streaming, the `patriots.exe` launcher, mods and the
Workshop, Linux.
