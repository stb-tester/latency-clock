all: client server libgsttimeoverlayparse.so \
    $(if $(wildcard /opt/vc/include/bcm_host.h),dispmanx-server)

CFLAGS?=-Wall -Werror -O2

libgsttimeoverlayparse.so : \
        gsttimestampoverlay.c \
        gsttimestampoverlay.h \
        gsttimeoverlayparse.c \
        gsttimeoverlayparse.h \
        plugin.c
	$(CC) -o$@ --shared -fPIC $^ $(CFLAGS) \
	    $$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0)

server : server.c timeserver.c timeserver.h
	$(CC) -o$@ server.c timeserver.c $(CFLAGS) \
	    $$(pkg-config --cflags --libs gstreamer-1.0) -lm -lpthread

# The Raspberry Pi's legacy (non-KMS) display stack, from the
# libraspberrypi-dev package:
VC_PREFIX?=/opt/vc

dispmanx-server : dispmanx-server.c timeserver.c timeserver.h
	$(CC) -o$@ dispmanx-server.c timeserver.c $(CFLAGS) \
	    -I$(VC_PREFIX)/include -L$(VC_PREFIX)/lib -Wl,-rpath,$(VC_PREFIX)/lib \
	    -lbcm_host -lvcos -lvchiq_arm -lm -lpthread

client : client.c
	$(CC) -o$@ $^ $(CFLAGS) $$(pkg-config --cflags --libs gstreamer-1.0)

dist:
	git archive -o latency-clock-0.0.1.tar HEAD --prefix=latency-clock-0.0.1/

clean:
	rm -f client server dispmanx-server libgsttimeoverlayparse.so
