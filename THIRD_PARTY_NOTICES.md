
## tools/h8500 — GNU binutils (GPLv2)

`tools/h8500/h8500-dis.c` and `tools/h8500/h8500-opc.h` are taken unmodified
from GNU binutils 2.16.1, the last release that carried an H8/500 target; the
architecture was dropped from binutils afterwards and neither Ghidra nor any
free disassembler has picked it up since.

They are GPLv2, so `tools/sc55dis` is GPLv2 as a whole. It is an analysis tool
and is not linked into the plug-in or any other product of this repository.

    Copyright 1993, 1998, 2000-2004 Free Software Foundation, Inc.
