# BlueRetro GameCube firmware updater

Pushes a firmware image into the adapter's OTA partition over a GameCube
controller port, with no Bluetooth involved. Meant as a recovery path for when
the Bluetooth stack is the thing that needs replacing.

**Status: untested.** The adapter half builds and has been checked against the
firmware, but this GameCube half has never been compiled or run. Treat the first
run as a bring-up exercise, and keep a working BLE update path available.

## Requirements

- Adapter firmware built with `CONFIG_BLUERETRO_GC_OTA=y`
- devkitPro with devkitPPC and libogc (`gamecube-dev` package)
- A way to run homebrew: Swiss from SD Gecko or SD2SP2

## Build

```
make
```

Produces `blueretro_ota.dol`.

## Use

1. Copy the firmware you want to flash to the root of the card as
   `blueretro.bin`.
2. Copy `blueretro_ota.dol` to the card as well.
3. Launch it from Swiss.

It probes all four ports for an adapter, then streams the image. Do not cut
power while it runs. If it fails partway, the adapter keeps running its current
firmware: the OTA partition being written is the inactive one, and it only
becomes the boot partition once the whole image has been written and verified.

## Protocol

Two vendor SI opcodes, implemented in `main/wired/nsi.c` and
`main/system/gc_ota.c`.

`0x1E` carries data and is never answered, like the existing game id command.
Its payload is `[sub][seq][8 data bytes]`:

| Sub | Meaning |
| --- | --- |
| `0x00` | START, begins an update and resets the sequence counter |
| `0x01` | DATA, eight firmware bytes; `seq` must be the previous one plus one |
| `0x02` | END, flushes the tail, sets the boot partition and restarts |
| `0x03` | ABORT |

`0x1F` is answered with three bytes, `[state][last accepted seq][protocol
version]`, where state is `0` idle, `1` ready, `2` busy, `3` done, `4` error.

The console must stop sending while the adapter reports busy. Writing flash
stalls the cache for tens of milliseconds and nothing can service the SI link
during that, so anything sent meanwhile is simply dropped.

Every accepted frame bumps the sequence number, and the adapter ignores a
sequence it has already taken. That makes retries harmless and lets the console
resynchronise after a busy period by rewinding to the last acknowledged frame
rather than guessing.

## Known rough edges

Areas most likely to need work on first bring-up:

- **SI auto polling.** libogc's PAD driver keeps polling in the background,
  which fights manual `SI_Transfer`. The tool disables polling for the duration
  and re-enables it after; the exact call may need adjusting.
- **Zero length replies.** The data opcode is deliberately not answered, so
  `SI_Transfer` is asked for a zero byte response. If libogc treats that as an
  error or never fires the callback, the data command needs a dummy reply added
  on the adapter side.
- **Throughput.** Eight bytes per transaction is the deliberately dumb starting
  point. Measure before optimising: the flush pacing is expected to dominate, in
  which case a wider payload buys nothing.
