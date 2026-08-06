This folder is the RP6502's drive.

A program running on the core sees it as MSC0:, and a plain
open("game.save", ...) lands here. Files you put here are visible to
the program by name.

It ships with the core because the openFPGA host will not create a
folder: asked to make a file in a path that is not there, it answers
with a descriptor and writes nothing at all. So the folder has to
arrive with the package, or the drive is read-only forever.

Deleting this file is harmless. Deleting the folder is not.
