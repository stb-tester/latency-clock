/* GStreamer
 * Copyright (C) 2016 William Manley <will@williammanley.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gsttimeoverlayparse
 *
 * The timeoverlayparse element records various timestamps onto the video.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! timeoverlayparse ! timeoverlayparse ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include "gsttimeoverlayparse.h"

#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_timeoverlayparse_debug_category);
#define GST_CAT_DEFAULT gst_timeoverlayparse_debug_category

/* prototypes */
static GstFlowReturn gst_timeoverlayparse_transform_frame_ip (GstVideoFilter * filter,
    GstVideoFrame * frame);

enum
{
  PROP_0
};

/* pad templates */

/* FIXME: add/remove formats you can handle */
#define VIDEO_SRC_CAPS \
    GST_VIDEO_CAPS_MAKE("{RGB, xRGB, BGR, BGRx}")

/* FIXME: add/remove formats you can handle */
#define VIDEO_SINK_CAPS \
    GST_VIDEO_CAPS_MAKE("{RGB, xRGB, BGR, BGRx}")


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstTimeOverlayParse, gst_timeoverlayparse, GST_TYPE_VIDEO_FILTER,
  GST_DEBUG_CATEGORY_INIT (gst_timeoverlayparse_debug_category, "timeoverlayparse", 0,
  "debug category for timeoverlayparse element"));

static void
gst_timeoverlayparse_class_init (GstTimeOverlayParseClass * klass)
{
  GstVideoFilterClass *video_filter_class = GST_VIDEO_FILTER_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
        gst_caps_from_string (VIDEO_SRC_CAPS)));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
        gst_caps_from_string (VIDEO_SINK_CAPS)));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "TimeOverlayParse", "Generic", "Reads the various timestamps from the "
      "video written by timestampoverlay",
      "William Manley <will@williammanley.net>");

  video_filter_class->transform_frame_ip = GST_DEBUG_FUNCPTR (gst_timeoverlayparse_transform_frame_ip);
}

static void
gst_timeoverlayparse_init (GstTimeOverlayParse *timeoverlayparse)
{
}

typedef struct {
  GstClockTime buffer_time;
  GstClockTime stream_time;
  GstClockTime running_time;
  GstClockTime clock_time;
  GstClockTime render_time;
} Timestamps;

static GstClockTime
read_timestamp(int lineoffset, unsigned char* buf, size_t stride, int pxsize)
{
  int bit;
  GstClockTime timestamp = 0;

  buf += (lineoffset * 8 + 4) * stride;

  for (bit = 0; bit < 64; bit++) {
    char color = buf[bit * pxsize * 8 + 4];
    timestamp |= (color & 0x80) ?  (guint64) 1 << (63 - bit) : 0;
  }

  return timestamp;
}

static GstFlowReturn
gst_timeoverlayparse_transform_frame_ip (GstVideoFilter * filter, GstVideoFrame * frame)
{
  GstTimeOverlayParse *overlay = GST_TIMEOVERLAYPARSE (filter);
  Timestamps timestamps;

  GST_DEBUG_OBJECT (overlay, "transform_frame_ip");

  GstClockTime buffer_time, running_time, clock_time;
  GstClockTimeDiff latency;
  GstSegment *segment = &GST_BASE_TRANSFORM (overlay)->segment;

  buffer_time = GST_BUFFER_TIMESTAMP (frame->buffer);

  if (!GST_CLOCK_TIME_IS_VALID (buffer_time)) {
    GST_DEBUG_OBJECT (filter, "Can't measure latency: buffer timestamp is "
        "invalid");
    return GST_FLOW_OK;
  }

  if (frame->info.stride[0] < (8 * frame->info.finfo->pixel_stride[0] * 64)) {
    GST_WARNING_OBJECT (filter, "Can't read timestamps: video-frame is to narrow");
    return GST_FLOW_OK;
  }

  GST_DEBUG ("buffer with timestamp %" GST_TIME_FORMAT,
      GST_TIME_ARGS (buffer_time));

  running_time = gst_segment_to_running_time (segment, GST_FORMAT_TIME,
      buffer_time);
  clock_time = running_time + gst_element_get_base_time (GST_ELEMENT (overlay));

  GST_DEBUG_OBJECT (filter, "Buffer timestamps"
      ": buffer_time = %" GST_TIME_FORMAT
      ", running_time = %" GST_TIME_FORMAT
      ", clock_time = %" GST_TIME_FORMAT,
      GST_TIME_ARGS(buffer_time),
      GST_TIME_ARGS(running_time),
      GST_TIME_ARGS(clock_time));

  timestamps.buffer_time = read_timestamp (0, frame->data[0],
      frame->info.stride[0], frame->info.finfo->pixel_stride[0]);
  timestamps.stream_time = read_timestamp (1, frame->data[0],
      frame->info.stride[0], frame->info.finfo->pixel_stride[0]);
  timestamps.running_time = read_timestamp (2, frame->data[0],
      frame->info.stride[0], frame->info.finfo->pixel_stride[0]);
  timestamps.clock_time = read_timestamp (3, frame->data[0],
      frame->info.stride[0], frame->info.finfo->pixel_stride[0]);
  timestamps.render_time = read_timestamp (4, frame->data[0],
      frame->info.stride[0], frame->info.finfo->pixel_stride[0]);

  GST_DEBUG_OBJECT (filter, "Read timestamps: buffer_time = %" GST_TIME_FORMAT
      ", stream_time = %" GST_TIME_FORMAT ", running_time = %" GST_TIME_FORMAT
      ", clock_time = %" GST_TIME_FORMAT ", render_time = %" GST_TIME_FORMAT,
      GST_TIME_ARGS(timestamps.buffer_time),
      GST_TIME_ARGS(timestamps.stream_time),
      GST_TIME_ARGS(timestamps.running_time),
      GST_TIME_ARGS(timestamps.clock_time),
      GST_TIME_ARGS(timestamps.render_time));

  latency = clock_time - timestamps.render_time;

  GST_INFO_OBJECT (filter, "Latency: %" GST_TIME_FORMAT,
      GST_TIME_ARGS(latency));

  return GST_FLOW_OK;
}
