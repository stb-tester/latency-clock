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
	$(CC) -o$@ $^ $(CFLAGS) $$(pkg-config --cflags --libs gstreamer-1.0) -lm

client : client.c
	$(CC) -o$@ $^ $(CFLAGS) $$(pkg-config --cflags --libs gstreamer-1.0)

dist:
	git archive -o latency-clock-0.0.1.tar HEAD --prefix=latency-clock-0.0.1/

clean:
	rm -f client server gsttimestampoverlay.so
