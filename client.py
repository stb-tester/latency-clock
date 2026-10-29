"""
This script is to be run using Stb-tester.

Copyright (C) 2016 William Manley <will@williammanley.net>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
Boston, MA 02110-1301, USA.
"""

from __future__ import division

import time

import numpy
import stbt
from numpy.lib.recfunctions import merge_arrays

import timeclient


NUM_TIMESTAMPS = 6
SQUARE_SIZE = 8
NUM_SQUARES = 64
NUM_SAMPLES = 50*25


VIDEO_TIMESTAMPS_FIELDS = [
    ("buffer_time", float),
    ("stream_time", float),
    ("running_time", float),
    ("clock_time", float),
    ("render_time", float),
    ("render_realtime", float),
]
VIDEO_TIMESTAMPS = numpy.dtype(VIDEO_TIMESTAMPS_FIELDS)

# clock_offset is the server's realtime clock minus ours, measured with
# timeclient.py; NaN if the server wasn't configured or didn't answer.  The
# latency corrected for the clock offset is
# hw_receive_time - render_realtime + clock_offset.
OWN_TIMESTAMPS_FIELDS = [
    ("hw_receive_time", float),
    ("stbt_receive_time", float),
    ("clock_offset", float),
    ("clock_offset_delay", float),
]
OWN_TIMESTAMPS = numpy.dtype(OWN_TIMESTAMPS_FIELDS)

RECORD = numpy.dtype(VIDEO_TIMESTAMPS_FIELDS + OWN_TIMESTAMPS_FIELDS)

def test_measure_latency():
    """Set the tag `latency_clock_server` to the hostname (or host:port) of the
    machine running the server to measure the offset between its clock and
    ours."""
    data = numpy.ndarray(shape=(NUM_SAMPLES), dtype=RECORD)

    server = stbt.get_config("result.tags", "latency_clock_server", None)
    host, _, port = (server or "").partition(":")
    clock_offset = timeclient.OffsetTracker(
        host or None, int(port or timeclient.DEFAULT_PORT))

    for n, (frame, _) in enumerate(stbt.frames(50)):
        if n >= NUM_SAMPLES:
            break
        own_timestamps = numpy.array(
            [(frame.time, time.time()) + clock_offset.update()],
            dtype=OWN_TIMESTAMPS)
        timestamps = read_timestamps(frame)
        data[n] = merge_arrays([timestamps, own_timestamps], flatten=True,
                               usemask=False)
    clock_offset.close()

    # Sometimes we'll lose a row but no biggie
    numpy.savetxt("latency-test.txt", data[:n])


def read_timestamps(frame):
    startline = (frame.shape[0] - NUM_TIMESTAMPS * SQUARE_SIZE) // 2
    endline = startline + NUM_TIMESTAMPS * SQUARE_SIZE
    startcol = (frame.shape[1] - NUM_SQUARES * SQUARE_SIZE) // 2
    endcol = startcol + NUM_SQUARES * SQUARE_SIZE

    bits = numpy.packbits(
        (frame[startline:endline:8, startcol:endcol:8, 0] & 0x80),
        axis=1)
    timestamps = numpy.fromstring(bits.data, dtype=">u8").astype(float) / 1e9
    return timestamps.view(dtype=VIDEO_TIMESTAMPS)
