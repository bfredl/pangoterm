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

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin

all: pangoterm

pango%: pango%.c
	@echo LINK $@
	@$(LIBTOOL) --mode=link --tag=CC $(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

.PHONY: clean
clean:
	$(LIBTOOL) --mode=clean rm -f pangoterm

.PHONY: install
install: install-bin

# rm the old binary first in case it's still in use
install-bin: pangoterm
	install -d $(DESTDIR)$(BINDIR)
	$(LIBTOOL) --mode=install cp --remove-destination pangoterm $(DESTDIR)$(BINDIR)/pangoterm
