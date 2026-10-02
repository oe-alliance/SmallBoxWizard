CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -Os -pipe
LDFLAGS ?=

# LVGL is the submodule lib/lvgl, configured by include/lv_conf.h.
CPPFLAGS += -Iinclude -Ilib/lvgl -DLV_CONF_INCLUDE_SIMPLE
CFLAGS += -std=c11 -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith
SECTIONS := -ffunction-sections -fdata-sections

PROGRAM := smallbox-wizard
SOURCES := \
	src/main.c \
	src/about.c \
	src/viewer.c \
	src/ui.c \
	src/input.c \
	src/process.c \
	src/storage.c \
	src/multiboot.c \
	src/network.c
LIBRARY_SOURCES := $(wildcard src/fonts/*.c) $(shell find lib/lvgl/src -name '*.c')
OBJECTS := $(SOURCES:.c=.o)
LIBRARY_OBJECTS := $(LIBRARY_SOURCES:.c=.o)
DEPENDS := $(OBJECTS:.o=.d) $(LIBRARY_OBJECTS:.o=.d)

.PHONY: all clean install

all: $(PROGRAM)

$(PROGRAM): $(OBJECTS) $(LIBRARY_OBJECTS)
	$(CC) $(LDFLAGS) -Wl,--gc-sections -o $@ $(OBJECTS) $(LIBRARY_OBJECTS) -lm

# Without the warnings of the wizard, in the C dialect of LVGL.
LIBRARY_CFLAGS = $(filter-out -std=% -W%,$(CFLAGS)) -std=gnu11

src/fonts/%.o: src/fonts/%.c
	$(CC) $(CPPFLAGS) $(LIBRARY_CFLAGS) $(SECTIONS) -MMD -MP -c -o $@ $<

lib/%.o: lib/%.c
	$(CC) $(CPPFLAGS) $(LIBRARY_CFLAGS) $(SECTIONS) -MMD -MP -c -o $@ $<

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SECTIONS) -MMD -MP -c -o $@ $<

install: $(PROGRAM)
	install -d $(DESTDIR)/usr/sbin
	install -m 0755 $(PROGRAM) $(DESTDIR)/usr/sbin/$(PROGRAM)

clean:
	rm -f $(OBJECTS) $(LIBRARY_OBJECTS) $(DEPENDS) $(PROGRAM)

-include $(wildcard $(DEPENDS))
