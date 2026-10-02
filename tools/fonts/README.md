# Fonts of the LVGL interface

`tools/mkfonts.sh` renders them into `src/fonts/wizard_font_<size>.c`, which are committed, so a build
needs no Node.js.

- `DejaVuSans.ttf`: DejaVu Sans 2.37, the font of the wizard, license in `DejaVuSans-LICENSE.txt`.
  Rendered are ASCII and Latin-1 for the English texts, see `tools/mkfonts.sh`.
- `fontawesome/fa-solid-900.ttf`: Font Awesome 7.3.1 Free Solid, converted unchanged from
  `webfonts/fa-solid-900.woff2` of https://github.com/FortAwesome/Font-Awesome (tag 7.3.1), as
  lv_font_conv reads no WOFF2. License in `fontawesome/LICENSE.txt` (fonts: SIL OFL 1.1). The
  rendered fonts hold only a few of its icons and are not named Font Awesome, its Reserved Font Name.
