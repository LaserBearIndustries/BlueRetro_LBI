# BlueRetro Companion

GameCube homebrew that talks to a BlueRetro adapter over a controller port.
Remap a controller on the TV, watch what the adapter is actually receiving, and
update its firmware, none of which needs a phone, a serial adapter, or opening
anything.

Tested end to end on HW2 GameCube hardware.

## Requirements

- Adapter firmware built with `CONFIG_BLUERETRO_GC_APP` for remapping and the
  input viewer, and `CONFIG_BLUERETRO_GC_OTA` for firmware updates
- devkitPro with devkitPPC and libogc (`gamecube-dev` package)
- A way to run homebrew: Swiss from SD Gecko or SD2SP2

## Build

```
make
```

Produces `blueretro_companion.dol`.

## Use

Copy `blueretro_companion.dol` to the SD card and launch it from Swiss. Any
controller port can drive the menus, not just port 1, which matters after
remapping a pad onto another port.

### Remap a controller

Walks through the GameCube's controls and asks which control on your pad should
drive each one. The pad stops driving the game while it does, so pressing a
button answers the prompt without also acting on it.

It first asks how your triggers work, because controllers differ and the right
answer is not guessable:

| Choice | For |
| --- | --- |
| Analog, click derived at 95% | Xbox and similar, analog with no switch at the bottom |
| Analog plus a real full pull click | GameCube and NSO pads, which have a switch under the trigger |
| Digital only | Switch Pro and similar, where L and R are plain buttons |

Sticks and triggers capture as whole controls: one sweep of a stick fills all
four directions. Tap a control to map it, hold anything to cancel. Capture is on
release rather than press, so no button has to be reserved for cancelling and
every button stays mappable.

Combos are added automatically, so pairing, reset and power off keep working
after a remap.

### Live input viewer

Shows the raw controller state as the adapter sees it, before any mapping: which
generic buttons are pressed and where the axes sit. Useful for working out why a
controller behaves oddly, and for seeing what a button is called. Hold Start to
leave.

### Update firmware

Put the firmware on the card root as `blueretro.bin`. The app finds the adapter,
shows the installed version against the one on the card, and asks before doing
anything. Settings and pairings are kept.

Interrupting a transfer is safe: the image goes to the inactive OTA partition
and only becomes bootable once it has been written and verified in full, so a
failed attempt leaves the adapter on its current firmware.

If it reports that no adapter answered, the installed firmware predates these
opcodes. Flash once over serial and it will work from then on.

## Protocol

Vendor SI opcodes, implemented in `main/wired/nsi.c`, `main/system/gc_ota.c` and
`main/system/gc_app.c`.

| Opcode | Direction | Purpose |
| --- | --- | --- |
| `0x1E` | write | Firmware data: `[sub][seq][8 bytes]` |
| `0x1F` | read | OTA status: `[state][last seq][protocol version]` |
| `0x20` | read | Running firmware version, eight bytes per slice |
| `0x21` | read | Raw controller state, pre mapping |
| `0x22` | write | Staged mapping writes |
| `0x23` | write | Hold a port's output neutral while capturing |

A whole SI reply has to fit in 128 bits on the GameCube ports, which is why the
version and the mapping arrive in slices rather than in one go.

The console must stop sending while the adapter reports busy. Writing flash
stalls the cache for tens of milliseconds and nothing can service the SI link
meanwhile, so anything sent during a flush is dropped. Every accepted frame
bumps a sequence number and a repeat is ignored, so retries are harmless and the
console can resynchronise from the last acknowledged frame.

## Notes

Two assumptions that first bring-up settled, worth recording since they shaped
the design:

- **Zero length replies work.** The write opcodes are deliberately unanswered
  and `SI_Transfer` is asked for a zero byte response, which turned out fine.
- **Disabling SI polling is sufficient.** libogc's PAD driver polls in the
  background and would fight manual transfers. Turning polling off for the
  duration is enough; the app hands the bus back for menus and prompts that need
  to read the GameCube pad.

Throughput has not been optimised. Eight bytes per transaction is a deliberately
dumb starting point and flush pacing is expected to dominate, so a wider payload
may buy nothing. Measure before changing it.
