# vncsixel - a VNC client that renders into the terminal via SIXEL.
#
# The libraries can be picked up either from pkg-config or from a plain
# prefix. Override PREFIX if libvncclient / libsixel live somewhere else:
#
#   make PREFIX=/usr/local
#
# Run the end to end test with:
#
#   make test

PROG        := vnt
TESTPROG    := vnctestserver

PREFIX      ?= /tmp/opencode/install
PKG_CONFIG  ?= pkg-config

CC          ?= cc
CFLAGS      ?= -O2 -g
CFLAGS      += -Wall -Wextra -Wno-unused-parameter
CPPFLAGS    += -Isrc
LDFLAGS     ?=

# --- library discovery ------------------------------------------------
HAVE_PKGCONFIG := $(shell command -v $(PKG_CONFIG) >/dev/null 2>&1 && \
                    $(PKG_CONFIG) --exists libsixel && echo yes)

ifeq ($(HAVE_PKGCONFIG),yes)
  SIXEL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libsixel)
  SIXEL_LIBS   := $(shell $(PKG_CONFIG) --libs libsixel)
  VNC_CFLAGS   := $(shell $(PKG_CONFIG) --cflags libvncclient 2>/dev/null)
  VNC_LIBS     := $(shell $(PKG_CONFIG) --libs libvncclient 2>/dev/null || echo -lvncclient)
else
  SIXEL_CFLAGS := -I$(PREFIX)/include
  SIXEL_LIBS   := -L$(PREFIX)/lib -lsixel
  VNC_CFLAGS   := -I$(PREFIX)/include
  VNC_LIBS     := -L$(PREFIX)/lib -lvncclient
endif

LDLIBS := $(VNC_LIBS) $(SIXEL_LIBS)
ALL_CPPFLAGS := $(CPPFLAGS) $(SIXEL_CFLAGS) $(VNC_CFLAGS)

# Where to find the shared libraries at run time.
# --disable-new-dtags makes the linker emit DT_RPATH instead of DT_RUNPATH.
# DT_RPATH is inherited by transitive dependencies (libvncserver.so needs
# libjpeg.so.62, which is not a direct dependency of our binaries), while
# DT_RUNPATH is not.
RUNPATH := -Wl,--disable-new-dtags -Wl,-rpath,$(PREFIX)/lib

OBJDIR := build

CLIENT_OBJS := $(OBJDIR)/vncsixel.o $(OBJDIR)/scaler.o $(OBJDIR)/sixel_out.o $(OBJDIR)/terminal.o
SERVER_OBJS := $(OBJDIR)/vnctestserver.o

.PHONY: all clean test

all: $(PROG) $(TESTPROG)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(ALL_CPPFLAGS) -c $< -o $@

$(OBJDIR)/%.o: tests/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(ALL_CPPFLAGS) -c $< -o $@

$(PROG): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(RUNPATH) -o $@ $^ $(LDLIBS)

$(TESTPROG): $(SERVER_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(RUNPATH) -o $@ $^ -L$(PREFIX)/lib -lvncserver

$(OBJDIR)/vncsixel.o:      src/vncsixel.c src/scaler.h src/sixel_out.h src/terminal.h
$(OBJDIR)/scaler.o:         src/scaler.c src/scaler.h
$(OBJDIR)/sixel_out.o:      src/sixel_out.c src/sixel_out.h
$(OBJDIR)/terminal.o:       src/terminal.c src/terminal.h
$(OBJDIR)/vnctestserver.o:  tests/vnctestserver.c

clean:
	rm -rf $(OBJDIR) $(PROG) $(TESTPROG)

test: all
	./tests/run_e2e.sh
