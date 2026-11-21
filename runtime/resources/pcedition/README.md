# PC prompt font

`External_00.bffnt` is the keyboard & mouse prompt font of Zelda-on-PC
(https://github.com/mircea32000/Zelda-on-PC, `bin/resources/pcedition/External_00.bffnt`, version 1.4.1),
licensed under the Mozilla Public License 2.0 (https://mozilla.org/MPL/2.0/). Its glyphs are Zelda-on-PC's own
artwork: one per key or mouse button at U+E100 + Windows virtual-key code, composites at U+E0F0..U+E0FC.

`runtime/src/input/pc_prompts.cpp` swaps it into the game's font archive at startup (see that file).
The build copies it next to BotWRecomp.exe as `resources/pcedition/External_00.bffnt`.
