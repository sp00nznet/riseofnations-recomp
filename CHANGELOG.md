# Changelog

## Unreleased

- Set up the Rise of Nations: Extended Edition recomp in tw's shape: setup,
  lift driver, host, conformance harness ([triage](docs/triage.md),
  [RECON](docs/RECON.md)).
- The whole exe lifts: 73,895 functions, 0 errors.
- The host is Large Address Aware: the game's most reported in-game crash
  (`Cant create vertex buffer !:outofmem`) is a 2 GB address-space limit.
- [Known crashes](docs/crashes.md), with sources.
