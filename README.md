# Introduction

**pv** ("Pipe Viewer") is a terminal-based tool for monitoring the progress
of data through a pipeline and modifying its flow.

When used just like "**cat**", **pv** displays how quickly data is being
transferred, how long it has taken, how near to completion it is, and an
estimate of how long it will be until completion.  Data flow rate, error
handling strategy, buffer size, and cache interaction can all be adjusted.

With the "**\--cursor**" option, several **pv** instances can work together,
showing transfer progress at different points in the same pipeline.

In "**\--watchfd**" mode, **pv** will inspect another process and show its
progress through the files it has open.

In "**\--monitor**" mode, **pv** will run a command and display the progress
of data through both its standard input and its standard output.


# Documentation

A manual page is included in this distribution ("`man pv`").  Before
installation, it is in "[docs/pv.1](./docs/pv.1.md)".

Changes are listed in "[docs/NEWS.md](./docs/NEWS.md)".

Contributors, please see "[CONTRIBUTING.md](./CONTRIBUTING.md)" and, for
more details, "[docs/DEVELOPERS.md](./docs/DEVELOPERS.md)".

Translators can use the Weblate instance hosted by Codeberg:
[https://translate.codeberg.org/engage/pv/](https://translate.codeberg.org/engage/pv/)

[![Translation status](https://translate.codeberg.org/widget/pv/multi-auto.svg)](https://translate.codeberg.org/engage/pv/)

If you don't see your language listed, raise an issue on the issue tracker
or via Weblate, asking for it to be added.


# Installation

Before compiling and installing from source, ensure you have the build
toolchain installed ("`sudo apt install build-essential`" on Debian or
Ubuntu systems).

The typical process for a system-wide install is:

    sh ./configure --prefix=/usr
    make
    sudo make install

See "[docs/INSTALL](./docs/INSTALL)" for more about the _configure_ script.

## Installing from the repository

Only packaged releases contain the _configure_ script.  When building from a
clone of the repository, _configure_ first needs to be generated with the
GNU build system tools (_autoconf_, _aclocal_, _autopoint_, _automake_), as
well as _gettext_.

On Debian or Ubuntu, run:

    sudo apt install automake autopoint gettext

Once those tools are in place, build _configure_ like this:

    autoreconf -is

Then follow the _Installation_ steps above.


# Copyright, bug reporting, and acknowledgements

See "[docs/ACKNOWLEDGEMENTS.md](./docs/ACKNOWLEDGEMENTS.md)" for a list of
contributors.

Copyright (C) 2002-2008, 2010, 2012-2015, 2017, 2021, 2023-2026 Andrew Wood.

License GPLv3+: GNU GPL version 3 or later <https://www.gnu.org/licenses/gpl-3.0.html>.

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
more details.

You should have received a copy of the GNU General Public License, version
3, in "[docs/COPYING](./docs/COPYING)".  If not, see
<https://www.gnu.org/licenses/gpl-3.0.html>.

Please report bugs or request features via the issue tracker linked from the
home page.

The **pv** home page is at:

> [https://ivarch.com/p/pv](https://ivarch.com/p/pv)

The latest version can always be found here.

---
