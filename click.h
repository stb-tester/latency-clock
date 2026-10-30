/* latency-clock
 * Copyright (C) 2026 William Manley <will@williammanley.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#ifndef _CLICK_H_
#define _CLICK_H_

#include <pthread.h>
#include <stdint.h>

/* The display's vsyncs, shared with the click thread: vsync k happens at
 * t0 + a + period * k ns (CLOCK_MONOTONIC), and the frames drawn for it say
 * they were displayed at that plus offset_ns. */
typedef struct {
  pthread_mutex_t lock;
  int ready;
  int64_t t0;
  double a, period;
  int64_t offset_ns;
} VsyncSchedule;

/* Plays a short click on ALSA `device` at the time of every `every`th vsync
 * (those with k % every == 0), from a background thread.  Returns 0 on
 * success, -1 on failure (with a message printed to stderr). */
int click_start (const char *device, int every, VsyncSchedule *schedule);

#endif
