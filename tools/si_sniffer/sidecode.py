"""Decode a .sicap capture into SI transactions.

    python sidecode.py capture.sicap [--dump N] [--cmd 15]

A GameCube SI bit is one low-then-high pulse pair: a short low and a long high
is a one, a long low and a short high is a zero. So a bit is decided by which
half is longer, which needs no threshold and gives a free consistency check -
every pair should be low then high, and every cell should be about the same
width.

A message ends with a stop bit: a short low, and then the line is released and
simply stays high until somebody else pulls it down. That gap is several times
a normal cell, which is what makes the boundary findable. Finding it matters,
because a request and the reply to it land in one capture and the stop bit
between them is not data - decoding straight through it shifts every bit after
it by one and produces bytes that look plausible and are wrong.
"""
import collections
import struct
import sys

MAGIC = b'\xA5\x5A\x17\x53'
HDR = 12
CYCLES_PER_US = 240
STOP_HIGH = 12          # ticks of 0.5us, so 6us

CMDS = {
    0x00: 'ident/reset',
    0x14: 'joybus read',
    0x15: 'joybus write',
    0x40: 'pad poll',
    0x41: 'origin',
    0x42: 'calibrate',
    0x43: 'long poll',
    0xFF: 'reset',
}


def records(blob):
    off = 0
    while True:
        i = blob.find(MAGIC, off)
        if i < 0 or len(blob) - i < HDR:
            return
        port = blob[i + 4]
        flags = blob[i + 5]
        (n,) = struct.unpack_from('<H', blob, i + 6)
        (ts,) = struct.unpack_from('<I', blob, i + 8)
        end = i + HDR + n * 2
        if n == 0 or n > 512 or port > 3 or len(blob) < end:
            off = i + 1                    # not a real header
            continue
        cells = []
        for k in range(n):
            b0 = blob[i + HDR + k * 2]
            b1 = blob[i + HDR + k * 2 + 1]
            cells.append((b0 & 0x7F, b1 & 0x7F, b0 >> 7, b1 >> 7))
        yield ts, port, flags, cells
        off = end


def split(cells):
    """Cut a capture into messages at the stop bits."""
    msgs, cur = [], []
    for c in cells:
        cur.append(c)
        if c[1] == 0 or c[1] >= STOP_HIGH:
            msgs.append(cur)
            cur = []
    if cur:
        msgs.append(cur)                   # ran out mid message
    return msgs


def decode_msg(cells):
    """One message: bytes, leftover bits, complaints, cell widths."""
    bits, problems, widths = [], [], []

    for idx, (d0, d1, l0, l1) in enumerate(cells[:-1]):   # last is the stop bit
        if l0 != 0 or l1 != 1:
            problems.append('cell %d levels %d/%d' % (idx, l0, l1))
        widths.append(d0 + d1)
        bits.append(1 if d1 > d0 else 0)

    out = bytearray()
    for k in range(0, len(bits) - 7, 8):
        v = 0
        for b in bits[k:k + 8]:
            v = (v << 1) | b
        out.append(v)

    return bytes(out), len(bits) % 8, problems, widths


def hx(b):
    return ' '.join('%02X' % c for c in b)


def main():
    path = sys.argv[1]
    dump = int(sys.argv[sys.argv.index('--dump') + 1]) if '--dump' in sys.argv else 0
    want = int(sys.argv[sys.argv.index('--cmd') + 1], 16) if '--cmd' in sys.argv else None

    blob = open(path, 'rb').read()
    recs = list(records(blob))
    print('file    : %s (%d bytes)' % (path, len(blob)))
    print('records : %d' % len(recs))
    if not recs:
        return
    span = (recs[-1][0] - recs[0][0]) & 0xFFFFFFFF
    print('span    : %.2f s' % (span / (CYCLES_PER_US * 1e6)))
    print()

    tx = []                 # (ts, [msg, msg, ...])
    per_msg = collections.Counter()
    reqw = collections.Counter()
    rplw = collections.Counter()
    ragged = 0
    problems = 0

    for ts, port, flags, cells in recs:
        msgs = []
        for m in split(cells):
            data, rem, probs, widths = decode_msg(m)
            if rem:
                ragged += 1
            if probs:
                problems += 1
            msgs.append((data, rem, widths))
        per_msg[len(msgs)] += 1
        if msgs:
            for w in msgs[0][2]:
                reqw[w] += 1
            for m in msgs[1:]:
                for w in m[2]:
                    rplw[w] += 1
        tx.append((ts, msgs))

    print('=== messages per capture ===')
    print('  %s' % dict(sorted(per_msg.items())))
    print('  messages with a ragged bit count: %d' % ragged)
    print('  messages with level problems    : %d' % problems)
    print()

    def show(name, c):
        tot = sum(c.values()) or 1
        line = '  '.join('%dt %.0f%%' % (w, 100.0 * n / tot)
                         for w, n in sorted(c.most_common(4)))
        print('  %-22s %s' % (name, line))

    print('=== cell width, ticks of 0.5us (8t = 4us) ===')
    show('console -> device', reqw)
    show('device -> console', rplw)
    print()

    pairs = collections.Counter()
    for ts, msgs in tx:
        if not msgs or not msgs[0][0]:
            continue
        req = msgs[0][0]
        rpl = msgs[1][0] if len(msgs) > 1 else b''
        pairs[(req, rpl)] += 1

    print('=== transactions, request -> reply ===')
    for (req, rpl), c in pairs.most_common(20):
        if want is not None and (not req or req[0] != want):
            continue
        name = CMDS.get(req[0], '') if req else ''
        print('  %-26s -> %-24s %6d  %s'
              % (hx(req)[:26], hx(rpl)[:24] or '(none)', c, name))
    print()

    if dump:
        print('=== first %d captures ===' % dump)
        t0 = tx[0][0]
        for ts, msgs in tx[:dump]:
            us = ((ts - t0) & 0xFFFFFFFF) / CYCLES_PER_US
            parts = ' | '.join('%s%s' % (hx(d) or '-', '+%db' % r if r else '')
                               for d, r, _ in msgs)
            print('  %11.1fus  %s' % (us, parts[:100]))


if __name__ == '__main__':
    main()
