#!/bin/bash
#export PATH=~/dev/buildroot/buildroot-2016.11.1/output/host/usr/bin:$PATH

#export SYSROOT=~/dev/buildroot/buildroot-2016.11.1/output/staging
#export SYSROOT=~/dev/buildroot/buildroot-2016.11.1/output/host/usr/arm-buildroot-linux-gnueabi/sysroot

#export PKG_CONFIG_DIR=/usr/lib/arm-linux-gnueabihf/pkgconfig
#export PKG_CONFIG_LIBDIR=${SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${SYSROOT}/usr/share/pkgconfig
#export PKG_CONFIG_SYSROOT_DIR=${SYSROOT}

#-sBUILD=DEBUG 
jam -dx -q -j4 -sKINDLE=true -sUSESDL=no -sGARGLKINI=/mnt/us/extensions/gargoyle/garglk.ini 
#jam -dx -q -sUSESDL=no install
