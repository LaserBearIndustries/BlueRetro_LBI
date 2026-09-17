"""Live capture from the SI sniffer firmware.

Reads the serial stream, writes every byte to a file, and prints a running
summary so you can tell from across the room whether the bus is actually
saying anything.

    python sicap.py COM38 capture.sicap [seconds]

Ctrl-C stops it. The file it writes is the raw stream, so sidecode.py can be
run on it afterwards as many times as you like.
"""
import collections
import sys
import time

try:
    import serial
except ImportError:
    sys.exit('needs pyserial: use the ESP-IDF python, which already has it')

MAGIC = b'\xA5\x5A\x17\x53'
HDR = 12


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM38'
    out = sys.argv[2] if len(sys.argv) > 2 else 'capture.sicap'
    limit = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0

    # No DTR or RTS: RTS drives EN on this probe, so letting the driver assert
    # it would reset the board just by opening the port - and a reset in the
    # middle of a game is the one thing that ruins a capture.
    sp = serial.Serial()
    sp.port = port
    # Must match SI_BAUD in main/si_sniffer.c.
    sp.baudrate = 2000000
    sp.timeout = 0.2
    sp.dtr = False
    sp.rts = False
    sp.open()
    sp.dtr = False
    sp.rts = False

    fh = open(out, 'wb')
    buf = bytearray()
    start = time.time()
    last = start
    total = 0
    recs = 0
    dropped = 0
    sizes = collections.Counter()

    print('capturing from %s to %s, ctrl-c to stop' % (port, out))

    try:
        while True:
            chunk = sp.read(8192)
            if chunk:
                fh.write(chunk)
                total += len(chunk)
                buf += chunk

                # Walk whole records out of the buffer for the live counts.
                while True:
                    i = buf.find(MAGIC)
                    if i < 0 or len(buf) - i < HDR:
                        break
                    n = buf[i + 6] | (buf[i + 7] << 8)
                    end = i + HDR + n * 2
                    if len(buf) < end:
                        break
                    if buf[i + 5] & 1:
                        dropped += 1
                    recs += 1
                    sizes[n] += 1
                    del buf[:end]

                if len(buf) > 1 << 16:
                    del buf[:-1024]

            now = time.time()
            if now - last >= 2.0:
                top = ', '.join('%ditems x%d' % (k, v)
                                for k, v in sizes.most_common(4))
                print('%6.1fs  %8d B  %6d records  %s%s'
                      % (now - start, total, recs, top or 'nothing yet',
                         '  DROPS:%d' % dropped if dropped else ''))
                last = now

            if limit and (now - start) >= limit:
                break
    except KeyboardInterrupt:
        pass
    finally:
        sp.close()
        fh.close()

    print()
    print('wrote %d bytes, %d records to %s' % (total, recs, out))
    if dropped:
        print('%d records followed a drop: the ring filled' % dropped)


if __name__ == '__main__':
    main()
