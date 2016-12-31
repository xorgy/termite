A keyboard-centric VTE-based terminal, aimed at use within a window manager
with tiling and/or tabbing support.

Termite looks for the configuration file in the following order:
``$XDG_CONFIG_HOME/termite/config``, ``~/.config/termite/config``,
``$XDG_CONFIG_DIRS/termite/config``, ``/etc/xdg/termite/config``.

Termite's exit status is 1 on a failure, including a termination of the child
process from an uncaught signal. Otherwise the exit status is that of the child
process.

DEPENDENCIES
============

A vte version >= ``0.46``.

If no browser is configured and $BROWSER is unset, xdg-open from xdg-utils is
used as a fallback.

BUILDING
========
::

    git clone --recursive https://github.com/xorgy/termite
    cd termite && make

KEYBINDINGS
===========

+----------------------+---------------------------------------------+
| ``ctrl-shift-r``     | reload configuration file                   |
+----------------------+---------------------------------------------+
| ``ctrl-shift-c``     | copy to CLIPBOARD                           |
+----------------------+---------------------------------------------+
| ``ctrl-shift-v``     | paste from CLIPBOARD                        |
+----------------------+---------------------------------------------+
| ``ctrl-shift-u``     | unicode input (standard GTK binding)        |
+----------------------+---------------------------------------------+
| ``shift-pageup``     | scroll up a page                            |
+----------------------+---------------------------------------------+
| ``shift-pagedown``   | scroll down a page                          |
+----------------------+---------------------------------------------+
| ``ctrl-shift-+``     | increase font size                          |
+----------------------+---------------------------------------------+
| ``ctrl-shift--``     | decrease font size                          |
+----------------------+---------------------------------------------+
| ``ctrl-shift-0``     | reset font size to default                  |
+----------------------+---------------------------------------------+
| ``F11``              | toggle fullscreen                           |
+----------------------+---------------------------------------------+
