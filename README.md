gst-mmal.tar
============

A binaries built for raspbian of the dependencies of latency-clock.
Includes:

* gst-mmal
* GStreamer 1.8
* mmal

It took ages to build so I've created this binary tarball.

Usage
-----

On Raspberry Pi:

    cd /
    curl https://raw.githubusercontent.com/stb-tester/latency-clock/gst-mmal.tar/gst-mmal.tar | \
    sudo tar -xf gst-mmal.tar

Build instructions:
-------------------

I built it on my PC in an ARM chroot with QEMU.

    sudo apt-get build-dep gstreamer1.0 gst-plugins-base1.0
    sudo apt-get remove libgstreamer1.0-dev
    sudo apt-get install cmake

    mkdir build
    cd build/
    git clone https://anongit.freedesktop.org/git/gstreamer/gstreamer.git
    git clone https://anongit.freedesktop.org/git/gstreamer/gst-plugins-base.git
    git clone https://github.com/youviewtv/gst-mmal.git
    git clone https://github.com/raspberrypi/userland.git
    git clone https://github.com/stb-tester/latency-clock.git

    (
        cd gstreamer/
        ./autogen.sh --prefix=/opt/latency-clock --disable-gtk-doc
        make -j 12
        sudo make install
    )
    (
        cd gst-plugins-base/
        ./autogen.sh PATH=/opt/latency-clock/bin:$PATH PKG_CONFIG_PATH=/opt/latency-clock/lib/pkgconfig/ --prefix=/opt/latency-clock --disable-gtk-doc
        make -j12
        sudo make install
    )
    (
        cd userland/
        cmake -DCMAKE_INSTALL_PREFIX:PATH=/opt/latency-clock/ .
        make -j12
        sudo make install
    )
    (
        cd gst-mmal/
        ./autogen.sh PATH=/opt/latency-clock/bin:$PATH PKG_CONFIG_PATH=/opt/latency-clock/lib/pkgconfig/ --prefix=/opt/latency-clock --disable-gtk-doc CFLAGS="-I/opt/vc/include/ -L/opt/vc/lib"
        make -j12 
        sudo make install
    )

    tar -cf gst-mmal.tar /opt/latency-clock/ /opt/vc/
