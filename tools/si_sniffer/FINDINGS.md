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

## Frame structure, and how much of it is actually new

Each write moves four bytes. `siframes.py` concatenates them and looks for
structure; 55,362 frames from the in-game capture.

There is a **26 frame cycle**: 67.6% of frames equal the frame 26 later, 62.4%
at 52. That number is not arbitrary - 461 writes/s over 17.7 reads/s is 26
writes per read, so one round is 26 writes and one read, repeating 17.7 times a
second.

A round looks like this, and the shape is consistent:

```
00000010 67001850 6010BC4F              marker and header
EC497800 EC5671F0 EC687100 EC7A7108     group, tag byte EC
43677720 43557720 43437720 43367E20     group, tag byte 43
00E08001 x6  00E08009  00E08001 ...     the rest of the round
```

Roughly ten frames of payload, then the balance filled with two near-identical
keepalive frames - `00 E0 80 01` and `00 E0 80 09`, which differ in one bit and
are 70% of the entire stream between them. The tagged groups repeat unchanged
from one round to the next; only the marker frames vary.

So the byte rate badly overstates the information rate:

| | raw | actually new |
| --- | --- | --- |
| console to device | 1,845 B/s | **356 B/s** changed against the previous cycle |
| keepalive share | | 71% of frames |
| distinct frames | | 5,713 of 55,362, 10.3% |

## The return channel is tiny, and that is the one that matters

Only `14` read carries data a stand-in could not invent, so this is the
direction that has to cross the radio. It is almost nothing:

- 17.5 frames/s, 70 B/s raw
- **39 distinct values, ever**, in 2,105 frames
- 88.5% of frames repeat the frame before them, so about **two state changes a
  second**

All of the form `01 xx 80 yy`, with the top three by share being `01 00 80 4C`
(33%), `01 80 80 55` (24%) and `01 40 80 A6` (18%).

Two changes a second out of a 39 value alphabet. Being one radio hop stale
means being tens of milliseconds behind a value that changes twice a second.

## Where this leaves the project

The staleness question is answered as far as this game and this session go. The
forward direction is 71% keepalive and 356 B/s of real change; the return
direction is 2 changes a second. Nothing here needs a round trip inside 6.5us -
only the electrical reply does, and that is synthesised locally from cached
state, which BlueRetro already does on a controller port.

Caveat worth keeping: this is one game and two minutes of one session. A busier
moment - combat, an item changing hands, a screen transition - could spike, and
the honest version of this claim is that it holds for what was on screen while
the capture ran.

---

# Correction: the rates above are undercounts

Everything in this document dated 17 September was measured from a capture that
was quietly losing transactions, and the per-second figures are wrong by a
factor of about 3.4. Recorded here rather than edited away, because the way it
went wrong is worth knowing.

The bus runs at **3,239 transactions/s**, which at ~110 bytes of capture per
transaction is about 400 kB/s. The serial link carries 92 kB/s at 921600 baud.
So the firmware dropped in long episodes, and only 387 records carried the drop
flag because the flag marks an episode, not each record lost inside it.

The giveaway was the spacing histogram: gaps between consecutive captured
transactions cluster at 200-250us and 300-350us, which are exactly an ident
transaction (151us of wire time) and a write transaction (247us) plus about
75us of idle. Transactions run back to back. There is no inter-round idle for a
960/s average to hide in, so the average had to be wrong.

Measured instead from the 337 stretches that carry no drop flag, where every
transaction that happened is present:

| | first reported | actual |
| --- | --- | --- |
| ident | ~479/s | **1,615/s** |
| write | ~461/s | **1,556/s, 6.2 kB/s** |
| read | ~17.5/s | **60/s, 0.24 kB/s** |
| writes per read | 26 | 26.1 |

Scaling the rest by the same factor: about **1.2 kB/s** of genuinely changed
content downstream rather than 356 B/s, and about **7 state changes/s** on the
return channel rather than 2. Multiboot is bus limited rather than rate limited
- writes back to back at 322us is 3,100/s, so **up to ~12 kB/s** at peak.

What survives unchanged, because it is measured inside one transaction or is a
ratio: the 6.5us turnaround, the cell widths per direction, the command set,
the status toggle, the 26 frame cycle, the 39 value return channel, and the
keepalive share. The frame sequence analysis also survives, because drops come
in episodes rather than at random: inside a clean run of up to 458 records the
write stream is contiguous, which is why a 26 frame period was visible at all.

**Practical consequence: 921600 baud is not enough to capture this bus.** A
complete capture needs about 400 kB/s, so at least 4 Mbaud, or fewer bytes per
transaction on the wire. Any future session should raise the baud first and
confirm the drop count is zero before trusting a rate.
