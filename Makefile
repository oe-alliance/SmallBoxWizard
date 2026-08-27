CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -Os -pipe
LDFLAGS ?=

CPPFLAGS += -Iinclude
CFLAGS += -std=c11 -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith

PROGRAM := smallbox-wizard
SOURCES := \
	src/main.c \
	src/ui.c \
	src/input.c \
	src/process.c \
	src/storage.c \
	src/multiboot.c \
	src/network.c
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean install

all: $(PROGRAM)

$(PROGRAM): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: $(PROGRAM)
	install -d $(DESTDIR)/usr/sbin
	install -m 0755 $(PROGRAM) $(DESTDIR)/usr/sbin/$(PROGRAM)

clean:
	rm -f $(OBJECTS) $(PROGRAM)
