# OOS System Fonts

`ui-proportional.otf` is retained as a host-test fixture. It is Red Hat Text
Regular from the Fedora
`redhat-text-fonts` package. It is distributed under the SIL Open Font License;
the complete license is stored in `LICENSE.txt`.

Production packages do not install this file. Native applications access the
platform system font through the `font-assets` WIT role rather than a
filesystem path. Device builds use `/system/fonts/Roboto-Regular.ttf`, while
the local backend uses its host system font.
