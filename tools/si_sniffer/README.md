# SI sniffer

A listen-only GameCube SI capture that streams live over the esp-prog, built
to reverse engineer the GameCube to GBA link.

It is its own firmware rather than a BlueRetro build option, because as a
BlueRetro option it does not fit: that image has 326 bytes of IRAM free, every
translation unit in `main/wired/` is mapped into IRAM, and its capture lands in
a 128kB memory card buffer that has to be downloaded through the companion app
afterwards. Here there is no Bluetooth stack, no memory card and no vendor
opcodes, so the capture streams out for as long as you leave it running.

It also records the RMT's own pulse durations rather than decoded bytes. That
distinction matters: a capture that starts half a bit late still decodes into
plausible looking bytes, and keeping the timings is the only way to tell.

The RMT output is never routed to the pin, so it cannot drive the bus.

## Use

    # build and flash, listening to console port 1
    idf.py -DSI_PORT=0 -p COM38 -b 921600 build flash

    # capture, ctrl-c to stop
    python sicap.py COM38 session.sicap

    # decode
    python sidecode.py session.sicap --dump 40

`SI_PORT` is zero based: 0 is the port marked 1 on the console. The pins are
BlueRetro hw2's, 19/5/26/27.

Use the ESP-IDF python for the host scripts - it already has pyserial.

## Reading the output

`sidecode.py` splits each capture at the stop bits, so a request and the reply
it provoked come out as separate messages. It prints the cell width for each
direction separately, which is worth watching: the two directions do not
necessarily run at the same rate.

## Restoring the adapter

This replaces BlueRetro entirely, including the partition table. Flash
BlueRetro normally to put it back; the SPIFFS region it keeps its config in
sits above what this writes and is left alone.
