# System Icon Policy

All OOS-owned functional UI icons come from **Font Awesome 5 Free**, using the
font bundled with the pinned LVGL source tree. The central mapping is
`system/src/oos/ui/icons.h`; production UI code must use that mapping rather
than embedding Unicode values, ad hoc letters, SVG paths, or symbols from a
second icon library.

This policy covers SystemUI controls, status indicators, and built-in
placeholder applications. Application packages retain ownership of their own
branded PNG icons; those are application content, not part of the OOS system
icon set, and OOS does not recolor them on selection.

The Orange OS brand logo is project-owned brand artwork rather than a
functional UI icon. Its only vector source is
`system/assets/branding/oos-logo.svg`; both the boot splash and the compiled
SystemUI BGRA image are generated from that file. Run
`scripts/generate-brand-assets.sh` after changing the source SVG.

Font Awesome's font files are distributed under SIL OFL 1.1. Its SVG icons are
distributed under CC BY 4.0, and supporting code under MIT. OOS embeds the
LVGL font glyphs and copies the complete upstream Font Awesome Free license
from the exact pinned LVGL checkout into every runtime resource package.

The procedural cellular signal bars and colored icon backgrounds are OOS
layout primitives, not third-party icon artwork.
