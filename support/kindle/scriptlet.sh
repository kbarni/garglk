#!/bin/sh
# Name: Gargoyle
# Author: kbarni
# Icon: data:image/png;base64,@ICON@
# DontUseFBInk

for GARGOYLE_DIR in /mnt/us/gargoyle /mnt/us/extensions/gargoyle; do
    if [ -x "${GARGOYLE_DIR}/gargoyle.sh" ]; then
        exec "${GARGOYLE_DIR}/gargoyle.sh"
    fi
done
