LD = ld
CC = cc
PKG_CONFIG = pkg-config
INSTALL = install
CFLAGS = -O2 -Wall -Wextra
LDFLAGS =
LIBS =
VLC_PLUGIN_CFLAGS := $(shell $(PKG_CONFIG) --cflags vlc-plugin)
VLC_PLUGIN_LIBS := $(shell $(PKG_CONFIG) --libs vlc-plugin)
VLC_PLUGIN_DIR := $(shell $(PKG_CONFIG) --variable=pluginsdir vlc-plugin)

plugindir = $(VLC_PLUGIN_DIR)/misc

override CC += -std=gnu11
override CPPFLAGS += -DPIC -I. -I/x/osd/vlcosd/src-vlc-3.0.18/include
override CFLAGS += -fPIC
override LDFLAGS += -Wl,-no-undefined
# Strip output
override LDFLAGS += -s

override CPPFLAGS += -DMODULE_STRING=\"fpvosd\"
override CFLAGS += $(VLC_PLUGIN_CFLAGS)
override LIBS += $(VLC_PLUGIN_LIBS)

SUFFIX := so
ifeq ($(OS),Windows_NT)
	SUFFIX := dll
endif

all: libfpvosd_plugin.$(SUFFIX)

install: all
	echo $(CFLAGS)
	mkdir -p -- $(DESTDIR)$(plugindir)
	$(INSTALL) --mode 0755 libfpvosd_plugin.$(SUFFIX) $(DESTDIR)$(plugindir)

install-strip:
	$(MAKE) install INSTALL="$(INSTALL) -s"

uninstall:
	rm -f $(plugindir)/libfpvosd_plugin.$(SUFFIX)

clean:
	rm -f -- libfpvosd_plugin.$(SUFFIX) *.o

mostlyclean: clean

SOURCES = fpvosd.c

$(SOURCES:%.c=%.o): $(SOURCES:%.c=%.c)

libfpvosd_plugin.$(SUFFIX): $(SOURCES:%.c=%.o)
	$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

.PHONY: all install install-strip uninstall clean mostlyclean
