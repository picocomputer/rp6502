Picocomputer 6502 - web player
==============================

This folder is a ready-to-publish itch.io HTML5 project that plays one
Picocomputer 6502 program in a browser. adventure.rp6502 is the sample
program.


Publish on itch.io
------------------

1. Put your .rp6502 next to index.html and delete adventure.rp6502.

2. In index.html, change the CONFIG block. rom is the file name of your
   program, title is the name in the browser tab, and db names the
   database for saves: use your full itch.io user name and the full
   project name, such as username-projectname.

3. Zip the contents of this folder, so that index.html is at the root of
   the zip, not in a subfolder. On itch.io, create a project, set the
   kind to HTML, upload the zip and tick "This file will be played in
   the browser". Set the embed size to 640x480 or 640x360, and leave
   scrollbars and SharedArrayBuffer support off.

Please add the tag RP6502 to the project, so that it is listed with the
other Picocomputer software at https://itch.io/games/tag-rp6502


Updating
--------

Replace rp6502.js and rp6502.wasm with the ones from a newer itch.io zip
and keep your index.html.


More
----

The CONFIG settings, the click-to-play overlay, the footer, saves, and
publishing on GitHub Pages or any other web server:

    https://picocomputer.github.io/web.html
