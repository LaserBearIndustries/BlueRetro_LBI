# Frozen

This is the last open version of the BlueRetro Companion app, kept here as
published rather than removed. It works, against the firmware in this repo.

Development continues privately. The firmware does not: the vendor SI opcodes
this uses are all in `main/wired/nsi.c` and the `main/system/gc_*` modules, and
they stay open, so anything else can speak the same protocol.

Each feature carries its own protocol version in its status reply, and the app
checks it before doing anything. That is the compatibility contract between the
two halves now that they version separately.
