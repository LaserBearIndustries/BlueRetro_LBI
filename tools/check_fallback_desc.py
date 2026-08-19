#!/usr/bin/env python3
"""Checks the built-in Xbox report descriptor against the reports the pad
actually sends.

The descriptor in generic.c is hand written, and a wrong field offset in it
would not fail to build, would not fail to parse, and would not look wrong on
screen - it would quietly put the right value on the wrong axis. The one thing
that can catch that here is decoding it back and comparing the field layout
against a capture of the controller.

    python3 tools/check_fallback_desc.py [trace.bin]

With no trace it checks the layout alone. With one it also confirms that the
bytes which moved in the capture line up with the fields claimed for them.
"""

import io
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'main', 'bluetooth', 'hidp', 'generic.c')

# What the trace showed, and therefore what the descriptor has to say:
# name, bit offset, bit size.
WANT = [
    ('X',      0,  16),
    ('Y',     16,  16),
    ('Z',     32,  16),
    ('Rz',    48,  16),
    ('Brake', 64,  16),
    ('Accel', 80,  16),
    ('Hat',   96,   4),
    ('pad',  100,   4),
    ('Btn',  104,  15),
    ('pad',  119,   1),
]

USAGE = {
    (0x01, 0x30): 'X', (0x01, 0x31): 'Y', (0x01, 0x32): 'Z',
    (0x01, 0x33): 'Rx', (0x01, 0x34): 'Ry', (0x01, 0x35): 'Rz',
    (0x01, 0x39): 'Hat',
    (0x02, 0xC4): 'Accel', (0x02, 0xC5): 'Brake',
}


def load_desc():
    s = io.open(SRC, encoding='utf-8', errors='replace').read()
    m = re.search(r'xb1_fallback_hid_desc\[\]\s*=\s*\{(.*?)\n\};', s, re.S)
    if not m:
        raise SystemExit('descriptor not found in generic.c')
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1)))


def walk(desc):
    """Returns (fields, total_bits, report_id). Only the items this
    descriptor uses; it is not a general HID parser."""
    i = 0
    bit = 0
    size = count = 0
    page = 0
    report_id = None
    pending = []
    fields = []

    while i < len(desc):
        b = desc[i]
        tag, typ, ln = b & 0xF0, (b >> 2) & 0x3, b & 0x3
        ln = 4 if ln == 3 else ln
        data = desc[i + 1:i + 1 + ln]
        val = int.from_bytes(data, 'little') if data else 0
        i += 1 + ln

        if typ == 1:                     # global
            if tag == 0x00:              # usage page
                page = val
            elif tag == 0x70:            # report size
                size = val
            elif tag == 0x90:            # report count
                count = val
            elif tag == 0x80:            # report id
                report_id = val
        elif typ == 2:                   # local
            if tag == 0x00:              # usage
                pending.append((page, val))
            elif tag == 0x10:            # usage minimum
                pending.append(('min', val))
            elif tag == 0x20:            # usage maximum
                pending.append(('max', val))
        elif typ == 0 and tag == 0x80:   # input
            const = bool(val & 1)
            if const:
                fields.append(('pad', bit, size * count))
                bit += size * count
            elif pending and pending[0][0] == 'min':
                fields.append(('Btn', bit, size * count))
                bit += size * count
            else:
                for k in range(count):
                    u = pending[k] if k < len(pending) else pending[-1]
                    fields.append((USAGE.get(u, str(u)), bit, size))
                    bit += size
            pending = []
        elif typ == 0 and tag in (0xA0, 0xC0):
            pending = []

    return fields, bit, report_id


def main():
    desc = load_desc()
    fields, total, report_id = walk(desc)

    print('descriptor: %d bytes, report ID %s, %d input bits (%d bytes)'
          % (len(desc), report_id, total, total // 8))

    bad = 0

    if report_id != 1:
        print('FAIL report ID is %s, the pad sends 1' % report_id)
        bad += 1

    if total != 120:
        print('FAIL describes %d bits, the pad sends 15 bytes = 120' % total)
        bad += 1

    if len(fields) != len(WANT):
        print('FAIL %d fields, expected %d' % (len(fields), len(WANT)))
        for f in fields:
            print('   got %-6s bit %-4d size %d' % f)
        return 1

    for got, want in zip(fields, WANT):
        if got != want:
            print('FAIL %-6s bit %-4d size %-3d   want %-6s bit %-4d size %d'
                  % (got + want))
            bad += 1

    if bad:
        print('\n%d problem%s' % (bad, '' if bad == 1 else 's'))
        return 1

    for name, off, sz in fields:
        print('  %-6s bit %-4d size %-3d  byte %d' % (name, off, sz, off // 8))

    if len(sys.argv) > 1:
        return check_trace(sys.argv[1], fields)

    print('\nlayout matches the capture. Pass a trace to check it against the '
          'bytes that actually moved.')
    return 0


def check_trace(path, fields):
    """Which bytes of the pad's reports ever changed, against which bytes the
    descriptor claims carry something."""
    raw = open(path, 'rb').read()
    off = 0
    first = None
    moved = set()
    n = 0

    while off + 11 <= len(raw):
        (dlen,) = struct.unpack_from('<H', raw, off)
        if dlen < 9 or off + 2 + dlen > len(raw):
            break
        opcode = struct.unpack_from('<H', raw, off + 2)[0]
        payload = raw[off + 11: off + 2 + dlen]
        off += 2 + dlen

        if opcode != 5 or len(payload) < 8:
            continue
        h = struct.unpack_from('<H', payload, 0)[0]
        if ((h >> 12) & 0x3) == 1:
            continue
        if struct.unpack_from('<H', payload, 6)[0] < 0x40:
            continue

        d = payload[8:]
        if len(d) != 17 or d[0] != 0xA1 or d[1] != 0x01:
            continue

        body = d[2:]
        n += 1
        if first is None:
            first = body
            continue
        for k, (a, b) in enumerate(zip(first, body)):
            if a != b:
                moved.add(k)

    if not n:
        print('\nno matching reports in that trace')
        return 0

    claimed = set()
    for name, o, sz in fields:
        if name == 'pad':
            continue
        for bit in range(o, o + sz):
            claimed.add(bit // 8)

    print('\n%d reports; bytes that moved: %s'
          % (n, sorted(moved) if moved else 'none'))

    stray = sorted(moved - claimed)
    if stray:
        print('FAIL bytes moved that the descriptor claims nothing for: %s' % stray)
        return 1

    print('every byte that moved is inside a described field.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
