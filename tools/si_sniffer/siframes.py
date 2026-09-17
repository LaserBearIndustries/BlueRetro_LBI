"""Look for structure in the four byte frames crossing the link.

    python siframes.py ingame_to_device.bin [--holes ingame_holes.txt]

Each joybus write moves exactly four bytes, and sistats.py concatenates them
in order, so this file is a sequence of 4 byte frames with nothing between
them. The question is whether those frames are self describing - a sequence
number, a type tag, a fixed cycle - because that is what decides whether a
stand-in for the cable can tolerate being a radio hop out of date.

Dropped records leave holes. A hole loses whole frames rather than shifting
alignment, since each frame is appended as a unit, but it does break any
sequence running through the stream, so runs are measured between holes and
not across them.
"""
import collections
import sys

FRAME = 4


def entropy(counter):
    import math
    tot = sum(counter.values())
    if tot <= 1:
        return 0.0
    h = 0.0
    for v in counter.values():
        p = v / tot
        h -= p * math.log(p, 2)
    return h


def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()
    n = len(data) // FRAME
    frames = [data[i * FRAME:(i + 1) * FRAME] for i in range(n)]

    print('file   : %s' % path)
    print('frames : %d (%d bytes)' % (n, len(data)))
    print()

    # Per position: how much does each byte of the frame actually vary?
    print('=== per byte position ===')
    print('  pos  distinct  entropy  most common values')
    cols = []
    for p in range(FRAME):
        c = collections.Counter(f[p] for f in frames)
        cols.append(c)
        top = ' '.join('%02X:%.0f%%' % (v, 100.0 * k / n)
                       for v, k in c.most_common(4))
        print('  %3d  %8d  %6.2fb  %s' % (p, len(c), entropy(c), top))
    print()

    # A counter shows up as a constant difference from one frame to the next.
    print('=== is any position a counter? step between consecutive frames ===')
    for p in range(FRAME):
        d = collections.Counter((frames[i + 1][p] - frames[i][p]) & 0xFF
                                for i in range(min(n - 1, 200000)))
        top = ' '.join('%+d:%.0f%%' % (v if v < 128 else v - 256,
                                       100.0 * k / sum(d.values()))
                       for v, k in d.most_common(3))
        print('  pos %d  %s' % (p, top))
    print()

    # Whole frame repetition.
    fc = collections.Counter(frames)
    print('=== whole frames ===')
    print('  distinct frames: %d of %d  (%.1f%% unique)'
          % (len(fc), n, 100.0 * len(fc) / n))
    for f, k in fc.most_common(10):
        print('  %s  %7d  %5.2f%%'
              % (' '.join('%02X' % b for b in f), k, 100.0 * k / n))
    print()

    # A fixed cycle would make frame i and frame i+P equal far more often than
    # chance. Try every plausible period.
    print('=== periodicity: how often does frame i equal frame i+P ===')
    best = []
    for period in range(1, 65):
        same = sum(1 for i in range(0, min(n - period, 100000))
                   if frames[i] == frames[i + period])
        tested = min(n - period, 100000)
        best.append((100.0 * same / tested, period))
    best.sort(reverse=True)
    for rate, period in best[:8]:
        print('  period %2d  %5.1f%% identical' % (period, rate))
    print()

    # And at byte granularity, which catches a cycle that only some positions
    # take part in.
    print('=== periodicity per position, best period for each ===')
    for p in range(FRAME):
        scores = []
        for period in range(1, 65):
            tested = min(n - period, 60000)
            same = sum(1 for i in range(tested)
                       if frames[i][p] == frames[i + period][p])
            scores.append((100.0 * same / tested, period))
        scores.sort(reverse=True)
        print('  pos %d  %s' % (p, '  '.join('P%d:%.0f%%' % (pp, r)
                                             for r, pp in scores[:4])))
    print()

    # How long does the stream sit completely still? Long identical runs mean
    # the channel is idling and a stale copy would have been correct anyway.
    print('=== runs of identical consecutive frames ===')
    runs = collections.Counter()
    run = 1
    for i in range(1, n):
        if frames[i] == frames[i - 1]:
            run += 1
        else:
            runs[run] += 1
            run = 1
    runs[run] += 1
    tot = sum(runs.values())
    still = sum(k * v for k, v in runs.items() if k > 1)
    print('  %d runs, longest %d frames' % (tot, max(runs)))
    print('  %d of %d frames (%.1f%%) repeat the frame before them'
          % (still - sum(v for k, v in runs.items() if k > 1), n,
             100.0 * (still - sum(v for k, v in runs.items() if k > 1)) / n))
    for length, k in sorted(runs.items())[:8]:
        print('    run of %-4d  %7d times' % (length, k))


if __name__ == '__main__':
    main()
