#!/bin/sh
# KPM runs the hooks from the extracted package directory.
mkdir -p /mnt/us/gargoyle
cp -rf ./gargoyle/. /mnt/us/gargoyle/
cp -f ./scriptlets/Gargoyle.sh /mnt/us/documents/Gargoyle.sh
