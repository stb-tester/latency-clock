#!/usr/bin/env python3
"""
Measures the offset between this machine's realtime clock and the clock of a
machine running latency-clock's `server` or `dispmanx-server`.

The latency that latency-clock measures is (the time we received a frame,
by our clock) - (the time the frame was displayed, by the server's clock), so
it is only as accurate as the two clocks are in sync.  Rather than relying on
NTP, measure the offset during the test and subtract it:

    latency = receive_time - render_realtime + offset

Like NTP, each request gives

    offset = ((t2 - t1) + (t3 - t4)) / 2
    delay = (t4 - t1) - (t3 - t2)

where t1 and t4 are when we sent the request and received the response (our
clock) and t2 and t3 are when the server received the request and sent the
response (its clock).  The error in the offset is at most delay / 2, so we
use the sample with the smallest delay in the last second or so.

The latency-clock readers (client.py) call `OffsetTracker.update()` once per
frame.  It never blocks: it sends a request and reads whichever responses
have arrived since the last call.  t4 comes from the kernel's receive
timestamp, so it doesn't matter how long a response waits for us to read
it.  Run on its own this prints the offset once a second:

    timeclient.py HOST [PORT]

Copyright (C) 2026 William Manley <will@williammanley.net>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.
"""

import collections
import math
import socket
import struct
import sys
import time

DEFAULT_PORT = 10123

# Not exposed by Python's socket module on all versions:
SO_TIMESTAMPNS = getattr(socket, "SO_TIMESTAMPNS", 35)
TIMESPEC = struct.Struct("@ll")

try:
    time_ns = time.time_ns
except AttributeError:  # Python < 3.7
    def time_ns():
        return int(time.time() * 1e9)


class OffsetTracker(object):
    """`update()` returns (offset, delay) in seconds, where offset is the
    server's clock minus ours, from the sample with the smallest delay in the
    last `window` seconds.  Returns (nan, nan) if we haven't had a response in
    that time, or if `host` is None.
    """
    def __init__(self, host, port=DEFAULT_PORT, window=1.0):
        self._samples = collections.deque()  # (t4, offset, delay) in ns
        self._window_ns = int(window * 1e9)
        self._sock = None
        if host:
            addr = socket.getaddrinfo(host, port, type=socket.SOCK_DGRAM)[0]
            self._sock = socket.socket(addr[0], socket.SOCK_DGRAM)
            self._sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
            self._sock.setblocking(False)
            self._sock.connect(addr[4])

    def close(self):
        if self._sock:
            self._sock.close()

    def update(self):
        if not self._sock:
            return (math.nan, math.nan)
        now = time_ns()
        self._receive()
        try:
            self._sock.send(struct.pack(">Q", time_ns()))
        except OSError:
            pass  # e.g. ECONNREFUSED from an earlier ICMP port unreachable
        while self._samples and self._samples[0][0] < now - self._window_ns:
            self._samples.popleft()
        if not self._samples:
            return (math.nan, math.nan)
        _, offset, delay = min(self._samples, key=lambda s: s[2])
        return (offset / 1e9, delay / 1e9)

    def _receive(self):
        while True:
            try:
                data, ancdata, _, _ = self._sock.recvmsg(
                    64, socket.CMSG_SPACE(TIMESPEC.size))
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                continue  # e.g. ECONNREFUSED: the server isn't running (yet)
            if len(data) < 24:
                continue
            t4 = None
            for level, type_, cdata in ancdata:
                if level == socket.SOL_SOCKET and type_ == SO_TIMESTAMPNS:
                    sec, nsec = TIMESPEC.unpack(cdata[:TIMESPEC.size])
                    t4 = sec * 1000000000 + nsec
            if t4 is None:
                t4 = time_ns()
            t1, t2, t3 = struct.unpack(">QQQ", data[:24])
            self._samples.append(
                (t4, ((t2 - t1) + (t3 - t4)) // 2, (t4 - t1) - (t3 - t2)))


def main(argv):
    host = argv[1]
    port = int(argv[2]) if len(argv) > 2 else DEFAULT_PORT
    tracker = OffsetTracker(host, port)
    next_print = time.time() + 1
    while True:
        offset, delay = tracker.update()
        if time.time() > next_print:
            print("%.6f offset %+.3f ms delay %.3f ms" % (
                time.time(), offset * 1000, delay * 1000), flush=True)
            next_print += 1
        time.sleep(1 / 30)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
