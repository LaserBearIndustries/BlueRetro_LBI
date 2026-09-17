# What the GameCube to GBA link actually does

Measured from Zelda: Four Swords Adventures on real hardware, 17 September
2026. Two captures: a five minute session covering boot, multiboot and play
(273,385 transactions), and a two minute sample taken while the game was fully
running with active content on the GBA screen (115,257 transactions). Nothing
here is inferred from documentation.

## The protocol

Three commands carry everything.

| request | reply | what it is |
| --- | --- | --- |
| `00` | `00 04 08` | ident. `00 04` is the device, `08` its status |
| `FF` | `00 04 08` | reset, same answer |
| `15 xx xx xx xx` | `32` | write four bytes, get a status back |
| `14` | `xx xx xx xx 18` | read four bytes and a status |

## The turnaround, which is the constraint

When the console finishes a request it releases the line, and the device pulls
it back down to reply. The gap between those two events is the entire budget
for anything standing in for the cable.

| command | samples | median | p99 | max |
| --- | --- | --- | --- | --- |
| `00` ident | 135,112 | 6.0us | 6.5us | 6.5us |
| `15` write | 131,975 | 6.0us | 6.5us | 6.5us |
| `14` read | 5,029 | 6.0us | 6.5us | 6.5us |

6.5us worst case over a quarter of a million transactions, and the in-game
capture reproduces it exactly. **A transparent radio bridge is impossible** -
not difficult, impossible by three orders of magnitude. Whatever sits in the
console port has to answer locally, from state, in 6.5us.

## Bit timing differs by direction

| direction | cell width |
| --- | --- |
| console to device | 5us, 80-88% of cells |
| device to console | 4us (63%) and 3.5us (37%) |

So a stand-in must answer at the speed the GBA answers, not the speed it was
asked. This is measured from pulse durations, not assumed.

## The rate is fixed, and content does not change it

| | after boot | in game, GBA screen active |
| --- | --- | --- |
| idents/s | ~500 | 479 |
| writes/s | ~480 | 461 |
| reads/s | ~19 | 17.7 |
| console to device | ~1.9 kB/s | 1,845 B/s |
| device to console | ~76 B/s | 70 B/s |

Identical. The link is a constant rate polled channel, not demand driven, and
activity on the GBA screen does not touch it.

That also settles what the link is *not* carrying. 1.85 kB/s cannot move a
240x160 screen. The GBA renders locally from the program it was handed at
multiboot; the link carries game state only.

## Multiboot

Seconds 14 to 17 of the session: 2,812 writes of four bytes, a little under
four seconds, about 11 kB. That is the GBA side of the game being handed over.
Peak rate 1,365 writes/s, so 5.5 kB/s - the busiest the link ever gets.

## The status byte is a toggle, not flow control

This mattered, because a status that reports congestion could not be
synthesised by a stand-in. It does not.

During multiboot the write status alternates almost exactly evenly between
`32` and `22`, which differ only in bit 4:

| second | `32` | `22` |
| --- | --- | --- |
| 14 | 118 | 120 |
| 15 | 683 | 680 |
| 16 | 311 | 311 |
| 17 | 291 | 291 |
| 19 on | hundreds | 0 |

A per-transfer toggle during bulk transfer, constant `32` for the whole of
gameplay. Reproducible locally.

## What this means for a wireless link

Feasible, but only as a state caching bridge, never as a tunnel.

- The side in the console port answers in 6.5us from cached state. BlueRetro
  already does exactly this on a controller port sixty times a second, so the
  hard real time part is solved and shipping.
- Nearly every reply is fixed: `00 04 08` to every ident, `32` to 99.8% of
  writes in gameplay. Synthesised, not relayed.
- Only `14` read carries device dependent data, at 18 a second, 70 bytes a
  second. Any radio carries that with room to spare.
- Peak demand is the multiboot at 5.5 kB/s for four seconds, and it tolerates
  being handed over before play starts.

The open question is no longer electrical or bandwidth. It is whether the
game's own protocol tolerates its state being a hop stale - which needs the
reconstructed payload streams read, not more capture.
