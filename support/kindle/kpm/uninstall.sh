#!/bin/sh
# Games and saved games are left in place.
rm -f /mnt/us/documents/Gargoyle.sh
cd /mnt/us/gargoyle || exit 0
rm -rf dist gargoyle.sh gargoyle.png menu.json config.xml gargoyle.log
