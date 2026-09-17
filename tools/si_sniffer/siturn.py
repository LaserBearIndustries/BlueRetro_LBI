"""Measure the turnaround: how long the device has to answer.

    python siturn.py session.sicap

This is the number the wireless link project lives or dies by. When the
console finishes a request it releases the line, and the device pulls it back
down to start its reply. The gap between those two events is the whole budget
for anything standing in for the cable: radio hop out, device answers, radio
hop back.

The capture records it directly. The last cell of a message is its stop bit,
and that cell's high time runs from the end of the request until whoever
answers pulls the line down, so it is the turnaround, measured rather than
inferred.

It also prints the gap between transactions, which is the other half of the
picture: how much slack there is before the console asks the next thing.

Durations are clamped at 127 ticks (63.5us) by the firmware, so anything at
127 is "longer than this", not a measurement.
"""
import collections
import struct
import sys

MAGIC = b'\xA5\x5A\x17\x53'
HDR = 12
CYCLES_PER_US = 240
CLAMP = 127


def pct(counter, fracs=(0.5, 0.9, 0.99, 1.0)):
    tot = sum(counter.values())
    if not tot:
        return {}
    out = {}
    run = 0
    keys = sorted(counter)
    for f in fracs:
        target = f * tot
        run = 0
        for k in keys:
            run += counter[k]
            if run >= target:
                out[f] = k
                break
    return out


def main():
    path = sys.argv[1]
    blob = open(path, 'rb').read()

    # Turnaround by what the request asked for, because a device may take
    # longer over some commands than others.
    turn = collections.defaultdict(collections.Counter)
    gap = collections.Counter()
    cellhigh = collections.Counter()

    off = 0
    nrec = 0
    while True:
        i = blob.find(MAGIC, off)
        if i < 0 or len(blob) - i < HDR:
            break
        port = blob[i + 4]
        (n,) = struct.unpack_from('<H', blob, i + 6)
        end = i + HDR + n * 2
        if n == 0 or n > 512 or port > 3 or len(blob) < end:
            off = i + 1
            continue
        nrec += 1

        cells = []
        for k in range(n):
            b0 = blob[i + HDR + k * 2]
            b1 = blob[i + HDR + k * 2 + 1]
            cells.append((b0 & 0x7F, b1 & 0x7F))

        # Every cell's high time, to show where a normal cell ends and a
        # turnaround begins - the threshold should come from this, not taste.
        for d0, d1 in cells:
            cellhigh[d1] += 1

        # First byte, decoded from the leading eight cells, names the command.
        cmd = None
        if len(cells) >= 8:
            v = 0
            for d0, d1 in cells[:8]:
                v = (v << 1) | (1 if d1 > d0 else 0)
            cmd = v

        # The first long high in the capture is the request's stop bit, and
        # its length is the turnaround.
        for idx, (d0, d1) in enumerate(cells):
            if d1 >= 10 and idx >= 8:
                turn[cmd][d1] += 1
                break

        # Trailing high, if the capture ended on one, is the wait before the
        # next transaction.
        if cells and cells[-1][1] >= CLAMP:
            gap[cells[-1][1]] += 1

        off = end

    print('records: %d' % nrec)
    print()

    print('=== high time of every cell, ticks of 0.5us ===')
    tot = sum(cellhigh.values())
    for w, c in sorted(cellhigh.items())[:16]:
        bar = '#' * int(60.0 * c / max(cellhigh.values()))
        print('  %3dt %5.1fus %8d %5.1f%%  %s'
              % (w, w * 0.5, c, 100.0 * c / tot, bar))
    print()

    print('=== turnaround, request end to reply start ===')
    for cmd in sorted(turn, key=lambda c: -sum(turn[c].values()))[:6]:
        c = turn[cmd]
        p = pct(c)
        name = {0x00: 'ident', 0x14: 'read', 0x15: 'write'}.get(cmd, '')
        print('  cmd %02X %-6s n=%-7d  median %.1fus  p90 %.1fus  p99 %.1fus  max %.1fus'
              % (cmd, name, sum(c.values()),
                 p.get(0.5, 0) * 0.5, p.get(0.9, 0) * 0.5,
                 p.get(0.99, 0) * 0.5, p.get(1.0, 0) * 0.5))
    print()

    print('=== turnaround detail for the write command ===')
    c = turn.get(0x15, collections.Counter())
    tot = sum(c.values()) or 1
    for w, k in sorted(c.items())[:14]:
        print('  %3dt %5.1fus %8d %5.1f%%' % (w, w * 0.5, k, 100.0 * k / tot))


if __name__ == '__main__':
    main()
