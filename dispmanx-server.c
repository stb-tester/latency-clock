/* latency-clock
 * Copyright (C) 2026 William Manley <will@williammanley.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * A server for the Raspberry Pi's legacy (non-KMS) display stack that draws
 * the same timestamps as `server`, but locked to the display's vsync.
 *
 * `server` draws the time at which GStreamer hands each frame to the display.
 * The frame isn't scanned out until the next vsync after that, and as
 * GStreamer's frame clock and the HDMI pixel clock aren't locked together
 * the wait drifts through 0 to 1 frame and wraps.  That shows up as a
 * sawtooth in the measured latency.
 *
 * Here we get a callback on every vsync, fit a line through the callback
 * times to predict future vsyncs, and draw each frame with the predicted time
 * of the vsync at which it will start being scanned out.  We check when each
 * frame was actually applied, and log any that missed their vsync.
 */
#include <bcm_host.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "timeserver.h"

#define WIDTH 640
#define HEIGHT 240
#define PITCH (WIDTH * 2)
#define NUM_TIMESTAMPS 6
#define STAMP_TOP ((HEIGHT - NUM_TIMESTAMPS * 8) / 2)
#define STAMP_LEFT ((WIDTH - 64 * 8) / 2)
#define NUM_BUFFERS 3

/* Number of vsyncs to fit the line through: */
#define FIT_WINDOW 600
/* Number of vsyncs to see before we start drawing: */
#define FIT_MIN 60
/* Callbacks further than this from the fitted line are left out of the fit
 * (they're late deliveries, not late vsyncs): */
#define FIT_OUTLIER_NS 2000000

static volatile sig_atomic_t quit = 0;

static void
on_signal (int sig)
{
  quit = 1;
}

static int64_t
clock_ns (clockid_t clock)
{
  struct timespec ts;
  clock_gettime (clock, &ts);
  return (int64_t) ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* CLOCK_REALTIME - CLOCK_MONOTONIC, sampling CLOCK_MONOTONIC either side of
 * CLOCK_REALTIME so that being preempted in between can't skew it. */
static int64_t
realtime_offset_ns (void)
{
  int64_t best_width = INT64_MAX, best = 0;
  int i;
  for (i = 0; i < 4; i++) {
    int64_t before = clock_ns (CLOCK_MONOTONIC);
    int64_t real = clock_ns (CLOCK_REALTIME);
    int64_t after = clock_ns (CLOCK_MONOTONIC);
    if (after - before < best_width) {
      best_width = after - before;
      best = real - (before + (after - before) / 2);
    }
  }
  return best;
}

/* State shared with the dispmanx callbacks, which run on a VCHI thread.  We
 * don't make dispmanx calls from the callbacks themselves. */
static struct {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  uint64_t vsync_count;
  int64_t vsync_ns;
  int update_done;
  int64_t update_done_ns;
} cb = {
  PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0
};

static void
vsync_callback (DISPMANX_UPDATE_HANDLE_T u, void *arg)
{
  int64_t now = clock_ns (CLOCK_MONOTONIC);
  pthread_mutex_lock (&cb.lock);
  cb.vsync_count++;
  cb.vsync_ns = now;
  pthread_cond_broadcast (&cb.cond);
  pthread_mutex_unlock (&cb.lock);
}

static void
update_callback (DISPMANX_UPDATE_HANDLE_T u, void *arg)
{
  int64_t now = clock_ns (CLOCK_MONOTONIC);
  pthread_mutex_lock (&cb.lock);
  cb.update_done = 1;
  cb.update_done_ns = now;
  pthread_cond_broadcast (&cb.cond);
  pthread_mutex_unlock (&cb.lock);
}

/* Least-squares fit of vsync time against vsync index over the last
 * FIT_WINDOW vsyncs.  Times are relative to t0 to keep the precision. */
typedef struct {
  int64_t t0;
  int64_t index[FIT_WINDOW];
  double t[FIT_WINDOW];
  int n, next;
  int64_t last_index;
  /* t = a + period * index, in ns relative to t0: */
  double a, period;
  int ready;
} VsyncModel;

static int64_t
model_index (const VsyncModel *m, int64_t t_ns)
{
  return (int64_t) floor ((t_ns - m->t0 - m->a) / m->period + 0.5);
}

static int64_t
model_time (const VsyncModel *m, int64_t index)
{
  return m->t0 + (int64_t) llround (m->a + m->period * index);
}

static void
model_fit (VsyncModel *m)
{
  double mean_x = 0, mean_y = 0, sxx = 0, sxy = 0;
  char inlier[FIT_WINDOW];
  int i, n = 0;
  for (i = 0; i < m->n; i++) {
    double x = m->index[i], y = m->t[i];
    inlier[i] = !m->ready ||
        fabs (y - (m->a + m->period * x)) <= FIT_OUTLIER_NS;
    if (inlier[i]) {
      mean_x += x; mean_y += y; n++;
    }
  }
  if (n < 2)
    return;
  mean_x /= n;
  mean_y /= n;
  /* Centred, because after a few hours the raw sums of squares would lose
   * most of their precision to cancellation: */
  for (i = 0; i < m->n; i++) {
    if (inlier[i]) {
      double dx = m->index[i] - mean_x;
      sxx += dx * dx;
      sxy += dx * (m->t[i] - mean_y);
    }
  }
  m->period = sxy / sxx;
  m->a = mean_y - m->period * mean_x;
}

/* Adds a vsync callback time and returns its index. */
static int64_t
model_add (VsyncModel *m, int64_t t_ns, double *residual_ns)
{
  int64_t index;
  if (m->n == 0) {
    m->t0 = t_ns;
    index = 0;
  } else if (!m->ready) {
    /* Until we have a fit assume we haven't missed any callbacks: */
    index = m->last_index + 1;
  } else {
    index = model_index (m, t_ns);
  }
  m->last_index = index;
  m->index[m->next] = index;
  m->t[m->next] = t_ns - m->t0;
  m->next = (m->next + 1) % FIT_WINDOW;
  if (m->n < FIT_WINDOW)
    m->n++;

  if (m->ready)
    *residual_ns = (t_ns - m->t0) - (m->a + m->period * index);
  else
    *residual_ns = 0;
  if (m->n >= FIT_MIN) {
    model_fit (m);
    m->ready = 1;
  }
  return index;
}

/* Same encoding as gsttimestampoverlay.c's draw_timestamp. */
static void
draw_timestamp (int lineoffset, uint64_t timestamp, unsigned char *buf,
    size_t stride, int pxsize)
{
  int bit, line;
  buf += lineoffset * 8 * stride;

  for (line = 0; line < 8; line++) {
    for (bit = 0; bit < 64; bit++) {
      char color = ((timestamp >> (63 - bit)) & 1) * 255;
      memset (buf + bit * pxsize * 8, color, pxsize * 8);
    }
    buf += stride;
  }
}

typedef struct {
  int64_t start_ns;
  int64_t offset_ns;
  unsigned char image[PITCH * HEIGHT];
  DISPMANX_RESOURCE_HANDLE_T resources[NUM_BUFFERS];
} Renderer;

/* Draws the timestamps for a frame that will start being scanned out at
 * `vsync_ns` (CLOCK_MONOTONIC) into resource `buffer`.  Returns the
 * render_realtime drawn. */
static uint64_t
render (Renderer *r, int buffer, int64_t vsync_ns)
{
  unsigned char *stamp = r->image + STAMP_TOP * PITCH + STAMP_LEFT * 2;
  int64_t render_ns = vsync_ns + r->offset_ns;
  uint64_t render_realtime = render_ns + realtime_offset_ns ();
  VC_RECT_T rect;

  /* As `server`: buffer_time, stream_time and running_time are the time
   * since we started; clock_time and render_time are the pipeline clock
   * (CLOCK_MONOTONIC here); render_realtime is the one that matters. */
  draw_timestamp (0, vsync_ns - r->start_ns, stamp, PITCH, 2);
  draw_timestamp (1, vsync_ns - r->start_ns, stamp, PITCH, 2);
  draw_timestamp (2, vsync_ns - r->start_ns, stamp, PITCH, 2);
  draw_timestamp (3, vsync_ns, stamp, PITCH, 2);
  draw_timestamp (4, render_ns, stamp, PITCH, 2);
  draw_timestamp (5, render_realtime, stamp, PITCH, 2);

  /* write_data ignores the rect's x and width, and transfers whole rows
   * starting at src_address + rect.y * pitch: */
  vc_dispmanx_rect_set (&rect, 0, STAMP_TOP, WIDTH, NUM_TIMESTAMPS * 8);
  vc_dispmanx_resource_write_data (r->resources[buffer], VC_IMAGE_RGB565,
      PITCH, r->image, &rect);
  return render_realtime;
}

typedef struct {
  uint64_t frames, missed_callbacks, skipped, late, early;
  double resid_sum2, resid_max;
  uint64_t resid_n;
  int64_t apply_delay_hist[4];
} Stats;

static void
print_stats (const VsyncModel *m, Stats *s, int64_t realtime_ns)
{
  fprintf (stderr, "stats: realtime %" PRId64 ".%03d"
      " fps %.6f frames %" PRIu64 " callback-jitter %.1fus (max %.1fus)"
      " missed-callbacks %" PRIu64 " skipped %" PRIu64 " late %" PRIu64
      " early %" PRIu64 " applied-after-submit-vsyncs [1:%" PRId64
      " 2:%" PRId64 " 3:%" PRId64 " other:%" PRId64 "]\n",
      realtime_ns / 1000000000, (int) (realtime_ns / 1000000 % 1000),
      1e9 / m->period, s->frames,
      s->resid_n ? sqrt (s->resid_sum2 / s->resid_n) / 1000. : 0.,
      s->resid_max / 1000., s->missed_callbacks, s->skipped, s->late,
      s->early, s->apply_delay_hist[1], s->apply_delay_hist[2],
      s->apply_delay_hist[3], s->apply_delay_hist[0]);
  memset (s, 0, sizeof (*s));
}

static void
usage (const char *argv0)
{
  fprintf (stderr,
      "Usage: %s [OPTIONS]\n"
      "  --display N    dispmanx display (default 0)\n"
      "  --layer N      dispmanx layer (default 2000)\n"
      "  --lead N       draw each frame N vsyncs after the vsync at which we\n"
      "                 submit it (default 1)\n"
      "  --offset-us N  add N us to the drawn times, e.g. for the time from\n"
      "                 the vsync callback to the first active line (default 0)\n"
      "  --time-port N  answer UDP time requests on port N; 0 to disable\n"
      "                 (default %d)\n", argv0, TIMESERVER_DEFAULT_PORT);
}

int
main (int argc, char *argv[])
{
  static Renderer renderer;
  static VsyncModel model;
  Stats stats = { 0 };
  int display_num = 0, layer = 2000, lead = 1, time_port =
      TIMESERVER_DEFAULT_PORT;
  long offset_us = 0;
  DISPMANX_DISPLAY_HANDLE_T display;
  DISPMANX_MODEINFO_T info;
  DISPMANX_ELEMENT_HANDLE_T element;
  DISPMANX_UPDATE_HANDLE_T update;
  VC_RECT_T src_rect, dst_rect, rect;
  VC_DISPMANX_ALPHA_T alpha = { DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS, 255, 0 };
  uint32_t image_ptr;
  uint64_t vsyncs_seen = 0;
  int i, shown = 0, pending = -1, prepared = -1;
  int64_t pending_for = 0, prepared_for = 0, next_stats_ns;
  uint64_t pending_render_realtime = 0, prepared_render_realtime = 0;
  struct sigaction sa;

  static const struct option options[] = {
    { "display", required_argument, NULL, 'd' },
    { "layer", required_argument, NULL, 'l' },
    { "lead", required_argument, NULL, 'n' },
    { "offset-us", required_argument, NULL, 'o' },
    { "time-port", required_argument, NULL, 'p' },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 }
  };
  int opt;
  while ((opt = getopt_long (argc, argv, "", options, NULL)) != -1) {
    switch (opt) {
      case 'd': display_num = atoi (optarg); break;
      case 'l': layer = atoi (optarg); break;
      case 'n': lead = atoi (optarg); break;
      case 'o': offset_us = atol (optarg); break;
      case 'p': time_port = atoi (optarg); break;
      default: usage (argv[0]); return opt == 'h' ? 0 : 1;
    }
  }
  if (lead < 1) {
    usage (argv[0]);
    return 1;
  }
  renderer.offset_ns = offset_us * 1000;

  if (time_port && timeserver_start (time_port) != 0)
    return 1;

  memset (&sa, 0, sizeof (sa));
  sa.sa_handler = on_signal;
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);
  sigaction (SIGHUP, &sa, NULL);

  bcm_host_init ();
  display = vc_dispmanx_display_open (display_num);
  if (!display) {
    fprintf (stderr, "Failed to open dispmanx display %d\n", display_num);
    return 1;
  }
  if (vc_dispmanx_display_get_info (display, &info) != 0) {
    fprintf (stderr, "Failed to get display info\n");
    return 1;
  }
  fprintf (stderr, "Display %d is %dx%d\n", display_num, info.width,
      info.height);

  /* White, apart from the timestamps which we redraw for every frame: */
  memset (renderer.image, 0xff, sizeof (renderer.image));
  vc_dispmanx_rect_set (&rect, 0, 0, WIDTH, HEIGHT);
  for (i = 0; i < NUM_BUFFERS; i++) {
    renderer.resources[i] = vc_dispmanx_resource_create (VC_IMAGE_RGB565,
        WIDTH, HEIGHT, &image_ptr);
    if (!renderer.resources[i]) {
      fprintf (stderr, "Failed to create dispmanx resource\n");
      return 1;
    }
    vc_dispmanx_resource_write_data (renderer.resources[i], VC_IMAGE_RGB565,
        PITCH, renderer.image, &rect);
  }

  /* 1:1 pixels, centred on the screen, as `server` does with mmalvideosink: */
  vc_dispmanx_rect_set (&src_rect, 0, 0, WIDTH << 16, HEIGHT << 16);
  vc_dispmanx_rect_set (&dst_rect, (info.width - WIDTH) / 2,
      (info.height - HEIGHT) / 2, WIDTH, HEIGHT);
  update = vc_dispmanx_update_start (0);
  element = vc_dispmanx_element_add (update, display, layer, &dst_rect,
      renderer.resources[shown], &src_rect, DISPMANX_PROTECTION_NONE, &alpha,
      NULL, DISPMANX_NO_ROTATE);
  vc_dispmanx_update_submit_sync (update);

  renderer.start_ns = clock_ns (CLOCK_MONOTONIC);
  next_stats_ns = renderer.start_ns + 10 * 1000000000ll;
  vc_dispmanx_vsync_callback (display, vsync_callback, NULL);

  while (!quit) {
    int64_t vsync_ns, index, now;
    uint64_t count;
    double residual;
    int update_done;
    int64_t update_done_ns;
    struct timespec deadline;

    pthread_mutex_lock (&cb.lock);
    clock_gettime (CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    while (cb.vsync_count == vsyncs_seen && !quit) {
      if (pthread_cond_timedwait (&cb.cond, &cb.lock, &deadline) == ETIMEDOUT)
        break;
    }
    count = cb.vsync_count;
    vsync_ns = cb.vsync_ns;
    pthread_mutex_unlock (&cb.lock);
    if (quit)
      break;
    if (count == vsyncs_seen) {
      fprintf (stderr, "No vsync callback for 1s\n");
      continue;
    }
    if (vsyncs_seen && count > vsyncs_seen + 1)
      stats.missed_callbacks += count - vsyncs_seen - 1;
    vsyncs_seen = count;

    index = model_add (&model, vsync_ns, &residual);
    if (!model.ready)
      continue;
    stats.resid_sum2 += residual * residual;
    stats.resid_n++;
    if (fabs (residual) > stats.resid_max)
      stats.resid_max = fabs (residual);

    if (pending >= 0) {
      /* The update callback for the frame that should have been applied at
       * this vsync may be delivered just after the vsync callback, so give
       * it a little while: */
      int64_t wait_until = model_time (&model, index) + 4000000;
      pthread_mutex_lock (&cb.lock);
      while (!cb.update_done && clock_ns (CLOCK_MONOTONIC) < wait_until) {
        clock_gettime (CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 500000;
        if (deadline.tv_nsec >= 1000000000) {
          deadline.tv_sec++;
          deadline.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait (&cb.cond, &cb.lock, &deadline);
      }
      update_done = cb.update_done;
      update_done_ns = cb.update_done_ns;
      cb.update_done = 0;
      pthread_mutex_unlock (&cb.lock);

      if (!update_done) {
        /* Still waiting for it to be applied, so it will be on screen later
         * than the time drawn on it: */
        stats.skipped++;
        continue;
      } else {
        int64_t applied = model_index (&model, update_done_ns);
        int64_t delay = applied - (pending_for - lead);
        stats.apply_delay_hist[delay >= 1 && delay <= 3 ? delay : 0]++;
        if (applied != pending_for) {
          if (applied > pending_for)
            stats.late++;
          else
            stats.early++;
          printf ("%s frame render_realtime %" PRIu64 " was applied at vsync "
              "%+" PRId64 "\n", applied > pending_for ? "late" : "early",
              pending_render_realtime, applied - pending_for);
          fflush (stdout);
        }
        shown = pending;
        pending = -1;
      }
    }

    if (prepared >= 0 && prepared_for == index + lead) {
      update = vc_dispmanx_update_start (0);
      vc_dispmanx_element_change_source (update, element,
          renderer.resources[prepared]);
      vc_dispmanx_update_submit (update, update_callback, NULL);
      pending = prepared;
      pending_for = prepared_for;
      pending_render_realtime = prepared_render_realtime;
      stats.frames++;
    } else if (prepared >= 0) {
      /* We fell behind; the frame we prepared is for a vsync that has gone */
      stats.skipped++;
    }
    prepared = -1;

    for (i = 0; i < NUM_BUFFERS; i++)
      if (i != shown && i != pending)
        break;
    prepared = i;
    prepared_for = index + lead + 1;
    prepared_render_realtime = render (&renderer, prepared,
        model_time (&model, prepared_for));

    now = clock_ns (CLOCK_MONOTONIC);
    if (now > next_stats_ns) {
      print_stats (&model, &stats, now + realtime_offset_ns ());
      next_stats_ns += 10 * 1000000000ll;
    }
  }

  vc_dispmanx_vsync_callback (display, NULL, NULL);
  update = vc_dispmanx_update_start (0);
  vc_dispmanx_element_remove (update, element);
  vc_dispmanx_update_submit_sync (update);
  for (i = 0; i < NUM_BUFFERS; i++)
    vc_dispmanx_resource_delete (renderer.resources[i]);
  vc_dispmanx_display_close (display);
  return 0;
}
