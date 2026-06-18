# 3dfx Linux Flash

Experimental Linux userspace flasher for 3dfx video BIOS ROMs.

This tool accesses PCI configuration space and flash EEPROMs directly. Use it
at your own risk, and always save the original ROM before flashing.

It scans for 3dfx PCI devices using vendor ID `121A`.

Supported device IDs:

```text
0003  Banshee
0004  Avenger / Voodoo3
0005  Avenger / Voodoo3
0009  VSA-100 / Napalm / Voodoo4 / Voodoo5
000B  Napalm2
0010  Rampage
```

The VSA-100/Napalm path has been tested on real hardware. Banshee, Voodoo3,
Napalm2, and Rampage support should be treated as untested until verified on
those cards.

## Build

```sh
make
```

## Usage

Run as root from a real Linux boot.

List supported 3dfx PCI devices:

```sh
sudo ./3dfx_flash --list
```

Identify the flash chip on the first matching board:

```sh
sudo ./3dfx_flash --index 0 --identify
```

Save the current ROM:

```sh
sudo ./3dfx_flash --index 0 --save save.rom
```

Flash a ROM:

```sh
sudo ./3dfx_flash --index 0 --flash M4800L.ROM --yes
```

## Notes

- This tool needs direct PCI resource access and I/O privileges.
- If a Linux graphics/framebuffer driver has claimed the card, unbind it first
  or boot with the card unused as a display adapter.
- The write path erases the EEPROM before programming. Keep a known-good PCI
  recovery card and a saved ROM nearby.
- If the card has a bad BIOS and Linux still boots without visible output, SSH
  from another machine can be used to reflash it without needing local display.
- Always start with `--list`, `--identify`, and `--save` before trying a write
  on a newly supported card family.
