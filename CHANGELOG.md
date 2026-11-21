# Changelog

## Unreleased

## [0.1.0] - 2026-11-20

- Playable: boots, menus, quick and custom battles render and play
  ([bringup.md](docs/bringup.md) section 3). The black 3D world was the CRT's
  SSE2 libm calls through the bridge, now shimmed.
- Setup builds against pcrecomp's `ron-build` branch, offers the VS Build
  Tools, and refuses a path with `&`, as Red Alert 2's does.
- `tools/hidden_run.py`: muted test runs on a hidden desktop, with scripted
  clicks and frame dumps; the README's hero gif came from one.
- Set up the Rise of Nations: Extended Edition recomp in tw's shape: setup,
  lift driver, host, conformance harness ([triage](docs/triage.md),
  [RECON](docs/RECON.md)).
- The whole exe lifts: 73,895 functions, 0 errors.
- The host is Large Address Aware: the game's most reported in-game crash
  (`Cant create vertex buffer !:outofmem`) is a 2 GB address-space limit.
- [Known crashes](docs/crashes.md), with sources.
