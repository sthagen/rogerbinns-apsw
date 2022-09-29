.. _building:

Building
********

See :ref:`version info <version_stuff>` to understand the
relationship between Python, APSW, and SQLite versions.

setup.py
========

Short story: You run :file:`setup.py` but you should ideally follow
the :ref:`recommended way <recommended_build>` which will also fetch
needed components for you.

+-------------------------------------------------------------+-------------------------------------------------------------------------+
| Command                                                     |  Result                                                                 |
+=============================================================+=========================================================================+
| | python setup.py install test                              | Compiles APSW with default Python compiler, installs it into Python     |
|                                                             | site library directory and then runs the test suite.                    |
+-------------------------------------------------------------+-------------------------------------------------------------------------+
| | python setup.py install :option:`--user`                  | Compiles APSW with default Python                                       |
|                                                             | compiler and installs it into a subdirectory of your home directory.    |
|                                                             | See :pep:`370` for more details.                                        |
+-------------------------------------------------------------+-------------------------------------------------------------------------+
| | python setup.py build_ext :option:`--force`               | Compiles the extension but doesn't install it.  The test suite is then  |
|   :option:`--inplace` test                                  | run.                                                                    |
+-------------------------------------------------------------+-------------------------------------------------------------------------+
| | python setup.py build :option:`--debug` install           | Compiles APSW with debug information.  This also turns on `assertions   |
|                                                             | <http://en.wikipedia.org/wiki/Assert.h>`_                               |
|                                                             | in APSW that double check the code assumptions.  If you are using the   |
|                                                             | SQLite amalgamation then assertions are turned on in that too.  Note    |
|                                                             | that this will considerably slow down APSW and SQLite.                  |
+-------------------------------------------------------------+-------------------------------------------------------------------------+

.. _setup_py_flags:

Additional :file:`setup.py` flags
=================================

There are a number of APSW specific flags to commands you can specify.

fetch
-----

:file:`setup.py` can automatically fetch SQLite and other optional
components.  You can set the environment variable :const:`http_proxy`
to control proxy usage for the download.

If any files are downloaded then the build step will automatically use
them.  This still applies when you do later builds without
re-fetching.

  | python setup.py fetch *options*

+----------------------------------------+--------------------------------------------------------------------------------------+
| fetch flag                             |  Result                                                                              |
+========================================+======================================================================================+
| | :option:`--version=VERSION`          | By default the SQLite version corresponding to the APSW release is retrieved,  You   |
|                                        | can also ask for specific versions, or for `latest` which uses the SQLite download   |
|                                        | page to work out the most recent version.                                            |
+----------------------------------------+--------------------------------------------------------------------------------------+
| | :option:`--missing-checksum-ok`      | Allows setup to continue if the :ref:`checksum <fetch_checksums>` is missing.        |
+----------------------------------------+--------------------------------------------------------------------------------------+
| | :option:`--all`                      | Gets all components listed below.                                                    |
+----------------------------------------+--------------------------------------------------------------------------------------+
| | :option:`--sqlite`                   | Automatically downloads the `SQLite amalgamation                                     |
|                                        | <https://sqlite.org/amalgamation.html>`__. The amalgamation is the                   |
|                                        | preferred way to use SQLite as you have total control over what components are       |
|                                        | included or excluded (see below) and have no dependencies on any existing            |
|                                        | libraries on your developer or deployment machines. The amalgamation includes the    |
|                                        | fts3/4/5, rtree, json1 and icu extensions. On non-Windows platforms, any existing    |
|                                        | :file:`sqlite3/` directory will be erased and the downloaded code placed in a newly  |
|                                        | created :file:`sqlite3/` directory.                                                  |
+----------------------------------------+--------------------------------------------------------------------------------------+

.. _fetch_checksums:

.. note::

  The SQLite downloads are not `digitally signed
  <http://en.wikipedia.org/wiki/Digital_signature>`__ which means you
  have no way of verifying they were produced by the SQLite team or
  were not modified between the SQLite servers and your computer.

  Consequently APSW ships with a :source:`checksums file <checksums>`
  that includes checksums for the various SQLite downloads.  If the
  download does not match the checksum then it is rejected and an
  error occurs.

  The SQLite download page is not checksummed, so in theory a bad guy
  could modify it to point at a malicious download version instead.
  (setup only uses the page to determine the current version number -
  the SQLite download site URL is hard coded.)

  If the URL is not listed in the checksums file then setup aborts.
  You can use :option:`--missing-checksum-ok` to continue.  You are
  recommended instead to update the checksums file with the
  correct information.

.. _fetch_configure:

.. note::

  (This note only applies to non-Windows platforms.)  By default the
  amalgamation will work on your platform.  It detects
  the operating system (and compiler if relevant) and uses the
  appropriate APIs.  However it then only uses the oldest known
  working APIs.  For example it will use the *sleep* system call.
  More recent APIs may exist but the amalgamation needs to be told
  they exist.  As an example *sleep* can only sleep in increments of
  one second while the *usleep* system call can sleep in increments of
  one microsecond. The default SQLite busy handler does small sleeps
  (eg 1/50th of a second) backing off as needed.  If *sleep* is used
  then those will all be a minimum of a second.  A second example is
  that the traditional APIs for getting time information are not
  re-entrant and cannot be used concurrently from multiple threads.
  Consequently SQLite has mutexes to ensure that concurrent calls do
  not happen.  However you can tell it you have more recent re-entrant
  versions of the calls and it won't need to bother with the mutexes.

  After fetching the amalgamation, setup automatically determines what
  new APIs you have by running the :file:`configure` script that comes
  with SQLite and noting the output.  The information is placed in
  :file:`sqlite3/sqlite3config.h`.  The build stage will automatically
  take note of this as needed.

.. _setup_build_flags:

build/build_ext
---------------

You can enable or omit certain functionality by specifying flags to
the build and/or build_ext commands of :file:`setup.py`::

  python setup.py build *options*

Note that the options do not accumulate.  If you want to specify multiple enables or omits then you
need to give the flag once and giving a comma separated list.  For example::

  python setup.py build --enable=fts3,fts3_parenthesis,rtree,icu

SQLite includes `many options defined to the C compiler
<https://www.sqlite.org/compile.html>`__.  If you want to change
compiled in default values, or provide defines like
SQLITE_CUSTOM_INCLUDE then you can use :option:`--definevalues` using
`=` and comma separating.  For example::

  python setup.py build_ext --definevalues SQLITE_DEFAULT_FILE_FORMAT=1,SQLITE_CUSTOM_INCLUDE=config.h

+----------------------------------------+--------------------------------------------------------------------------------------+
| build/build_ext flag                   | Result                                                                               |
+========================================+======================================================================================+
| | :option:`--enable-all-extensions`    | Enables the STAT4, FTS3/4/5, RTree, JSON1, RBU, and ICU extensions if *icu-config*   |
|                                        | is on your path                                                                      |
+----------------------------------------+--------------------------------------------------------------------------------------+
| | :option:`--enable=fts3`              | Enables the :ref:`full text search extension <ext-fts3>`.                            |
| | :option:`--enable=fts4`              | This flag only helps when using the amalgamation. If not using the                   |
| | :option:`--enable=fts5`              | amalgamation then you need to separately ensure fts3/4/5 is enabled in the SQLite    |
|                                        | install. You are likely to want the `parenthesis option                              |
|                                        | <https://sqlite.org/compile.html#enable_fts3_parenthesis>`__ on unless you have      |
|                                        | legacy code (`--enable-all-extensions` turns it on).                                 |
+----------------------------------------+--------------------------------------------------------------------------------------+
| | :option:`--enable=rtree`             | Enables the :ref:`spatial table extension <ext-rtree>`.                              |
|                                        | This flag only helps when using the amalgamation. If not using the                   |
|                                        | amalgamation then you need to separately ensure rtree is enabled in the SQLite       |
|                                        | install.                                                                             |
+----------------------------------------+--------------------------------------------------------------------------------------+
| | :option:`--enable=rbu`               | Enables the :ref:`reumable bulk update extension <ext-rbu>`.                         |
|                                        | This flag only helps when using the amalgamation. If not using the                   |
|                                        | amalgamation then you need to separately ensure rbu is enabled in the SQLite         |
|                                        | install.                                                                             |
+----------------------------------------+--------------------------------------------------------------------------------------+
| | :option:`--enable=icu`               | Enables the :ref:`International Components for Unicode extension <ext-icu>`.         |
|                                        | Note that you must have the ICU libraries on your machine which setup will           |
|                                        | automatically try to find using :file:`icu-config`.                                  |
|                                        | This flag only helps when using the amalgamation. If not using the                   |
|                                        | amalgamation then you need to separately ensure ICU is enabled in the SQLite         |
|                                        | install.                                                                             |
+----------------------------------------+--------------------------------------------------------------------------------------+
| | :option:`--omit=ITEM`                | Causes various functionality to be omitted. For example                              |
|                                        | :option:`--omit=load_extension` will omit code to do with loading extensions. If     |
|                                        | using the amalgamation then this will omit the functionality from APSW and           |
|                                        | SQLite, otherwise the functionality will only be omitted from APSW (ie the code      |
|                                        | will still be in SQLite, APSW just won't call it). In almost all cases you will need |
|                                        | to regenerate the SQLite source because the omits also alter the generated SQL       |
|                                        | parser. See `the relevant SQLite documentation                                       |
|                                        | <https://sqlite.org/compile.html#omitfeatures>`_.                                    |
+----------------------------------------+--------------------------------------------------------------------------------------+


.. _matching_sqlite_options:

Matching APSW and SQLite options
================================

APSW needs to see the same options as SQLite to correctly match it.
For example if SQLite is compiled without loadable extensions, then
APSW also needs to know that at compile time because the APIs won't be
present.  Another example is :attr:`Cursor.description_full` needs to
know if `SQLITE_ENABLE_COLUMN_METADATA` was defined when building
SQLite for the same reason.

If you use the amalgamation (recommended configuration) then APSW and
SQLite will see the same options and will be correctly in sync.

If you are using the system provided SQLite then specify
`--use-system-sqlite-config` to `build_ext`, and the configuration
will be automatically obtained (using `ctypes find_library
<https://docs.python.org/3/library/ctypes.html?highlight=find_library#ctypes.util.find_library>`__)

You can use the amalgamation and `--use-system-sqlite-config`
simultaneously in which case the amalgamation will have an identical
configuration to the system one.  This is useful if you are using a
newer SQLite version in the amalgamation, but still want to match the
system.


Finding SQLite 3
================

SQLite 3 is needed during the build process. If you specify
:option:`fetch --sqlite` to the :file:`setup.py` command line
then it will automatically fetch the current version of the SQLite
amalgamation. (The current version is determined by parsing the
`SQLite download page <https://sqlite.org/download.html>`_). You
can manually specify the version, for example
:option:`fetch --sqlite --version=3.7.4`.

These methods are tried in order:

  `Amalgamation <https://sqlite.org/amalgamation.html>`__

      The file :file:`sqlite3.c` and then :file:`sqlite3/sqlite3.c` is
      looked for. The SQLite code is then statically compiled into the
      APSW extension and is invisible to the rest of the
      process. There are no runtime library dependencies on SQLite as
      a result.  When you use :option:`fetch` this is where it places
      the downloaded amalgamation.

  Local build

    The header :file:`sqlite3/sqlite3.h` and library :file:`sqlite3/libsqlite3.{a,so,dll}` is looked for.


  User directories

    If specifying :option:`--user` then your user directory is
    searched first. See :pep:`370` for more details.

  System directories

    The default compiler include path (eg :file:`/usr/include`) and library path (eg :file:`/usr/lib`) are used.


.. note::

  If you compiled SQLite with any OMIT flags (eg
  :const:`SQLITE_OMIT_LOAD_EXTENSION`) then you must include them in
  the :file:`setup.py` command or file. For this example you could use
  :option:`setup.py build --omit=load_extension` to add the same flags.

.. _recommended_build:

Recommended
===========

These instructions show how to build automatically downloading and
using the amalgamation plus other :ref:`extensions`. Any existing SQLite on
your system is ignored at build time and runtime. (Note that you can
even use APSW in the same process as a different SQLite is used by
other libraries - this happens a lot on Mac.) You should follow these
instructions with your current directory being where you extracted the
APSW source to.

  Windows::

    > python setup.py fetch --all build --enable-all-extensions install test

  Mac/Linux etc::

    $ python setup.py fetch --all build --enable-all-extensions install test

.. note::

  There may be some warnings during the compilation step about
  sqlite3.c, `but they are harmless <https://sqlite.org/faq.html#q17>`_


The extension just turns into a single file apsw.so (Linux/Mac) or
apsw.pyd (Windows). (More complicated name on Pythons implementing
:pep:`3149`). You don't need to install it and can drop it into any
directory that is more convenient for you and that your code can
reach. To just do the build and not install, leave out *install* from
the lines above. (Use *build_ext --inplace* to have the extension put
in the main directory.)

The test suite will be run. It will print the APSW file used, APSW and
SQLite versions and then run lots of tests all of which should pass.

Source distribution (advanced)
==============================

If you want to make a source distribution or a binary distribution
that creates an intermediate source distribution such as `bdist_rpm`
then you can have the SQLite amalgamation automatically included as
part of it.  If you specify the fetch command as part of the same
command line then everything fetched is included in the source
distribution.  For example this will fetch all components, include
them in the source distribution and build a rpm using those
components::

  $ python setup.py fetch --all bdist_rpm

.. _testing:

Testing
=======

SQLite itself is `extensively tested
<https://sqlite.org/testing.html>`__. It has considerably more code
dedicated to testing than makes up the actual database functionality.

APSW includes tests which use the standard Python testing modules to
verify correct operation. New code is developed alongside the tests.
Reported issues also have test cases to ensure the issue doesn't
happen or doesn't happen again.::

  $ python3 -m apsw.tests
                  Python  /usr/bin/python3 sys.version_info(major=3, minor=10, micro=4, releaselevel='final', serial=0)
  Testing with APSW file  /space/apsw/apsw/__init__.cpython-310-x86_64-linux-gnu.so
            APSW version  3.39.2.0
      SQLite lib version  3.39.2
  SQLite headers version  3039002
      Using amalgamation  True
  ...............................................................................................
  ----------------------------------------------------------------------
  Ran 95 tests in 25.990s

  OK

The tests also ensure that as much APSW code as possible is executed
including alternate paths through the code.  95.5% of the APSW code is
executed by the tests. If you checkout the APSW source then there is a
script :source:`tools/coverage.sh` that enables extra code that
deliberately induces extra conditions such as memory allocation
failures, SQLite returning undocumented error codes etc. That brings
coverage up to 99.6% of the code.

A memory checker `Valgrind <http://valgrind.org>`_ is used while
running the test suite. The test suite is run multiple times to make
any memory leaks or similar issues stand out. A checking version of
Python is also used.  See :source:`tools/valgrind.sh` in the source.
The same testing is also done with the `compiler's sanitizer option
<https://en.wikipedia.org/wiki/AddressSanitizer>`__.

To ensure compatibility with the various Python versions, a script
downloads and compiles all supported Python versions in both debug and
release configurations (and 32 and 64 bit) against the APSW and SQLite
supported versions running the tests. See :source:`tools/megatest.py`
in the source.

In short both SQLite and APSW have a lot of testing!
