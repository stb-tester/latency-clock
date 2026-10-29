/* latency-clock
 * Copyright (C) 2026 William Manley <will@williammanley.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#include "timeserver.h"

#include <endian.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Request: at least 8 bytes.  The first 8 are opaque to us and are echoed
 * back (the client puts its send time there).
 *
 * Response: 24 bytes:
 *   0..7    the first 8 bytes of the request, unchanged
 *   8..15   when we received the request, CLOCK_REALTIME ns, big-endian
 *   16..23  when we sent the response, CLOCK_REALTIME ns, big-endian
 */

static uint64_t
timespec_to_ns (const struct timespec *ts)
{
  return (uint64_t) ts->tv_sec * 1000000000ull + ts->tv_nsec;
}

static uint64_t
realtime_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_REALTIME, &ts);
  return timespec_to_ns (&ts);
}

static void *
timeserver_thread (void *arg)
{
  int sock = (int) (intptr_t) arg;

  while (1) {
    unsigned char req[64], resp[24];
    char control[CMSG_SPACE (sizeof (struct timespec))];
    struct sockaddr_storage from;
    struct iovec iov = { req, sizeof (req) };
    struct msghdr msg = {
      .msg_name = &from, .msg_namelen = sizeof (from),
      .msg_iov = &iov, .msg_iovlen = 1,
      .msg_control = control, .msg_controllen = sizeof (control),
    };
    struct cmsghdr *cmsg;
    uint64_t rx_ns = 0, tx_ns;
    ssize_t len;

    len = recvmsg (sock, &msg, 0);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      perror ("timeserver: recvmsg");
      break;
    }
    if (len < 8)
      continue;

    /* The kernel's receive timestamp excludes our scheduling delay: */
    for (cmsg = CMSG_FIRSTHDR (&msg); cmsg; cmsg = CMSG_NXTHDR (&msg, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPNS) {
        struct timespec ts;
        memcpy (&ts, CMSG_DATA (cmsg), sizeof (ts));
        rx_ns = timespec_to_ns (&ts);
      }
    }
    if (rx_ns == 0)
      rx_ns = realtime_ns ();

    memcpy (resp, req, 8);
    rx_ns = htobe64 (rx_ns);
    memcpy (resp + 8, &rx_ns, 8);
    tx_ns = htobe64 (realtime_ns ());
    memcpy (resp + 16, &tx_ns, 8);
    sendto (sock, resp, sizeof (resp), 0, (struct sockaddr *) &from,
        msg.msg_namelen);
  }
  close (sock);
  return NULL;
}

int
timeserver_start (uint16_t port)
{
  int sock, one = 1, zero = 0;
  struct sockaddr_in6 addr = {
    .sin6_family = AF_INET6, .sin6_port = htons (port),
    .sin6_addr = IN6ADDR_ANY_INIT,
  };
  pthread_t thread;

  sock = socket (AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    perror ("timeserver: socket");
    return -1;
  }
  /* Accept IPv4 too: */
  setsockopt (sock, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof (zero));
  if (setsockopt (sock, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof (one)) != 0)
    perror ("timeserver: SO_TIMESTAMPNS (continuing without it)");
  if (bind (sock, (struct sockaddr *) &addr, sizeof (addr)) != 0) {
    perror ("timeserver: bind");
    close (sock);
    return -1;
  }
  if (pthread_create (&thread, NULL, timeserver_thread,
          (void *) (intptr_t) sock) != 0) {
    fprintf (stderr, "timeserver: failed to create thread\n");
    close (sock);
    return -1;
  }
  pthread_detach (thread);
  fprintf (stderr, "Answering time requests on UDP port %u\n", port);
  return 0;
}
