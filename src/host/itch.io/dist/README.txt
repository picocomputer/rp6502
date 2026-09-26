Picocomputer 6502 - itch.io sample
=================================

A ready-to-publish itch.io HTML5 project that plays one self-running
Picocomputer 6502 program in the browser. The page is just the game: it
boots and runs the moment itch.io loads it. The example program is
adventure.rp6502; swap in your own and ship it.


Deploy in three steps
---------------------

1. Configure. Open index.html and edit the CONFIG block near the top of
   the script - the ROM filename and a couple of display settings
   (below). That block is the only part you touch.

2. Drop in your program. Put your .rp6502 next to index.html and point
   CONFIG.rom at it. Delete the adventure.rp6502 placeholder.

3. Zip and upload. Zip the *contents* of this folder so index.html sits
   at the root of the zip (not inside a subfolder). On itch.io create a
   project, set the kind to HTML, upload the zip, and tick "This file
   will be played in the browser". Save, then play.

index.html, rp6502.js, rp6502.wasm, and your .rp6502 must stay together;
all paths are relative so the bundle works wherever itch.io serves it.


The editable section
--------------------

Everything you configure lives in one CONFIG object at the top of the
script:

    rom       Your program's filename, relative to index.html
              (e.g. game.rp6502).
    title     Browser tab title.
    bg        Letterbox/pillarbox fill color, six hex digits, no "#".
    filter    Pixel scaling: nearest (blocky), linear (smooth), or
              sharp.
    db        Browser save database name. Blank means the file name
              in rom. See "Saves and browser storage" below.


Updating the emulator
---------------------

index.html, rp6502.js, and rp6502.wasm are one matched set from a single
emulator build. The reason CONFIG is a single block near the top is to
make upgrades trivial: pull the newer files and re-apply that block.


itch.io embed settings
----------------------

    Manually set size          Choose 640x480 or 640x360. 320 wide
                               games will scale.
    Kind of Project            Choose HTML and upload a ZIP file.
    Enable scrollbars          Leave it off.
    SharedArrayBuffer support  Leave it off.


Saves and browser storage
-------------------------

A program saves by opening a file on the SAVE: device, such as
SAVE:hopper.hiscore. In the browser those files are in /saves/, and
they are the only files kept after the player leaves. The ROM is written
to /roms/ in memory, and a file written anywhere else, such as the
working directory, is lost when the player leaves.

/saves/ is mirrored to an IndexedDB database in the player's browser,
so saved games and high scores are kept. A close after a write queues
the save to IndexedDB, and a syncfs from the program returns once
IndexedDB has stored it. The page also requests persistent storage, so
a browser that grants it does not clear the saves to free space. Where
the browser blocks storage, as some private windows do, saves last
only until the player leaves the page.

Database name. CONFIG.db names the database. When it is blank, the
name is the file name in CONFIG.rom, without any folder or query
string. In either case each character other than A-Z, a-z, 0-9, ".",
"-" and "_" becomes "_". Earlier versions of this page named the
database after the whole rom setting, so a page whose rom setting has
a folder or a query string opens a new, empty database after an
upgrade. Set db to the old name, such as games_hopper.rp6502 for
games/hopper.rp6502, to keep those saves. Saves stored from the /db/
folder of an earlier version load into /saves/ under the same names.

One window at a time. A database is used by one window at a time,
because each window loads a separate copy and writes that copy back. A
second window with the same db shows "This game is running in another
window" and starts when the first window closes. Games with different
db names run side by side.

Who can see it. itch.io serves every HTML game from one shared origin
(html-classic.itch.zone), and IndexedDB is per-origin, so the database
namespace is shared with every other itch.io game the player runs. Any
game that opens the same name reads and writes the same saves - two
unrelated games that both ship game.rp6502 will collide. A unique db
(say, yourname-yourgame) avoids that.

Sharing on purpose. Give several of your pages the same db and their
games share one set of saves. This suits the episodes of one game,
since only one episode runs at a time. It works *because* of the
shared origin; if itch.io ever moves games onto separate origins,
cross-page sharing stops and existing saves effectively reset.


Please tag it RP6502
--------------------

When you publish, add the tag RP6502 to your project so it shows up
alongside other Picocomputer software (itch.io tags are on the project
edit page under Genre / Tags).

    https://itch.io/games/tag-rp6502


Notes
-----

Third-party license notices: append ?credits to the page URL to dump
every bundled component's license (the emulator's built-in --credits).
Keep that path reachable if you redistribute the bundled emulator.

Paste: a browser paste (Ctrl+V / Cmd+V) types the clipboard into the
emulated keyboard. Comes with the emulator; there's nothing to
configure.

README.txt is for you, the author. itch.io serves every file in the zip
but only opens index.html, so this file is never loaded by the player -
delete it before uploading if you like.
