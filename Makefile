
SQLITEVERSION=3.36.0
APSWSUFFIX=-r1

RELEASEDATE="30 July 2021"

VERSION=$(SQLITEVERSION)$(APSWSUFFIX)
VERDIR=apsw-$(VERSION)
VERWIN=apsw-$(SQLITEVERSION)

PYTHON=python3

# Some useful info
#
# To use a different SQLite version: make SQLITEVERSION=1.2.3 blah blah
#
# build_ext      - builds extension in current directory fetching sqlite
# test           - builds extension in place then runs test suite
# doc            - makes the doc
# source         - makes a source zip in dist directory after running code through test suite

GENDOCS = \
	doc/blob.rst \
	doc/vfs.rst \
	doc/vtable.rst \
	doc/connection.rst \
	doc/cursor.rst \
	doc/apsw.rst \
	doc/backup.rst

.PHONY : all docs doc header linkcheck publish showsymbols compile-win source source_nocheck release tags clean ppa dpkg dpkg-bin coverage valgrind valgrind1 tagpush

all: header docs

tagpush:
	git tag -af $(SQLITEVERSION)$(APSWSUFFIX)
	git push --tags

clean:
	make PYTHONPATH="`pwd`" VERSION=$(VERSION) -C doc clean
	rm -rf dist build work/* megatestresults apsw.egg-info
	mkdir dist
	for i in '*.pyc' '*.pyo' '*~' '*.o' '*.so' '*.dll' '*.pyd' '*.gcov' '*.gcda' '*.gcno' '*.orig' '*.tmp' 'testdb*' 'testextension.sqlext' ; do \
		find . -type f -name "$$i" -print0 | xargs -0t --no-run-if-empty rm -f ; done

doc: docs

docs: build_ext $(GENDOCS) doc/example.rst doc/.static
	env PYTHONPATH=. $(PYTHON) tools/docmissing.py
	env PYTHONPATH=. $(PYTHON) tools/docupdate.py $(VERSION)
	make PYTHONPATH="`pwd`" VERSION=$(VERSION) RELEASEDATE=$(RELEASEDATE) -C doc clean html
	-tools/spellcheck.sh

doc/example.rst: example-code.py tools/example2rst.py src/apswversion.h
	rm -f dbfile
	env PYTHONPATH=. $(PYTHON) tools/example2rst.py

doc/.static:
	mkdir -p doc/.static

# This is probably gnu make specific but only developers use this makefile
$(GENDOCS): doc/%.rst: src/%.c tools/code2rst.py
	env PYTHONPATH=. $(PYTHON) tools/code2rst.py $(SQLITEVERSION) $< $@

build_ext:
	env APSW_FORCE_DISTUTILS=t $(PYTHON) setup.py fetch --version=$(SQLITEVERSION) --all build_ext --inplace --force --enable-all-extensions

coverage:
	env APSW_FORCE_DISTUTILS=t $(PYTHON) setup.py fetch --version=$(SQLITEVERSION) --all && env APSW_PY_COVERAGE=t tools/coverage.sh

test: build_ext
	env PYTHONHASHSEED=random APSW_FORCE_DISTUTILS=t $(PYTHON) tests.py

debugtest:
	gcc -pthread -fno-strict-aliasing -g -fPIC -Wall -DAPSW_USE_SQLITE_CONFIG=\"sqlite3/sqlite3config.h\" -DEXPERIMENTAL -DSQLITE_DEBUG -DAPSW_USE_SQLITE_AMALGAMATION=\"sqlite3.c\" -DAPSW_NO_NDEBUG -DAPSW_TESTFIXTURES -I`$(PYTHON) -c "import distutils.sysconfig,sys; sys.stdout.write(distutils.sysconfig.get_python_inc())"` -I. -Isqlite3 -Isrc -c src/apsw.c
	gcc -pthread -g -shared apsw.o -o apsw.so
	env PYTHONHASHSEED=random $(PYTHON) tests.py $(APSWTESTS)

# Needs a debug python.  Look at the final numbers at the bottom of
# l6, l7 and l8 and see if any are growing
valgrind: /space/pydebug/bin/python
	$(PYTHON) setup.py fetch --version=$(SQLITEVERSION) --all && \
	  env APSWTESTPREFIX=/tmp/ PATH=/space/pydebug/bin:$$PATH SHOWINUSE=t APSW_TEST_ITERATIONS=6 tools/valgrind.sh 2>&1 | tee l6 && \
	  env APSWTESTPREFIX=/tmp/ PATH=/space/pydebug/bin:$$PATH SHOWINUSE=t APSW_TEST_ITERATIONS=7 tools/valgrind.sh 2>&1 | tee l7 && \
	  env APSWTESTPREFIX=/tmp/ PATH=/space/pydebug/bin:$$PATH SHOWINUSE=t APSW_TEST_ITERATIONS=8 tools/valgrind.sh 2>&1 | tee l8

# Same as above but does just one run
valgrind1: /space/pydebug/bin/python
	$(PYTHON) setup.py fetch --version=$(SQLITEVERSION) --all && \
	  env APSWTESTPREFIX=/tmp/ PATH=/space/pydebug/bin:$$PATH SHOWINUSE=t APSW_TEST_ITERATIONS=1 tools/valgrind.sh


linkcheck:
	make RELEASEDATE=$(RELEASEDATE) VERSION=$(VERSION) -C doc linkcheck

publish: docs
	if [ -d ../apsw-publish ] ; then rm -f ../apsw-publish/* ../apsw-publish/_static/* ../apsw-publish/_sources/* ; \
	rsync -a doc/build/html/ ../apsw-publish/ ;  cd ../apsw-publish ; git status ; \
	fi

header:
	echo "#define APSW_VERSION \"$(VERSION)\"" > src/apswversion.h


# the funky test stuff is to exit successfully when grep has rc==1 since that means no lines found.
showsymbols:
	rm -f apsw`$(PYTHON) -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))"`
	$(PYTHON) setup.py fetch --all --version=$(SQLITEVERSION) build_ext --inplace --force --enable-all-extensions
	test -f apsw`$(PYTHON) -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))"`
	set +e; nm --extern-only --defined-only apsw`$(PYTHON) -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))"` | egrep -v ' (__bss_start|_edata|_end|_fini|_init|initapsw|PyInit_apsw)$$' ; test $$? -eq 1 || false

# Getting Visual Studio 2008 Express to work for 64 compilations is a
# pain, so use this builtin hidden command
WIN64HACK=win64hackvars
WINBPREFIX=fetch --version=$(SQLITEVERSION) --all build --enable-all-extensions
WINBSUFFIX=install build_test_extension test
WINBINST=bdist_wininst
WINBMSI=bdist_msi
WINBWHEEL=bdist_wheel

# You need to use the MinGW version of make.  See
# http://bugs.python.org/issue3308 if 2.6+ or 3.0+ fail to run with
# missing symbols/dll issues.  For Python 3.1 they went out of their
# way to prevent mingw from working.  You have to install msvc.
# Google for "visual c++ express edition 2008" and hope the right version
# is still available.

compile-win:
	-del /q apsw*.pyd
	-del /q dist\\*.egg
	-cmd /c del /s /q __pycache__
	cmd /c del /s /q dist
	cmd /c del /s /q build
	cmd /c del /s /q apsw.egg-info
	-cmd /c md dist
	-cmd /c del /s /q c:\\python310-32\\lib\\site-packages\\*apsw*
	c:/python310-32/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBMSI) $(WINBWHEEL)
	-cmd /c del /s /q c:\\python310\\lib\\site-packages\\*apsw*
	"c:\program files (x86)\microsoft visual studio 14.0\vc\vcvarsall.bat" amd64 & c:/python310/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBMSI) $(WINBWHEEL)
	-cmd /c del /s /q c:\\python39-32\\lib\\site-packages\\*apsw*
	c:/python39-32/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI) $(WINBWHEEL)
	-cmd /c del /s /q c:\\python39\\lib\\site-packages\\*apsw*
	"c:\program files (x86)\microsoft visual studio 14.0\vc\vcvarsall.bat" amd64 & c:/python39/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI) $(WINBWHEEL)
	-cmd /c del /s /q c:\\python38\\lib\\site-packages\\*apsw*
	c:/python38/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI) $(WINBWHEEL)
	-cmd /c del /s /q c:\\python38-64\\lib\\site-packages\\*apsw*
	"c:\program files (x86)\microsoft visual studio 14.0\vc\vcvarsall.bat" amd64 & c:/python38-64/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI) $(WINBWHEEL)
	-cmd /c del /s /q c:\\python37\\lib\\site-packages\\*apsw*
	c:/python37/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI) $(WINBWHEEL)
	-cmd /c del /s /q c:\\python37-64\\lib\\site-packages\\*apsw*
	"c:\program files (x86)\microsoft visual studio 14.0\vc\vcvarsall.bat" amd64 & c:/python37-64/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)  $(WINBWHEEL)
	-cmd /c del /s /q c:\\python36\\lib\\site-packages\\*apsw*
	c:/python36/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI) $(WINBWHEEL)
	-cmd /c del /s /q c:\\python36-64\\lib\\site-packages\\*apsw*
	"c:\program files (x86)\microsoft visual studio 14.0\vc\vcvarsall.bat" amd64 & c:/python36-64/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI) $(WINBWHEEL)
	set APSW_FORCE_DISTUTILS=t & "c:\program files (x86)\microsoft visual studio 14.0\vc\vcvarsall.bat" amd64 & c:/python35-64/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python34/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python34-64/python setup.py $(WIN64HACK) $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python33/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python33-64/python setup.py $(WIN64HACK) $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python32/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python32-64/python setup.py  $(WIN64HACK) $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python31/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python31-64/python setup.py  $(WIN64HACK) $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python27/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python27-64/python setup.py  $(WIN64HACK) $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python26/python setup.py $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python26-64/python setup.py $(WIN64HACK) $(WINBPREFIX) $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python25/python setup.py $(WINBPREFIX) --compile=mingw32 $(WINBSUFFIX) $(WINBINST) $(WINBMSI)
	set APSW_FORCE_DISTUTILS=t & c:/python24/python setup.py $(WINBPREFIX) --compile=mingw32 $(WINBSUFFIX) $(WINBINST)
	set APSW_FORCE_DISTUTILS=t & c:/python23/python setup.py $(WINBPREFIX) --compile=mingw32 $(WINBSUFFIX) $(WINBINST)
	del dist\\*.egg

setup-wheel:
	c:/python310/python -m ensurepip
	c:/python310/python -m pip install --upgrade wheel setuptools
	c:/python310-32/python -m ensurepip
	c:/python310-32/python -m pip install --upgrade wheel setuptools
	c:/python39/python -m ensurepip
	c:/python39/python -m pip install --upgrade wheel setuptools
	c:/python39-32/python -m ensurepip
	c:/python39-32/python -m pip install --upgrade wheel setuptools
	c:/python38/python -m ensurepip
	c:/python38/python -m pip install --upgrade wheel setuptools
	c:/python38-64/python -m ensurepip
	c:/python38-64/python -m pip install --upgrade wheel setuptools
	c:/python37/python -m ensurepip
	c:/python37/python -m pip install --upgrade wheel setuptools
	c:/python37-64/python -m ensurepip
	c:/python37-64/python -m pip install --upgrade wheel setuptools
	c:/python36/python -m ensurepip
	c:/python36/python  -m pip install --upgrade wheel setuptools
	c:/python36-64/python -m ensurepip
	c:/python36-64/python -m pip install --upgrade wheel setuptools


source_nocheck: docs
	env APSW_FORCE_DISTUTILS=t $(PYTHON) setup.py sdist --formats zip --add-doc

# Make the source and then check it builds and tests correctly.  This will catch missing files etc
source: source_nocheck
	mkdir -p work
	rm -rf work/$(VERDIR)
	cd work ; unzip -q ../dist/$(VERDIR).zip
# Make certain various files do/do not exist
	for f in doc/vfs.html doc/_sources/pysqlite.txt tools/apswtrace.py ; do test -f work/$(VERDIR)/$$f ; done
	for f in sqlite3.c sqlite3/sqlite3.c debian/control ; do test ! -f work/$(VERDIR)/$$f ; done
# Test code works
	cd work/$(VERDIR) ; $(PYTHON) setup.py fetch --version=$(SQLITEVERSION) --all build_ext --inplace --enable-all-extensions build_test_extension test

release:
	test -f dist/$(VERDIR).zip
	test -f dist/$(VERDIR).win32-py2.3.exe
	test -f dist/$(VERDIR).win32-py2.4.exe
	test -f dist/$(VERWIN).win32-py2.5.exe
	test -f dist/$(VERWIN).win32-py2.5.msi
	test -f dist/$(VERWIN).win32-py2.6.exe
	test -f dist/$(VERWIN).win32-py2.6.msi
	test -f dist/$(VERWIN).win-amd64-py2.6.exe
	test -f dist/$(VERWIN).win-amd64-py2.6.msi
	test -f dist/$(VERWIN).win32-py2.7.exe
	test -f dist/$(VERWIN).win32-py2.7.msi
	test -f dist/$(VERWIN).win-amd64-py2.7.exe
	test -f dist/$(VERWIN).win-amd64-py2.7.msi
	test -f dist/$(VERWIN).win32-py3.1.exe
	test -f dist/$(VERWIN).win32-py3.1.msi
	test -f dist/$(VERWIN).win-amd64-py3.1.exe
	test -f dist/$(VERWIN).win-amd64-py3.1.msi
	test -f dist/$(VERWIN).win32-py3.2.exe
	test -f dist/$(VERWIN).win32-py3.2.msi
	test -f dist/$(VERWIN).win-amd64-py3.2.exe
	test -f dist/$(VERWIN).win-amd64-py3.2.msi
	test -f dist/$(VERWIN).win32-py3.3.exe
	test -f dist/$(VERWIN).win32-py3.3.msi
	test -f dist/$(VERWIN).win-amd64-py3.3.exe
	test -f dist/$(VERWIN).win-amd64-py3.3.msi
	test -f dist/$(VERWIN).win32-py3.4.exe
	test -f dist/$(VERWIN).win32-py3.4.msi
	test -f dist/$(VERWIN).win-amd64-py3.4.exe
	test -f dist/$(VERWIN).win-amd64-py3.4.msi
	test -f dist/$(VERWIN).win-amd64-py3.5.exe
	test -f dist/$(VERWIN).win-amd64-py3.5.msi
	test -f dist/$(VERWIN).win32-py3.6.exe
	test -f dist/$(VERWIN).win32-py3.6.msi
	test -f	dist/$(VERWIN)-cp36-cp36m-win32.whl
	test -f dist/$(VERWIN).win-amd64-py3.6.exe
	test -f dist/$(VERWIN).win-amd64-py3.6.msi
	test -f dist/$(VERWIN)-cp36-cp36m-win_amd64.whl
	test -f dist/$(VERWIN).win32-py3.7.exe
	test -f dist/$(VERWIN).win32-py3.7.msi
	test -f dist/$(VERWIN)-cp37-cp37m-win32.whl
	test -f dist/$(VERWIN).win-amd64-py3.7.exe
	test -f dist/$(VERWIN).win-amd64-py3.7.msi
	test -f dist/$(VERWIN)-cp37-cp37m-win_amd64.whl
	test -f dist/$(VERWIN).win32-py3.8.exe
	test -f dist/$(VERWIN).win32-py3.8.msi
	test -f dist/$(VERWIN)-cp38-cp38-win32.whl
	test -f dist/$(VERWIN).win-amd64-py3.8.exe
	test -f dist/$(VERWIN).win-amd64-py3.8.msi
	test -f dist/$(VERWIN)-cp38-cp38-win_amd64.whl
	test -f dist/$(VERWIN).win32-py3.9.exe
	test -f dist/$(VERWIN).win32-py3.9.msi
	test -f dist/$(VERWIN)-cp39-cp39-win32.whl
	test -f dist/$(VERWIN).win-amd64-py3.9.exe
	test -f dist/$(VERWIN).win-amd64-py3.9.msi
	test -f dist/$(VERWIN)-cp39-cp39-win_amd64.whl
	test -f dist/$(VERWIN).win32-py3.10.msi
	test -f dist/$(VERWIN)-cp39-cp310-win32.whl
	test -f dist/$(VERWIN).win-amd64-py3.10.msi
	test -f dist/$(VERWIN)-cp310-cp310-win_amd64.whl
	-rm -f dist/$(VERDIR)-sigs.zip dist/*.asc
	for f in dist/* ; do gpg --use-agent --armor --detach-sig "$$f" ; done
	cd dist ; zip -m $(VERDIR)-sigs.zip *.asc

tags:
	rm -f TAGS
	ctags-exuberant -e --recurse --exclude=build --exclude=work .
