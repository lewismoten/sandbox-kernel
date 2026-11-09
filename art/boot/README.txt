sandbox-kernel boot art

Target display geometry
The eInk screen is 250 × 122 in logical landscape coordinates for the
Waveshare 2.13-inch V4. PNGs are grayscale at 1 bit per pixel. P4 PBMs
pack each row MSB first with 1 for black and 0 for white. Strip the P4
header for blit() masks. Icon row strides are 12 bytes (96) and 5 bytes
(40). The 250-wide PBMs have a 32-byte logical stride including row
padding. The panel renderer handles rotation and native polarity.

The HDMI home artwork is 960 × 540 for 2× nearest-neighbor output.
A 640 × 480 overlay is included for the alternate mode. HDMI PNGs
are indexed. Use their palette RGB values when converting to RGB565.
For #RRGGBB bytes, pack ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3).
Full-scale white is #F8FCF8 in this zero-filled 8-bit representation.
The bars retain their original order, and the ramp has 16 distinct
steps quantized to RGB565. The test card retains 1 px and 2 px
checker interiors at (736,326) and (846,326), each 84 × 50.

File inventory (pixel size, byte size, exact palette)
eink-mark-96.png  96×96  165 bytes
  #000000 #FFFFFF
eink-mark-96.pbm  96×96  1161 bytes
  #000000 #FFFFFF
eink-mark-40.png  40×40  144 bytes
  #000000 #FFFFFF
eink-mark-40.pbm  40×40  209 bytes
  #000000 #FFFFFF
eink-starting-template-250x122.png  250×122  196 bytes
  #000000 #FFFFFF
eink-starting-template-250x122.pbm  250×122  3915 bytes
  #000000 #FFFFFF
eink-starting-250x122.png  250×122  416 bytes
  #000000 #FFFFFF
eink-starting-250x122.pbm  250×122  3915 bytes
  #000000 #FFFFFF
eink-status-template-250x122.png  250×122  184 bytes
  #000000 #FFFFFF
eink-status-template-250x122.pbm  250×122  3915 bytes
  #000000 #FFFFFF
eink-status-250x122.png  250×122  400 bytes
  #000000 #FFFFFF
eink-status-250x122.pbm  250×122  3915 bytes
  #000000 #FFFFFF
hdmi-overlay-960x540.png  960×540  3064 bytes
  #000000 #0000F8 #00FC00 #00FCF8 #101010 #202020 #303430 #404440 #505450 #50CCD8 #606460 #707470 #7888A0 #888888 #989898 #A8A8A8 #B8B8B8 #C8C8C8 #D878E8 #D8DCD8 #E8ECE8 #F80000 #F800F8 #F87468 #F8CC58 #F8FC00 #F8FCF8
hdmi-overlay-640x480.png  640×480  2044 bytes
  #000000 #0000F8 #00FC00 #00FCF8 #101010 #202020 #303430 #404440 #505450 #50CCD8 #606460 #707470 #7888A0 #888888 #989898 #A8A8A8 #B8B8B8 #C8C8C8 #D878E8 #D8DCD8 #E8ECE8 #F80000 #F800F8 #F87468 #F8CC58 #F8FC00 #F8FCF8
hdmi-test-card-960x540.png  960×540  3061 bytes
  #000000 #0000F8 #00FC00 #00FCF8 #101010 #101420 #202020 #303430 #404440 #505450 #50CCD8 #606460 #707470 #7888A0 #888888 #989898 #A8A8A8 #B8B8B8 #C8C8C8 #D878E8 #D8DCD8 #E8ECE8 #F80000 #F800F8 #F87468 #F8CC58 #F8FC00 #F8FCF8
hdmi-recovery-console-960x540.png  960×540  1540 bytes
  #000000 #101420 #50CCD8 #7888A0 #D878E8 #F87468 #F8CC58 #F8FCF8
README.txt  UTF-8 text, no pixel dimensions or palette.

Text boxes (x, y, width, height)
Origin is top left. Bounds are half-open. Fixed text is baked into the
regular screens. The eInk templates leave fixed text boxes blank too.
The eInk screens use the kernel’s 5×7 font at 2×, with 12-pixel character
advance and a 14-pixel space. Draw title words on two lines on starting.
Use 2× on the recovery console with a fixed 12-pixel character advance
(including spaces), 24-pixel line pitch, and white #F8FCF8 text.
Split or clip log text at 60 columns and scroll after 12 rows.

starting
  SANDBOX          112,  24,  84,  14  fixed
  KERNEL           112,  42,  72,  14  fixed
  STARTING...      112,  77, 132,  14  fixed

status
  SANDBOX KERNEL    54,   6, 170,  14  fixed
  UP:                4,  48,  36,  14  fixed
  USB:               4,  70,  48,  14  fixed
  ERR:               4,  92,  48,  14  fixed
  Build ID          54,  24, 144,  14  12 characters
  Uptime            54,  48, 192,  14  16 characters
  USB value         54,  70, 192,  14  16 characters
  Error value       54,  92, 192,  14  16 characters

overlay-960x540
  Panel            298, 312, 364,  96  fixed
  Title            391, 331, 252,  21  fixed
  Subtitle         392, 371, 180,  14  fixed

overlay-640x480
  Panel            138, 272, 364,  96  fixed
  Title            231, 291, 252,  21  fixed
  Subtitle         232, 331, 180,  14  fixed

test-card
  Title            139, 329, 336,  28  fixed
  Caption          140, 376, 192,  14  fixed
  1 PX caption     754, 392,  48,  14  fixed
  2 PX caption     864, 392,  48,  14  fixed

recovery
  Title            192,  52, 336,  28  fixed
  Caption          193,  96, 192,  14  fixed
  Kernel log       120, 151, 720, 288  60 columns × 12 rows

License
SPDX-License-Identifier: MIT
The chip-with-K mark, screen layouts, and lowercase glyphs are original
to this project. Uppercase glyphs and punctuation use sandbox-kernel’s
original MIT-licensed 5×7 bitmap font from core/render.c. The artwork
contains no third-party logos or fonts and can ship under the project’s
MIT license.

Integration
The images are assets and layout references. Recovery selection and
runtime text rendering belong in the kernel. Raspberry Pi is the
initial target, and the boot graphics carry no platform branding.

Notes from the kernel repository
The recovery header in this art is baked from hdmi-recovery-console-960x540.png (96,44, 424x72).
Near-white #F8FCF8 is drawn as #FFFFFF by the kernel, and the ramp is the code's neutral gray
(G equal to R), not the green-tinted ramp in these PNGs. The Game-Maker castle is not part of the
kernel: Game-Maker OS owns that branding.
