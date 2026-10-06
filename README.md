# AmbientForce

An ambient instrument for MPC OS (Force / MPC standalone), as a VST2 plugin with its own
touchscreen pages: layers of sound that answer one MIDI stream through a shared harmony, built
for long, slowly changing pieces. What it is meant to become is in [docs/CONCEPT.md](docs/CONCEPT.md).

**Status: in development, nothing released yet.** The plugin side (VST2 glue, touchscreen pages,
preset browser, saved state, tests and tools) comes from SubForce; the engine is still a stub that
plays a sine per note. The first milestone is planned in
[docs/plans/2026-10-06-m1-first-light.md](docs/plans/2026-10-06-m1-first-light.md).

## Building

On Linux or WSL (Ubuntu 24.04: `g++`, `g++-arm-linux-gnueabihf`, `make`, `python3`, `qemu-user`,
and Pillow for the skin):

```sh
make test          # the suite under ASan/UBSan
make test-arm      # the same suite for the Force's CPU, under qemu-arm
make arm-plugin    # build/arm/ambientforce.so, profile-guided
make skin preview  # the skin, and every page as surface/build/page_*.png
```

Your own settings (`FORCE`, `SSH_KEY`, `PY`) go in `local.mk` next to the Makefile.

## License

MIT, see [LICENSE](LICENSE). The vendored skin generator and installer in
`third_party/mpc-vst-plugins/` are MIT too (sd88me/mpc-vst-plugins).
