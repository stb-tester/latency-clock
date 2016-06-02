/* GStreamer
 *
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <gst/gst.h>

static gboolean bus_call (GstBus *bus, GstMessage *msg, gpointer data);

int main(int argc, char* argv[])
{
  GMainLoop *loop;
  GstBus *bus;
  GstElement * epipeline;
  GstPipeline * pipeline;
  GstClock* clock;
  GError * err = NULL;
  gchar * source_pipeline;
  struct timespec ts;
  int res;

  gst_init(&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  if (argc > 1)
    source_pipeline = argv[1];
  else
    source_pipeline = "v4l2src";

  epipeline = gst_parse_launch (g_strdup_printf (
      "%s "
      "! video/x-raw,width=1280,height=720 "
      "! timeoverlayparse "
      "! fakesink", source_pipeline), &err);

  if (err) {
    fprintf(stderr, "Error creating pipeline: %s\n", err->message);
    return 1;
  }
  g_return_val_if_fail (epipeline != NULL, 1);
  pipeline = GST_PIPELINE(epipeline);

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (bus, bus_call, loop);
  gst_object_unref (bus);

/*  gst_element_set_state(epipeline, GST_STATE_READY); */

  clock = g_object_new (GST_TYPE_SYSTEM_CLOCK, "clock-type",
      GST_CLOCK_TYPE_REALTIME, NULL);
  gst_pipeline_use_clock(pipeline, clock);

  res = clock_gettime(CLOCK_REALTIME, &ts);
  g_return_val_if_fail (res == 0, 1);

  gst_element_set_state(epipeline, GST_STATE_PLAYING);

  g_main_loop_run (loop);

  return 0;
}

static gboolean
bus_call (GstBus *bus, GstMessage *msg, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;

  switch (GST_MESSAGE_TYPE (msg)) {

    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
      break;

    case GST_MESSAGE_ERROR: {
      gchar  *debug;
      GError *error;

      gst_message_parse_error (msg, &error, &debug);
      g_free (debug);

      g_printerr ("Error: %s\n", error->message);
      g_error_free (error);

      exit (1);
      break;
    }
    default:
      break;
  }

  return TRUE;
}
