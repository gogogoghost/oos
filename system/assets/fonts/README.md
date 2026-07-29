# OOS System Fonts

`ui-proportional.otf` is Red Hat Text Regular from the Fedora
`redhat-text-fonts` package. It is distributed under the SIL Open Font License;
the complete license is stored in `LICENSE.txt` and copied into every OOS
runtime package.

The stable installed name is `/opt/oos/share/fonts/ui-proportional.otf`.
Native applications access it through the `font-assets` WIT role rather than a
filesystem path. Keep this role and installed filename stable when replacing
the font, and verify that the replacement license permits redistribution.
