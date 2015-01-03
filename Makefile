ifeq ($(shell uname),Darwin)
    LIBTOOL ?= glibtool
else
    LIBTOOL ?= libtool
endif

ifneq ($(VERBOSE),1)
    LIBTOOL +=--quiet
endif

CFLAGS  +=-Wall -Iinclude -std=c99
LDFLAGS +=-lutil

ifeq ($(DEBUG),1)
  CFLAGS +=-ggdb -DDEBUG
endif

ifeq ($(PROFILE),1)
  CFLAGS +=-pg
  LDFLAGS+=-pg
endif

CFLAGS  +=$(shell pkg-config --cflags vterm)
LDFLAGS +=$(shell pkg-config --libs   vterm)

CFLAGS  +=$(shell pkg-config --cflags gtk+-2.0)
LDFLAGS +=$(shell pkg-config --libs   gtk+-2.0)

CFLAGS  +=$(shell pkg-config --cflags cairo)
LDFLAGS +=$(shell pkg-config --libs   cairo)

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
SHAREDIR=$(PREFIX)/share

CFLAGS+=-DPANGOTERM_SHAREDIR="\"$(SHAREDIR)\""

HFILES=$(wildcard *.h)
CFILES=$(wildcard *.c)
OBJECTS=$(CFILES:.c=.lo)

all: pangoterm

pangoterm: $(OBJECTS)
	@echo LINK $@
	@$(LIBTOOL) --mode=link --tag=CC $(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.lo: %.c $(HFILES)
	@echo CC $<
	@$(LIBTOOL) --mode=compile --tag=CC $(CC) $(CFLAGS) -o $@ -c $<

.PHONY: clean
clean:
	$(LIBTOOL) --mode=clean rm -f $(OBJECTS)
	$(LIBTOOL) --mode=clean rm -f pangoterm

.PHONY: install
install: install-bin install-share

# rm the old binary first in case it's still in use
install-bin: pangoterm
	install -d $(DESTDIR)$(BINDIR)
	$(LIBTOOL) --mode=install cp --remove-destination pangoterm $(DESTDIR)$(BINDIR)/pangoterm

install-share:
	install -d $(DESTDIR)$(SHAREDIR)/pixmaps
	$(LIBTOOL) --mode=install cp pangoterm.svg $(DESTDIR)$(SHAREDIR)/pixmaps/
	install -d $(DESTDIR)$(SHAREDIR)/applications
	$(LIBTOOL) --mode=install cp pangoterm.desktop $(DESTDIR)$(SHAREDIR)/applications/

# DIST CUT

VERSION=0

DISTDIR=pangoterm-$(VERSION)

distdir: all
	mkdir __distdir
	cp LICENSE __distdir
	cp *.c *.h __distdir
	cp pangoterm.svg pangoterm.desktop pangoterm.cfg __distdir
	sed "/^# DIST CUT/Q" <Makefile >__distdir/Makefile
	mv __distdir $(DISTDIR)

TARBALL=$(DISTDIR).tar.gz

dist: distdir
	tar -czf $(TARBALL) $(DISTDIR)
	rm -rf $(DISTDIR)

dist+bzr:
	$(MAKE) dist VERSION=$(VERSION)+bzr`bzr revno`

distdir+bzr:
	$(MAKE) distdir VERSION=$(VERSION)+bzr`bzr revno`
