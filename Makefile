all: client server gsttimestampoverlay.so

CFLAGS?=-Wall -Werror -O2

gsttimestampoverlay.so : \
        gsttimestampoverlay.c \
        gsttimestampoverlay.h \
        gsttimeoverlayparse.c \
        gsttimeoverlayparse.h \
        plugin.c
	$(CC) -o$@ --shared -fPIC $^ $(CFLAGS) \
	    $$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0)

server : server.c
	$(CC) -o$@ $^ $(CFLAGS) $$(pkg-config --cflags --libs gstreamer-1.0)

client : client.c
	$(CC) -o$@ $^ $(CFLAGS) $$(pkg-config --cflags --libs gstreamer-1.0)
