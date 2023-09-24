# Introduction

This is the README for **pv** ("Pipe Viewer"), a terminal-based tool for
monitoring the progress of data through a pipeline.  It can be inserted into
any normal pipeline between two processes to give a visual indication of how
quickly data is passing through, how long it has taken, how near to
completion it is, and an estimate of how long it will be until completion.


# Documentation

A manual page is included in this distribution ("`man pv`").  Before
installation, it is in "[docs/pv.1](./docs/pv.1.md)".

Changes are listed in "[docs/NEWS.md](./docs/NEWS.md)".  The to-do list is "[docs/TODO.md](./docs/TODO.md)".

Developers and translators, please see "[docs/DEVELOPERS.md](./docs/DEVELOPERS.md)".


# Compilation

To compile the package, type "`sh ./configure`", which should generate a
Makefile for your system.  You may then type "`make`" to build everything,
and "`make install`" to install it.

See the file "[docs/INSTALL](./docs/INSTALL)" for more about the _configure_ script.

If this is not a packaged release, you will need the GNU build system tools
(`autoconf`, `aclocal`, `autopoint`, `automake`) and the `gettext`
development tools.  You can then run "`autoreconf -is`" to generate the
"`configure`" script.


# Copyright, bug reporting, and acknowledgements

Copyright (C) 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood.

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

Please report any bugs to **pv@ivarch.com**, or use the issue tracker linked
from the **pv** home page.

The **pv** home page is at:

[https://www.ivarch.com/programs/pv.shtml](https://www.ivarch.com/programs/pv.shtml)

The latest version can always be found here.

**Please see "[docs/ACKNOWLEDGEMENTS.md](./docs/ACKNOWLEDGEMENTS.md)" for a list of contributors.**

---
