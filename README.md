# 3dfx Linux Flash

Experimental Linux userspace flashing tools for 3dfx (Voodoo) cards. Two
separate utilities:

- **`3dfx_vbios_flash`** — flashes the video **BIOS ROM** (the parallel flash
  chip), through the card's ROM window in BAR0.
- **`3dfx_edid_flash`** — flashes the **EDID EEPROM** (a serial I2C `24LC08B`)
  over the card's DDC bus, for boards that carry an on-board EDID. No desolder.

Both access PCI resources directly. Use them at your own risk, and always save
the original ROM/EDID before writing. They scan for 3dfx PCI devices by vendor
ID `121A`.

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
make            # builds both: 3dfx_vbios_flash and 3dfx_edid_flash
```

Run either as root, from a real Linux boot, with the card not actively driving
a display (stop your display manager / unbind any KMS driver first).

## 3dfx_vbios_flash — video BIOS ROM

```sh
sudo ./3dfx_vbios_flash --list
sudo ./3dfx_vbios_flash --index 0 --identify
sudo ./3dfx_vbios_flash --index 0 --save save.rom
sudo ./3dfx_vbios_flash --index 0 --flash M4800L.ROM --yes
```

The write path erases the EEPROM before programming. Keep a known-good PCI
recovery card and a saved ROM nearby. If the card has a bad BIOS and Linux
still boots without visible output, SSH from another machine can reflash it.

## 3dfx_edid_flash — EDID EEPROM

For custom cards that present an on-board EDID, this writes a 256-byte EDID
into the `24LC08B` over the card's single DDC/I2C bus. On these boards the DDC
bus is muxed and `GPIO_1` (vidSerialParallelPort bit 29) selects the digital /
on-board side, where the EEPROM lives; the tool drives it automatically.

```sh
sudo ./3dfx_edid_flash --list
sudo ./3dfx_edid_flash --index 0 --identify          # show the current EDID
sudo ./3dfx_edid_flash --index 0 --save current.bin  # read it out
sudo ./3dfx_edid_flash --index 0 --flash V4.bin --yes
sudo ./3dfx_edid_flash --index 0 --flash V4.bin --yes --force   # blank/fresh chip
```

Safety:

- It refuses to flash unless the bus proves itself — either it reads back a
  **valid EDID header**, or (for a blank chip, with `--force`) the EEPROM
  **ACKs its I2C address**. Otherwise it aborts rather than write into thin air.
- It backs up the current EEPROM to `edid_backup.bin` before writing, then
  **reads back and verifies** the whole 256 bytes.
- The EEPROM's write-protect pin must permit writes (e.g. WP tied to GND).
- Tip: prepare the EDID so its detailed timing matches a mode the panel/encoder
  actually locks (correct sync polarity and porches), or the card will faithfully
  advertise a timing that does not display.

## Notes

- Both tools need direct PCI resource access (run as root).
- If a Linux graphics/framebuffer driver has claimed the card, stop X / unbind
  it first, or boot with the card unused as a display adapter.
- Always start with `--list`, `--identify`, and `--save` before any write.
