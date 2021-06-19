Installation
============
Copy all files into the root Quake 2 directory and run. Make a backup of the relevant files first
if you want to restore them later.


Notes
=====
This is based on vkQuake2. As such, updated graphics backends are included in this package in
addition to the main client. You should be able to drop this into any regular installation of
Quake 2, including the original, GoG and Steam versions. Let me know if any of these do not work.

The miniaudio mixer is enabled by default, but you can switch to the original DMA mixer with the
"s_mixer" cvar:

  +set s_mixer dma
  
You will need to specify this on the command line upon launching the game. Toggling between the DMA
and miniaudio mixers will not work once the game has loaded.

I've only done limited testing on this. There's some subtle differences to the original game, but
they shouldn't be too noticeable. Feel free to post a bug report on GitHub if you find anything.


Links
=====
miniaudio:   https://miniaud.io
Source Code: https://github.com/mackron/vkQuake2
vkQuake2:    https://github.com/kondrak/vkQuake2
