WAYLAND_PROTOCOLS=$(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER=$(shell pkg-config --variable=wayland_scanner wayland-scanner)
LIBS=\
	 $(shell pkg-config --cflags --libs "wlroots >= 0.17.0") \
	 $(shell pkg-config --cflags --libs wayland-server) \
	 $(shell pkg-config --cflags --libs wayland-client) \
	 $(shell pkg-config --cflags --libs xkbcommon) \
	 $(shell pkg-config --cflags --libs cairo) \
	 $(shell pkg-config --cflags --libs pangocairo)

# Detect whether this wlroots build includes XWayland support.
# wlroots must have been compiled with -Dxwayland=enabled.
XWAYLAND_SUPPORT := $(shell pkg-config --variable=have_xwayland wlroots 2>/dev/null)
ifeq ($(XWAYLAND_SUPPORT),true)
  XWAYLAND_CFLAGS =
else
  # Older wlroots versions don't export have_xwayland; fall back to checking
  # whether the header exists.
  XWAYLAND_HDR := $(shell pkg-config --variable=includedir wlroots 2>/dev/null)/wlr/xwayland.h
  ifeq ($(wildcard $(XWAYLAND_HDR)),$(XWAYLAND_HDR))
    XWAYLAND_CFLAGS =
  else
    # XWayland header not present — compile without it.
    XWAYLAND_CFLAGS = -DTINYWL_NO_XWAYLAND
  endif
endif

# wayland-scanner is a tool which generates C headers and rigging for Wayland
# protocols, which are specified in XML. wlroots requires you to rig these up
# to your build system yourself and provide them in the include path.
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

tinywl: tinywl.c services.c menu.c background.c panel.c window-state.c tinywl.h services.h window-state.h xdg-shell-protocol.h
	$(CC) $(CFLAGS) \
		-g -Werror -I. \
		-DWLR_USE_UNSTABLE \
		$(XWAYLAND_CFLAGS) \
		-o $@ tinywl.c services.c menu.c background.c panel.c window-state.c \
		$(LIBS)

clean:
	rm -f tinywl xdg-shell-protocol.h xdg-shell-protocol.c
	@echo "done"

install: tinywl
	cp tinywl /bin/tinywl
	@echo "tinywl installed to /bin/tinywl"

uninstall:
	rm -f /bin/tinywl
	@echo "tinywl removed from /bin/tinywl"

.DEFAULT_GOAL=tinywl
.PHONY: clean install uninstall
