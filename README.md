Introduction
------------

This is the README for **pv** ("Pipe Viewer"), a terminal-based tool for
monitoring the progress of data through a pipeline.  It can be inserted into
any normal pipeline between two processes to give a visual indication of how
quickly data is passing through, how long it has taken, how near to
completion it is, and an estimate of how long it will be until completion.


Documentation
-------------

A manual page is included in this distribution ("`man pv`").  Before
installation, it is in "[doc/pv.1.md](./doc/pv.1)".

Changes are listed in "[doc/NEWS.md](./doc/NEWS.md)".  The to-do list is "[doc/TODO.md](./doc/TODO.md)".

Developers and translators, please see "[doc/DEVELOPERS.md](./doc/DEVELOPERS.md)".


Compilation
-----------

To compile the package, type "`sh ./configure`", which should generate a
Makefile for your system.  You may then type "`make`" to build everything,
and "`make install`" to install it.

See the file "[doc/INSTALL](./doc/INSTALL)" for more about the _configure_ script.

If this is not a packaged release, you will need the GNU build system tools
(`autoconf`, `aclocal`, `autopoint`, `automake`) and the `gettext`
development tools.  You can then run "`autoreconf -is`" to generate the
"`configure`" script.


Author and acknowledgements
---------------------------

This package is copyright 2023 Andrew Wood, and is being distributed under
the terms of the Artistic License 2.0.  For more details of this license,
see the file "[doc/COPYING](./doc/COPYING)".

Report bugs in **pv** using the contact form linked from the home page, or
though the [project issue tracker](https://codeberg.org/a-j-wood/pv/issues).

The **pv** home page is at:

[http://www.ivarch.com/programs/pv.shtml](http://www.ivarch.com/programs/pv.shtml)

The latest version can always be found here.

**Please see "[doc/ACKNOWLEDGEMENTS.md](./doc/ACKNOWLEDGEMENTS.md)" for a list of contributors.**

---
