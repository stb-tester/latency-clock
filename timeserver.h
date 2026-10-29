/* latency-clock
 * Copyright (C) 2026 William Manley <will@williammanley.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#ifndef _TIMESERVER_H_
#define _TIMESERVER_H_

#include <stdint.h>

#define TIMESERVER_DEFAULT_PORT 10123

/* Answers time requests on UDP `port` from a background thread, so a client
 * can work out the offset between its CLOCK_REALTIME and ours.  See
 * timeclient.py for the protocol.  Returns 0 on success, -1 on failure (with
 * a message printed to stderr). */
int timeserver_start (uint16_t port);

#endif
