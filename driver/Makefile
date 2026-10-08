# Builds libpica.a for the 3DS with devkitARM.
#
#   make            -> lib/libpica.a (release) and lib/libpicad.a (debug)
#   make examples   -> examples/*/ *.3dsx
#   make install    -> copies headers and libraries to $(DEVKITPRO)/portlibs/3ds
#
# Requires DEVKITPRO/DEVKITARM to be set (devkitPro's 3ds-dev group).

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

PREFIX   := $(DEVKITARM)/bin/arm-none-eabi-
CC       := $(PREFIX)gcc
AR       := $(PREFIX)gcc-ar

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := $(ARCH) -std=gnu99 -Wall -Wextra -ffunction-sections -fdata-sections \
            -D__3DS__ -Iinclude -I$(DEVKITPRO)/libctru/include

SOURCES  := $(wildcard src/*.c)
REL_OBJS := $(patsubst src/%.c,build/release/%.o,$(SOURCES))
DBG_OBJS := $(patsubst src/%.c,build/debug/%.o,$(SOURCES))

.PHONY: all clean examples install

all: lib/libpica.a lib/libpicad.a

lib/libpica.a: $(REL_OBJS)
	@mkdir -p lib
	$(AR) rcs $@ $^

lib/libpicad.a: $(DBG_OBJS)
	@mkdir -p lib
	$(AR) rcs $@ $^

build/release/%.o: src/%.c $(wildcard include/pica/*.h) src/internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O2 -c $< -o $@

build/debug/%.o: src/%.c $(wildcard include/pica/*.h) src/internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O0 -g -c $< -o $@

examples: all
	@for d in examples/*/; do $(MAKE) -C $$d || exit 1; done

install: all
	mkdir -p $(DEVKITPRO)/portlibs/3ds/include/pica $(DEVKITPRO)/portlibs/3ds/lib
	cp include/pica/*.h $(DEVKITPRO)/portlibs/3ds/include/pica/
	cp lib/libpica.a lib/libpicad.a $(DEVKITPRO)/portlibs/3ds/lib/

clean:
	rm -rf build lib
	@for d in examples/*/; do $(MAKE) -C $$d clean; done
