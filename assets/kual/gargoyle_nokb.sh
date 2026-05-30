#!/bin/sh
EXTENSION=/mnt/us/extensions/gargoyle
cd ${EXTENSION}/dist/
#./fbink -g file=../gargoyle.png,halign=MIDDLE,valign=MIDDLE

lipc-set-prop -s com.lab126.winmgr orientationLock R

SAVED_GAMES=${EXTENSION}/saved_games \
GAMES=/mnt/us/IF/ \
LD_LIBRARY_PATH=${EXTENSION}/dist \
GARGOYLE_FULLSCREEN=1 \
./gargoyle

lipc-set-prop -s com.lab126.winmgr orientationLock U
