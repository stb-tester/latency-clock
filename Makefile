all: gsttimestampoverlay.so

CFLAGS?=-Wall -Werror -O2

gsttimestampoverlay.so : gsttimestampoverlay.c  gsttimestampoverlay.h
	$(CC) -o$@ --shared -fPIC $^ $(CFLAGS) \
	    $$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0)
