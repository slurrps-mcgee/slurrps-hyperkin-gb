# Retron SQ firmware programs, all built on libsqcart (lib/):
#   sqload               loading screen + first copy of a cartridge
#   sqcart               the cartridge library for shell scripts
#   sqtools_libretro.so  Cartridge Tools, a libretro core run by RetroArch
#   sqhome               the home screen
#
# CC, CFLAGS and LDFLAGS come from Buildroot (retronsq.mk) or the tests,
# on the make command line; "override" adds ours to theirs.

override CFLAGS += -std=gnu99 -Wall -Wno-format-truncation
LIB_SRC = lib/common.c lib/crc.c lib/copy.c lib/cart.c lib/save.c lib/cartwrite.c \
	lib/config.c lib/library.c lib/actions.c
# Position-independent so the same objects link into the libretro core
LIB_OBJ = $(LIB_SRC:.c=.o)

all: sqload sqcart sqtools_libretro.so sqhome

lib/%.o: lib/%.c lib/sqcart.h
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

libsqcart.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

sqload: load/sqload.c libsqcart.a
	$(CC) $(CFLAGS) -o $@ load/sqload.c libsqcart.a $(LDFLAGS) \
		-lSDL2 -lSDL2_image -lSDL2_ttf -lm -lpthread

sqhome: home/home.c home/ui.c home/input.c home/home.h libsqcart.a
	$(CC) $(CFLAGS) -o $@ home/home.c home/ui.c home/input.c libsqcart.a $(LDFLAGS) \
		-lSDL2 -lSDL2_image -lSDL2_ttf -lpthread

sqcart: cli/sqcart.c libsqcart.a
	$(CC) $(CFLAGS) -o $@ cli/sqcart.c libsqcart.a $(LDFLAGS) -lpthread

sqtools_libretro.so: tools/sqtools.c tools/bitmap.h tools/libretro.h libsqcart.a
	$(CC) $(CFLAGS) -fPIC -shared -o $@ tools/sqtools.c libsqcart.a $(LDFLAGS) \
		-lpthread -Wl,--no-undefined

clean:
	rm -f lib/*.o libsqcart.a sqload sqcart sqtools_libretro.so sqhome

.PHONY: all clean
