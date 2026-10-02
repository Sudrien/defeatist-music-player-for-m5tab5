# arabixel

Arabic for the screen: Arabixel Basic's glyphs as a C bitmap table, and
the shaping that picks which glyph each letter of a word needs.

Upstream: https://arabiandev.itch.io/arabixel-basic-font
Arabixel Basic (c) 2024 ArabianDev. Creative Commons Attribution 4.0
International (CC BY 4.0). `LICENSE.txt` is the font's own, unchanged.

## Licence

**`Arabixel_Basic.ttf` and `arabixel_tab.c` are CC BY 4.0. The rest of
this component -- `arabixel.h`, `arabixel_shape.c` -- and the rest of the
project are MIT.**

`arabixel_tab.c` is an adaptation under CC BY 4.0: the outlines are
converted to 12-row bitmaps, and ten glyphs have one row removed so they
fit (an empty row where there was one; otherwise a repeated row of the
tail of final jeem, hah and khah -- `tools/gen_arabixel.py` lists them).
CC BY asks for credit, a link to the licence and a note that it was
changed; this file and the table's header give all three. It places no
condition on the code it is compiled with.

## What it draws

The Arabic block's letters, hamza forms, Arabic and Persian digits and
punctuation, and every joined form in Presentation Forms-B, lam-alef
included. Not the Persian and Urdu letters پ چ ژ گ ک ی ے -- the font
does not have them, so they draw as boxes -- and not the vowel marks,
which `arabixel_shape()` drops because gfx.c places glyphs side by side
and never one over another.

## Regenerating

    pip install fonttools
    ./tools/gen_arabixel.py

The table's header records the sha256 of the font it was made from.
