# Gamepad Icons

The Controls screen draws controller buttons as small pictures. The
"Gamepad Icons" setting picks which set of pictures is used: Original, Xbox or
Nintendo. Each set is one PNG file that you can paint yourself.

## Files

The files live in the game folder, under `gamedata/icons/`:

| Setting  | File               |
|----------|--------------------|
| Original | `pad_original.png` |
| Xbox     | `pad_xbox.png`     |
| Nintendo | `pad_nintendo.png` |

In the source tree they are in `pc_port/assets/gamedata/icons/`. A build copies
them into the game folder.

The setting is saved in `config.cfg` as `gamepad_icons` (0 = Original,
1 = Xbox, 2 = Nintendo).

## Image size

Paint the image at **128 x 256** pixels. You can also paint it at exactly 2x
(256 x 512) or 4x (512 x 1024) for sharper icons. Any other size is rejected
and the disc icons are shown instead.

The image is laid out like the game's own button sheet, `TIM/OPTION2.TIM`, so
an icon has to sit in exactly the right spot to be used. Use transparency
(alpha 0) for everything that is not an icon.

## Where each icon goes

Every icon is a **12 x 12** pixel square. The table gives the top-left pixel
of each square in the 128 x 256 image (at 2x or 4x, multiply both numbers).

The "u, v" column is the same position in the game's own coordinates, which is
what the code and logs use. To get the image position: x = u - 128, y = v.

| Icon             | Image x, y | u, v     | On the disc? |
|------------------|------------|----------|--------------|
| L2               | 116, 0     | 244, 0   | yes          |
| R2               | 116, 16    | 244, 16  | yes          |
| L1               | 116, 32    | 244, 32  | yes          |
| R1               | 116, 48    | 244, 48  | yes          |
| Triangle         | 116, 64    | 244, 64  | yes          |
| Circle           | 116, 80    | 244, 80  | yes          |
| Cross            | 116, 96    | 244, 96  | yes          |
| Square           | 116, 112   | 244, 112 | yes          |
| Select           | 116, 128   | 244, 128 | yes          |
| L3               | 116, 144   | 244, 144 | yes          |
| R3               | 116, 160   | 244, 160 | yes          |
| Start            | 116, 176   | 244, 176 | yes          |
| D-pad Up         | 116, 192   | 244, 192 | no, new      |
| D-pad Right      | 116, 208   | 244, 208 | no, new      |
| D-pad Down       | 116, 224   | 244, 224 | no, new      |
| D-pad Left       | 116, 240   | 244, 240 | no, new      |
| Left stick Up    | 100, 0     | 228, 0   | no, new      |
| Left stick Right | 100, 16    | 228, 16  | no, new      |
| Left stick Down  | 100, 32    | 228, 32  | no, new      |
| Left stick Left  | 100, 48    | 228, 48  | no, new      |

So the right-hand column (x 116 to 127) holds the twelve buttons from the
original game, top to bottom, then the four d-pad directions in the space the
disc leaves empty. The four left-stick directions get a new column just to the
left (x 100 to 111). Each row is 16 pixels below the one above; the 4 pixels
between squares stay transparent.

Face buttons are placed by position, not by letter: "Cross" is the bottom face
button, "Circle" the right one, "Square" the left one and "Triangle" the top
one. So on the Xbox sheet the A button goes in the Cross square, B in Circle,
X in Square and Y in Triangle.

## When an icon is missing

- A square with nothing painted in it (fully transparent) is drawn as text
  instead, for example `D-PAD U` or `LS L`.
- If the file for the chosen set is missing or cannot be read, the game uses the
  icons from the disc. The twelve original buttons then show as pictures, and
  the d-pad and stick directions show as text.
- Buttons with no PlayStation equivalent (Guide, touchpad, paddles) are always
  drawn as text.

The log (`SilentHill_<date>.log`, with debug logging on) has one `[PADICONS]`
line each time a set is loaded, saying which file was used and how many icons
had art.
