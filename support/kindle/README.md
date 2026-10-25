# Gargoyle for Kindle

Gargoyle runs on jailbroken Kindles with firmware 5.16.3 or later (the hard
float `kindlehf` builds), with an interface in the style of the Kindle:
a GTK+ 2 interface drawn in black and white, sized for e-ink and touch.

## Installing

Download `gargoyle-kindlehf.zip` from the releases and extract it at the root
of the Kindle storage. `gargoyle/` holds the program with its `games` and
`saved_games` folders, and `documents/Gargoyle.sh` adds Gargoyle to the
library. The `gargoyle` folder can also go in `extensions/` to start it from
KUAL.

With [KPM](https://github.com/KindleModding/KPM), install the `.kpkg` package
instead; uninstalling it keeps games, saves and settings.

## Playing

The game list shows the platform of each game on its icon; files that are not
games are greyed out. Tap a file to select it and again to open it, tap a
folder to open it, and swipe or use the arrows to turn pages. **Download
games** searches [IFDB](https://ifdb.org) and downloads into the games folder
with [ifdb-dl](https://github.com/dfghjkjhgr/ifdb-dl), which runs in
[kTerm](https://github.com/bfabiszewski/kterm).

In a game, the header bar shows the title, a button to show or hide the
keyboard (the game takes its area while it is hidden, which suits external
keyboards) and a menu to save, restore, go back to the game list or quit.
Z-code games also save themselves before each command and carry on where they
were left the next time they are opened.

Touch gestures:

* Tap on the input line: place the cursor.
* Double tap on a word of the story: type it on the input line.
* Swipe up or down: page through the story; swipe left or right: move the
  cursor.
* Two-finger tap, by thirds of the screen:
  * left: clear the input line (top), delete the previous word (middle), move
    the cursor a word left (bottom);
  * middle: previous (top half) or next (bottom half) command of the history;
  * right: delete a character (top), delete the next word (middle), show or
    hide the keyboard (bottom).

Settings go in `gargoyle/config/garglk.ini`, as described in the `garglk.ini`
of Gargoyle; for instance `zoom 1.2` makes text larger and `fullscreen 1`
starts with the keyboard hidden.

The interface is translatable: catalogs are in `garglk/kindle/l10n`; copy
`gargoyle.pot` to `<language>.po` to add a language.

## Building

The build needs the `kindlehf` toolchain of
[koxtoolchain](https://github.com/koreader/koxtoolchain) (built with
`./gen-tc.sh kindlehf` or extracted from its release into `~/x-tools`) with
[kindle-sdk](https://github.com/KindleModding/kindle-sdk) installed on top of
it (`./gen-sdk.sh kindlehf`), and Go for the game downloader. Then run:

    support/kindle/build.sh
    support/kindle/package.sh

The program is installed into `build/dist` and the packages are written to
`build/package`.

On a desktop, `cmake -DINTERFACE=KINDLE -DDIST_INSTALL=ON` builds the same
interface against GTK+ 2, given a `lipc` library.
