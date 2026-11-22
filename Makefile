PREFIX ?= /usr/local
DESTDIR ?=
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
CFLAGS ?= -O2
CXXFLAGS ?= -Os

VERSION := $(strip $(shell cat VERSION))
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client)
SOURCES := main.cpp copy.cpp paste.cpp terminate.cpp
HEADERS := common.hpp config.hpp paste.hpp protocol.hpp
PROTOCOLS := ext-data-control wlr-data-control
PROTOCOL_OBJECTS := $(addprefix protocols/,$(addsuffix .o,$(PROTOCOLS)))
GENERATED := $(foreach ext,c h o,$(addprefix protocols/,$(addsuffix .$(ext),$(PROTOCOLS))))

.PHONY: all install clean
all: clip-bridge

protocols/ext-data-control.c protocols/ext-data-control.h &: protocols/ext-data-control-v1.xml
	$(WAYLAND_SCANNER) client-header $< protocols/ext-data-control.h
	$(WAYLAND_SCANNER) private-code $< protocols/ext-data-control.c

protocols/wlr-data-control.c protocols/wlr-data-control.h &: protocols/wlr-data-control-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< protocols/wlr-data-control.h
	$(WAYLAND_SCANNER) private-code $< protocols/wlr-data-control.c

protocols/%.o: protocols/%.c protocols/%.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_CFLAGS) -c $< -o $@

clip-bridge: $(SOURCES) $(HEADERS) $(PROTOCOL_OBJECTS) VERSION Makefile
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++23 -flto -ffunction-sections -fdata-sections -fno-rtti \
		-Wall -Wextra -Wpedantic -static-libstdc++ -static-libgcc \
		-DCLIP_BRIDGE_VERSION='"$(VERSION)"' $(WAYLAND_CFLAGS) $(SOURCES) $(PROTOCOL_OBJECTS) \
		$(LDFLAGS) -Wl,--gc-sections -Wl,--as-needed -o $@.new $(WAYLAND_LIBS) $(LDLIBS)
	mv -f $@.new $@

install: clip-bridge
	install -Dm755 clip-bridge "$(DESTDIR)$(PREFIX)/bin/clip-bridge"
	install -Dm644 LICENSE "$(DESTDIR)$(PREFIX)/share/licenses/clip-bridge/LICENSE"
	install -Dm644 README.md "$(DESTDIR)$(PREFIX)/share/doc/clip-bridge/README.md"
	install -Dm644 protocols/ext-data-control-v1.xml "$(DESTDIR)$(PREFIX)/share/licenses/clip-bridge/ext-data-control-v1.xml"
	install -Dm644 protocols/wlr-data-control-unstable-v1.xml "$(DESTDIR)$(PREFIX)/share/licenses/clip-bridge/wlr-data-control-unstable-v1.xml"

clean:
	$(RM) clip-bridge clip-bridge.new $(GENERATED)
