Picocomputer 6502 - web player
==============================

This folder is a web page that plays one Picocomputer 6502 program in a
browser. adventure.rp6502 is the sample program.


Publish
-------

1. Put your .rp6502 next to index.html and delete adventure.rp6502.

2. In index.html, change the CONFIG block. rom is the file name of your
   program, title is the name in the browser tab, and db names the
   database for saves.

3. Copy the folder to any web server. A browser runs the page only from
   a web server, not from a file on disk; to try it on your computer,
   run "python3 -m http.server 8000" in this folder and open
   http://localhost:8000.


Updating
--------

Replace rp6502.js and rp6502.wasm with the ones from a newer web zip and
keep your index.html.


More
----

The CONFIG settings, the click-to-play overlay, the footer, saves, and
building a web zip with CMake:

    https://picocomputer.github.io/web.html
