CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

.PHONY: all clean

all: 3dfx_vbios_flash 3dfx_edid_flash

3dfx_vbios_flash: 3dfx_vbios_flash.c
	$(CC) $(CFLAGS) -o $@ $<

3dfx_edid_flash: 3dfx_edid_flash.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f 3dfx_vbios_flash 3dfx_edid_flash
