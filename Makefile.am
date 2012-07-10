MAINTAINERCLEANFILES = Makefile.in aclocal.m4 configure config.h.in config.h.in~ stamp-h.in

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src

EXTRA_DIST = README.md NOTICE LICENSE ChangeLog scripts notes

test:
	env PYTHONPATH=$(pwd)/tests\:$(PYTHONPATH) ${SHELL} tests/pytest.sh

clean-local:
	rm -f tests/config/defaults.py
	rm -f tests/config/server/default.py
	rm -f tests/config/data/default.py
	rm -f tests/data/data.default

