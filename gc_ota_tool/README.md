# BlueRetro GameCube firmware updater

Pushes a firmware image into the adapter's OTA partition over a GameCube
controller port, with no Bluetooth involved. Meant as a recovery path for when
the Bluetooth stack is the thing that needs replacing.

Tested end to end on HW2 GameCube hardware: full image transferred, verified and
booted.

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

It probes all four ports for an adapter, then shows what is installed against
what is on the card before doing anything:

```
  installed : v26.08_LBI-2-g327ade5
  on card   : v26.10_LBI
  image     : BlueRetro_hw2_gamecube, 609536 bytes

Press A to flash, B to cancel.
```

The installed version comes from the adapter over opcode `0x20`. The card
version is read out of the image's own `esp_app_desc_t` rather than its
filename, so a file that is not adapter firmware is called out instead of being
streamed blindly. Matching versions are pointed out, which is the usual sign of
a stale copy on the card.

Do not cut power while it runs. If it fails partway the adapter keeps running
its current firmware: the partition being written is the inactive one, and it
only becomes the boot partition once the whole image is written and verified.

## Protocol

Three vendor SI opcodes, implemented in `main/wired/nsi.c` and
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

`0x20` takes a one byte slice index and answers with eight bytes of the running
firmware's version string. Four slices cover the 32 byte field, because a whole
SI reply has to fit in 128 bits.

The console must stop sending while the adapter reports busy. Writing flash
stalls the cache for tens of milliseconds and nothing can service the SI link
during that, so anything sent meanwhile is simply dropped.

Every accepted frame bumps the sequence number, and the adapter ignores a
sequence it has already taken. That makes retries harmless and lets the console
resynchronise after a busy period by rewinding to the last acknowledged frame
rather than guessing.

## Notes

Two assumptions that first bring-up settled, worth recording since they shaped
the design:

- **Zero length replies work.** The data opcode is deliberately not answered and
  `SI_Transfer` is asked for a zero byte response. That turned out fine, so the
  adapter needs no dummy reply.
- **Disabling SI polling is sufficient.** libogc's PAD driver polls in the
  background, which would fight manual transfers. Turning polling off for the
  duration is enough. The tool hands the bus back for the confirmation prompt,
  since reading buttons needs polling, and takes it again before transferring.

Throughput has not been optimised. Eight bytes per transaction is the
deliberately dumb starting point, and flush pacing is expected to dominate, so a
wider payload may buy nothing. Measure before changing it.
