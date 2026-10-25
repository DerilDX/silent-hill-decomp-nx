# Making an HD Cyrillic font

Russian replaces every cell of the game font with Cyrillic, so the EU HD font
(`FONT16.png`) can't be used for it. Russian loads its own HD file instead:

    gamedata/load/1ST/FONT16_RU.png

It can also ship as a mod, at the same path inside the mod folder, just like the
EU HD font.

## The image

- It uses the same grid as the EU HD font: 21 columns by 6 rows of 12x16 cells,
  so 256x96 at native size. Any whole multiple works (2560x960 is 10x, the same
  size as the EU HD font). An image that isn't a whole multiple of 256x96 is
  ignored, and the log says so.
- Each cell holds the letter the Russian font has in that cell, NOT the EU
  font's letter. Use these as your starting points:
  - `fonts/FONT16_RU_guide.png`: every cell numbered and labelled with its
    letter. The red line marks how wide the game spaces that letter.
  - `fonts/FONT16_RU_10x_template.png`: the native Russian font blown up to
    2560x960, ready to draw over.
  - `fonts/FONT16_RU_native.png`: the native font at 1x.
- Letter spacing comes from the native font, not from the image. Keep each
  letter's ink inside its red line, or it will run into the next letter.
- Keep the background transparent, as in the EU HD font.

## Which cells matter

Russian text uses:

- the Cyrillic capitals (cells 26-51 and 84-89)
- the lowercase run (cells 91-125)
- the Latin lowercase in cells 58-83: `e`, `o` and `c` stand in for е, о and
  с, and the rest draw any Latin words in the text
- the digits and punctuation

Cells 108, 111 and 117 are drawn by other cells instead, so leave them as they
are.

## Notes

- With an HD Cyrillic font installed, the Menus filter smooths Russian text the
  same way it smooths the EU HD font. Without one, Russian text stays sharp,
  because smoothing the small native letters blurs them together.
- It works on every version of the game (USA, PAL and Japanese).
