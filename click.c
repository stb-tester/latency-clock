/* latency-clock
 * Copyright (C) 2026 William Manley <will@williammanley.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * Plays a click at the same time as particular vsyncs, so that a capture
 * device's audio timestamps can be compared with its video timestamps.
 *
 * Before writing each period of audio we ask ALSA how many frames are queued
 * ahead of it (snd_pcm_delay()), which tells us when the next frame we write
 * will be played, and put the click at the right sample.  So the accuracy
 * depends on the driver's delay accounting, plus any fixed delay after the
 * driver (e.g. in the HDMI encoder).
 */
#include "click.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if __has_include(<alsa/asoundlib.h>)
#include <alsa/asoundlib.h>
#else
/* The bits of alsa-lib's API that we use, for building on systems with
 * libasound2 but not libasound2-dev (link with -l:libasound.so.2). */
typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
typedef enum { SND_PCM_STREAM_PLAYBACK = 0 } snd_pcm_stream_t;
typedef enum { SND_PCM_FORMAT_S16_LE = 2 } snd_pcm_format_t;
typedef enum { SND_PCM_ACCESS_RW_INTERLEAVED = 3 } snd_pcm_access_t;
int snd_pcm_open (snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream,
    int mode);
int snd_pcm_set_params (snd_pcm_t *pcm, snd_pcm_format_t format,
    snd_pcm_access_t access, unsigned int channels, unsigned int rate,
    int soft_resample, unsigned int latency);
snd_pcm_sframes_t snd_pcm_writei (snd_pcm_t *pcm, const void *buffer,
    snd_pcm_uframes_t size);
int snd_pcm_delay (snd_pcm_t *pcm, snd_pcm_sframes_t *delayp);
int snd_pcm_recover (snd_pcm_t *pcm, int err, int silent);
const char *snd_strerror (int errnum);
#endif

#define RATE 48000
#define CHANNELS 2
#define PERIOD 240              /* 5ms */
#define LATENCY_US 40000
#define CLICK_LEN 96            /* 2ms */
#define CLICK_HZ 3000
/* Starts at its peak, so the onset is the first sample: */
#define CLICK_SAMPLE(i) \
  ((int16_t) (0.8 * 32767 * cos (2 * M_PI * CLICK_HZ * (i) / RATE)))

typedef struct {
  snd_pcm_t *pcm;
  int every;
  VsyncSchedule *schedule;
} Click;

static int64_t
monotonic_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static void *
click_thread (void *arg)
{
  Click *c = arg;
  static int16_t buf[PERIOD * CHANNELS];
  uint64_t written = 0;         /* frames written so far */
  int64_t click_at = -1;        /* frame index of the next click, or -1 */
  int64_t last_k = -1;

  while (1) {
    snd_pcm_sframes_t delay = 0, n;
    int64_t now, t_next;
    int i, err;

    err = snd_pcm_delay (c->pcm, &delay);
    now = monotonic_ns ();
    if (err < 0) {
      snd_pcm_recover (c->pcm, err, 1);
      delay = 0;
    }
    /* When the next frame we write will be played: */
    t_next = now + (int64_t) delay * 1000000000ll / RATE;

    pthread_mutex_lock (&c->schedule->lock);
    if (c->schedule->ready && click_at < 0) {
      const VsyncSchedule *s = c->schedule;
      int64_t k = (int64_t) ceil ((t_next - s->t0 - s->a) / s->period);
      int64_t tk;
      k = (k + c->every - 1) / c->every * c->every;
      tk = s->t0 + llround (s->a + s->period * k) + s->offset_ns;
      /* Schedule it once it's within the next couple of periods: */
      if (k != last_k
          && tk < t_next + 2ll * PERIOD * 1000000000ll / RATE) {
        click_at = written + llround ((tk - t_next) * (double) RATE / 1e9);
        last_k = k;
        printf ("click vsync %" PRId64 " at %" PRId64 " ns (monotonic), "
            "frame %" PRId64 ", ALSA delay %ld frames\n", k, tk, click_at,
            (long) delay);
        fflush (stdout);
      }
    }
    pthread_mutex_unlock (&c->schedule->lock);

    for (i = 0; i < PERIOD; i++) {
      int64_t j = (int64_t) (written + i) - click_at;
      int16_t v = (click_at >= 0 && j >= 0 && j < CLICK_LEN) ?
          CLICK_SAMPLE (j) : 0;
      buf[i * CHANNELS] = buf[i * CHANNELS + 1] = v;
    }
    n = snd_pcm_writei (c->pcm, buf, PERIOD);
    if (n < 0) {
      fprintf (stderr, "click: write failed: %s\n", snd_strerror (n));
      snd_pcm_recover (c->pcm, n, 1);
      continue;
    }
    written += n;
    if (click_at >= 0 && (int64_t) written >= click_at + CLICK_LEN)
      click_at = -1;
  }
  return NULL;
}

int
click_start (const char *device, int every, VsyncSchedule *schedule)
{
  static Click c;
  pthread_t thread;
  int err;

  c.every = every;
  c.schedule = schedule;
  if ((err = snd_pcm_open (&c.pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0
      || (err = snd_pcm_set_params (c.pcm, SND_PCM_FORMAT_S16_LE,
              SND_PCM_ACCESS_RW_INTERLEAVED, CHANNELS, RATE, 1,
              LATENCY_US)) < 0) {
    fprintf (stderr, "click: can't open ALSA device %s: %s\n", device,
        snd_strerror (err));
    return -1;
  }
  if (pthread_create (&thread, NULL, click_thread, &c) != 0) {
    fprintf (stderr, "click: failed to create thread\n");
    return -1;
  }
  pthread_detach (thread);
  fprintf (stderr, "Clicking on %s every %d vsyncs\n", device, every);
  return 0;
}
