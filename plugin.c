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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gsttimeoverlayparse.h"
#include "gsttimestampoverlay.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "timestampoverlay", GST_RANK_NONE,
             GST_TYPE_TIMESTAMPOVERLAY) &&
         gst_element_register (plugin, "timeoverlayparse", GST_RANK_NONE,
             GST_TYPE_TIMEOVERLAYPARSE);
}

#ifndef VERSION
#define VERSION "0.0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "latency_clock"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "latency_clock"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "http://stb-tester.com/"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    timeoverlayparse,
    "Elements for measuring video capture latency",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)

