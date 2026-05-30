#!/bin/sh
EXTENSION=/mnt/us/extensions/gargoyle
cd ${EXTENSION}/dist/
./fbink -g file=../gargoyle.png,halign=MIDDLE,valign=MIDDLE
lipc-set-prop -s com.lab126.keyboard open net.fabiszewski.gargoyle:Abc:1

SAVED_GAMES=${EXTENSION}/saved_games \
GAMES=/mnt/us/IF/ \
LD_LIBRARY_PATH=${EXTENSION}/dist \
PATH=$PATH:${EXTENSION}/dist \
GTK2_RC_FILES=${EXTENSION}/dist/gtkrc \
USE_KB=YES \
./gargoyle

lipc-set-prop -s com.lab126.keyboard close net.fabiszewski.gargoyle
