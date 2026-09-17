"""Census a large .sicap capture and reassemble what crossed the link.

    python sistats.py session.sicap [--out prefix]

sidecode.py keeps every record to print detail; that does not scale to the
tens of megabytes a multiboot produces, so this streams instead and keeps only
counters plus the reconstructed payloads.

With --out it writes two files: the bytes the console wrote to the device, in
order, and the bytes the device returned. Drops are marked in a sidecar list
rather than silently closing the gap, because a multiboot image with an
unannounced hole in it is worse than one with a known hole.

The timestamp is a 32 bit CPU cycle counter, which wraps every 17.9s at
240MHz, so elapsed time is accumulated from differences rather than read
directly.
"""
import collections
import struct
import sys

MAGIC = b'\xA5\x5A\x17\x53'
HDR = 12
CYCLES_PER_US = 240
STOP_HIGH = 10          # 5us: see siturn.py, cells end by 4us and
                        # turnarounds start at 5.5us

CMDS = {
    0x00: 'ident/reset',
    0x14: 'joybus read',
    0x15: 'joybus write',
    0x40: 'pad poll',
    0x41: 'origin',
    0xFF: 'reset',
}


def messages(cells):
    """Split a capture at the stop bits, decode each message to bytes."""
    out = []
    cur = []
    for c in cells:
        cur.append(c)
        if c[1] == 0 or c[1] >= STOP_HIGH:
            out.append(cur)
            cur = []
    if cur:
        out.append(cur)

    msgs = []
    for m in out:
        bits = [1 if d1 > d0 else 0 for d0, d1 in m[:-1]]
        data = bytearray()
        for k in range(0, len(bits) - 7, 8):
            v = 0
            for b in bits[k:k + 8]:
                v = (v << 1) | b
            data.append(v)
        msgs.append((bytes(data), len(bits) % 8, [d0 + d1 for d0, d1 in m[:-1]]))
    return msgs


def main():
    path = sys.argv[1]
    prefix = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None

    blob = open(path, 'rb').read()
    print('file    : %s (%d bytes)' % (path, len(blob)))

    shapes = collections.Counter()
    cmds = collections.Counter()
    replies = collections.Counter()
    reqw = collections.Counter()
    rplw = collections.Counter()
    ragged = 0
    truncated = 0
    drops = 0
    nrec = 0

    wrote = bytearray()          # console -> device payload
    read = bytearray()           # device -> console payload
    holes = []
    per_sec = collections.defaultdict(collections.Counter)

    prev_ts = None
    elapsed = 0

    off = 0
    while True:
        i = blob.find(MAGIC, off)
        if i < 0 or len(blob) - i < HDR:
            break
        port = blob[i + 4]
        flags = blob[i + 5]
        (n,) = struct.unpack_from('<H', blob, i + 6)
        (ts,) = struct.unpack_from('<I', blob, i + 8)
        end = i + HDR + n * 2
        if n == 0 or n > 512 or port > 3 or len(blob) < end:
            off = i + 1
            continue

        cells = []
        for k in range(n):
            b0 = blob[i + HDR + k * 2]
            b1 = blob[i + HDR + k * 2 + 1]
            cells.append((b0 & 0x7F, b1 & 0x7F))

        nrec += 1
        if flags & 1:
            drops += 1
            holes.append((len(wrote), len(read)))
        if flags & 2:
            truncated += 1

        if prev_ts is not None:
            elapsed += (ts - prev_ts) & 0xFFFFFFFF
        prev_ts = ts
        sec = int(elapsed / (CYCLES_PER_US * 1000000))

        msgs = messages(cells)
        req = msgs[0][0] if msgs else b''
        rpl = msgs[1][0] if len(msgs) > 1 else b''

        for _, rem, widths in msgs:
            if rem:
                ragged += 1
        if msgs:
            for w in msgs[0][2]:
                reqw[w] += 1
            for m in msgs[1:]:
                for w in m[2]:
                    rplw[w] += 1

        if req:
            cmds[req[0]] += 1
            per_sec[sec][req[0]] += 1
            shapes[(req[0], len(req), len(rpl))] += 1
            if rpl:
                replies[(req[0], rpl[0] if len(rpl) == 1 else None)] += 1

            # The payload each side actually moved.
            if req[0] == 0x15 and len(req) >= 5:
                wrote += req[1:5]
            if req[0] == 0x14 and len(rpl) >= 4:
                read += rpl[:4]

        off = end

    print('records : %d' % nrec)
    print('span    : %.1f s' % (elapsed / (CYCLES_PER_US * 1e6)))
    print('drops   : %d records followed a gap' % drops)
    print('cut off : %d records filled the memory' % truncated)
    print('ragged  : %d messages did not end on a byte' % ragged)
    print()

    print('=== commands seen ===')
    for c, k in cmds.most_common():
        print('  %02X  %-14s %8d' % (c, CMDS.get(c, ''), k))
    print()

    print('=== transaction shapes: cmd, request bytes, reply bytes ===')
    for (c, rq, rp), k in shapes.most_common(12):
        print('  %02X  req %2d  rpl %2d   %8d  %s' % (c, rq, rp, k, CMDS.get(c, '')))
    print()

    print('=== single byte replies by command ===')
    for (c, r), k in replies.most_common(10):
        if r is not None:
            print('  %02X -> %02X   %8d' % (c, r, k))
    print()

    def show(name, cnt):
        tot = sum(cnt.values()) or 1
        print('  %-20s %s' % (name, '  '.join(
            '%dt %.0f%%' % (w, 100.0 * v / tot) for w, v in sorted(cnt.most_common(5)))))

    print('=== cell width, ticks of 0.5us ===')
    show('console -> device', reqw)
    show('device -> console', rplw)
    print()

    print('=== what happened when, by second ===')
    for sec in sorted(per_sec)[:80]:
        c = per_sec[sec]
        if not c:
            continue
        top = ' '.join('%02X:%d' % (k, v) for k, v in c.most_common(3))
        print('  %4ds  %s' % (sec, top))
    print()

    print('=== payload moved ===')
    print('  console -> device : %d bytes' % len(wrote))
    print('  device -> console : %d bytes' % len(read))
    if holes:
        print('  %d gaps, first at write offset %d' % (len(holes), holes[0][0]))

    if prefix:
        open(prefix + '_to_device.bin', 'wb').write(wrote)
        open(prefix + '_to_console.bin', 'wb').write(read)
        with open(prefix + '_holes.txt', 'w') as fh:
            for w, r in holes:
                fh.write('%d %d\n' % (w, r))
        print('  written: %s_to_device.bin, %s_to_console.bin, %s_holes.txt'
              % (prefix, prefix, prefix))


if __name__ == '__main__':
    main()
