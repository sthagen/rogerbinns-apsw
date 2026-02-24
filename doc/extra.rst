SQLite extra
============

In addition to the main library, SQLite has additional programs and
loadable extensions.  However these need to be separately compiled and
installed.  Full APSW builds such as those on PyPI include all the
ones that compile for that platform.  This is for convenience and to
help promote these great extras.

Access is provided via an API and via the command line.

Third party libraries such as for compression (:code:`zlib`) or
command line editing (:code:`readline`), TCL are not used - the only
dependencies are the platform and SQLite.  That means they can be
freely copied to other systems.

The binaries are all marked as packaged by APSW.  Under Windows this
is indicated in the detailed properties listing.  On other platforms
running :code:`strings` should show it, with ELF binaries having a
:code:`note.apsw` section and MacOS (mach-o) binaries having a
:code:`apsw` section.

.. include:: sqlite_extra.rst-inc

Command line
------------

Programs can be run by giving their name and parameters. For example
code:`sqlite3_scrub` ::

    python3 -m apsw.sqlite_extra sqlite3_scrub source.db dest.db

You can also get the filename for any program or extension.  For example the
:code:`sqlite3_rsync` program::

    python3 -m apsw.sqlite_extra --path sqlite3_rsync

The :code:`csv` extension::

    python3 -m apsw.sqlite_extra --path csv

List what is available::

    python3 -m apsw.sqlite_extra --path csv


API
---

.. automodule:: apsw.sqlite_extra
    :members:
    :undoc-members:
    :member-order: bysource