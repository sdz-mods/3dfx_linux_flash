#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#if defined(__i386__) || defined(__x86_64__)
#include <sys/io.h>
#endif

#define TDFX_VENDOR_ID 0x121a
#define DEV_BANSHEE   0x0003
#define DEV_AVENGER1  0x0004
#define DEV_AVENGER2  0x0005
#define DEV_NAPALM    0x0009
#define DEV_NAPALM2   0x000b
#define DEV_RAMPAGE   0x0010

#define PCI_COMMAND     0x04
#define PCI_BAR0        0x10
#define PCI_IOBAR       0x18
#define PCI_ROM_BAR     0x30

#define VSA_ROM_OFFSET  0x00a00000u
#define VSA_MAP_SIZE    0x02000000u
#define ROM_WINDOW_SIZE 0x00010000u

#define MISC_INIT0      0x10
#define MISC_INIT1      0x14

#define MANID_AMD_TI    0x01
#define MANID_ATMEL     0x1f
#define MANID_SST       0xbf

#define AMD_TI_29010    0x20
#define ATMEL_29LV512   0x3d
#define ATMEL_49BV512   0x03
#define ATMEL_49F010    0x17
#define SST_29LE512     0x3d
#define SST_29EE010     0x07
#define SST_39SF010     0xb5
#define SST_39VF512     0xd4

struct pci_dev {
    char bdf[64];
    char path[512];
    uint16_t vendor;
    uint16_t device;
    uint16_t subvendor;
    uint16_t subdevice;
    uint64_t bar0;
    uint64_t bar0_size;
    uint32_t rom_bar_orig;
    uint16_t command_orig;
    uint16_t io_base;
};

struct mapped_dev {
    struct pci_dev pci;
    int cfg_fd;
    int res0_fd;
    volatile uint8_t *mmio;
    volatile uint8_t *rom;
    volatile uint32_t *misc0;
    volatile uint32_t *misc1;
    uint32_t misc0_orig;
    uint32_t misc1_orig;
};

struct image {
    uint8_t *data;
    size_t file_size;
    size_t program_size;
};

static bool is_supported_3dfx_device(uint16_t device) {
    return device == DEV_BANSHEE ||
           device == DEV_AVENGER1 ||
           device == DEV_AVENGER2 ||
           device == DEV_NAPALM ||
           device == DEV_NAPALM2 ||
           device == DEV_RAMPAGE;
}

static bool is_napalm_device(uint16_t device) {
    return device == DEV_NAPALM || device == DEV_NAPALM2;
}

static const char *device_name(uint16_t device) {
    switch (device) {
    case DEV_BANSHEE:  return "Banshee";
    case DEV_AVENGER1:
    case DEV_AVENGER2: return "Avenger/Voodoo3";
    case DEV_NAPALM:   return "VSA-100/Napalm";
    case DEV_NAPALM2:  return "Napalm2";
    case DEV_RAMPAGE:  return "Rampage";
    default:           return "unknown";
    }
}

static void die(const char *msg) {
    fprintf(stderr, "error: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static uint16_t read_hex16_file(const char *path) {
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) die(path);
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) die(path);
    buf[n] = 0;
    return (uint16_t)strtoul(buf, NULL, 0);
}

static void path_join(char *out, size_t out_sz, const char *a, const char *b) {
    snprintf(out, out_sz, "%s/%s", a, b);
}

static bool read_resource0(struct pci_dev *dev) {
    char res_path[640];
    path_join(res_path, sizeof(res_path), dev->path, "resource");
    FILE *f = fopen(res_path, "r");
    if (!f) return false;

    unsigned long long start = 0, end = 0, flags = 0;
    if (fscanf(f, "%llx %llx %llx", &start, &end, &flags) != 3) {
        fclose(f);
        return false;
    }
    fclose(f);

    dev->bar0 = start;
    dev->bar0_size = (end >= start) ? (end - start + 1) : 0;
    return dev->bar0_size >= VSA_ROM_OFFSET + ROM_WINDOW_SIZE;
}

static int read_config(const struct pci_dev *dev, void *buf, size_t len, off_t off) {
    char cfg_path[640];
    path_join(cfg_path, sizeof(cfg_path), dev->path, "config");
    int fd = open(cfg_path, O_RDWR);
    if (fd < 0) return -1;
    ssize_t n = pread(fd, buf, len, off);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static int write_config_fd(int fd, const void *buf, size_t len, off_t off) {
    ssize_t n = pwrite(fd, buf, len, off);
    return n == (ssize_t)len ? 0 : -1;
}

static int scan_devices(struct pci_dev **out) {
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) die("open /sys/bus/pci/devices");

    size_t cap = 8, count = 0;
    struct pci_dev *list = calloc(cap, sizeof(*list));
    if (!list) die("calloc");

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        struct pci_dev dev = {0};
        size_t bdf_len = strlen(de->d_name);
        if (bdf_len >= sizeof(dev.bdf)) bdf_len = sizeof(dev.bdf) - 1;
        memcpy(dev.bdf, de->d_name, bdf_len);
        dev.bdf[bdf_len] = 0;
        snprintf(dev.path, sizeof(dev.path), "/sys/bus/pci/devices/%s", de->d_name);

        char p[640];
        path_join(p, sizeof(p), dev.path, "vendor");
        dev.vendor = read_hex16_file(p);
        if (dev.vendor != TDFX_VENDOR_ID) continue;

        path_join(p, sizeof(p), dev.path, "device");
        dev.device = read_hex16_file(p);
        if (!is_supported_3dfx_device(dev.device)) continue;

        uint32_t ssid = 0;
        if (read_config(&dev, &ssid, sizeof(ssid), 0x2c) == 0) {
            dev.subvendor = (uint16_t)(ssid & 0xffff);
            dev.subdevice = (uint16_t)(ssid >> 16);
        }

        uint32_t bar = 0, rom = 0;
        uint16_t cmd = 0;
        read_config(&dev, &bar, sizeof(bar), PCI_BAR0);
        read_config(&dev, &rom, sizeof(rom), PCI_ROM_BAR);
        read_config(&dev, &cmd, sizeof(cmd), PCI_COMMAND);
        dev.rom_bar_orig = rom;
        dev.command_orig = cmd;

        uint32_t io = 0;
        if (read_config(&dev, &io, sizeof(io), PCI_IOBAR) == 0)
            dev.io_base = (uint16_t)(io & 0xfff8);

        if (!read_resource0(&dev)) continue;

        if (count == cap) {
            cap *= 2;
            struct pci_dev *tmp = realloc(list, cap * sizeof(*list));
            if (!tmp) die("realloc");
            list = tmp;
        }
        list[count++] = dev;
    }

    closedir(d);
    *out = list;
    return (int)count;
}

static void print_device(int idx, const struct pci_dev *d) {
    printf("%d: %s %s VEN_%04X DEV_%04X SUBSYS_%04X%04X BAR0=0x%llx size=0x%llx\n",
           idx, d->bdf, device_name(d->device), d->vendor, d->device, d->subdevice, d->subvendor,
           (unsigned long long)d->bar0, (unsigned long long)d->bar0_size);
}

static void flush_bus(const struct mapped_dev *m) {
#if defined(__i386__) || defined(__x86_64__)
    if (m->pci.io_base) {
        (void)inl(m->pci.io_base);
        (void)inl(m->pci.io_base);
        return;
    }
#endif
    (void)*(volatile uint32_t *)(m->mmio);
    (void)*(volatile uint32_t *)(m->mmio);
}

static void rom_write(struct mapped_dev *m, uint16_t off, uint8_t val) {
    m->rom[off] = val;
    flush_bus(m);
}

static uint8_t rom_read(struct mapped_dev *m, uint16_t off) {
    uint8_t v = m->rom[off];
    flush_bus(m);
    return v;
}

static void send_rom_command(struct mapped_dev *m, uint8_t cmd) {
    rom_write(m, 0x5555, 0xaa);
    rom_write(m, 0x2aaa, 0x55);
    rom_write(m, 0x5555, cmd);
}

static void begin_rom(struct mapped_dev *m) {
    uint32_t rom_bar = (uint32_t)(m->pci.bar0 + VSA_ROM_OFFSET) | 1u;
    uint16_t command = m->pci.command_orig | 0x0002u;

    if (write_config_fd(m->cfg_fd, &rom_bar, sizeof(rom_bar), PCI_ROM_BAR) < 0)
        die("write PCI ROM BAR");
    if (write_config_fd(m->cfg_fd, &command, sizeof(command), PCI_COMMAND) < 0)
        die("write PCI command");

    m->misc0_orig = *m->misc0;
    if (is_napalm_device(m->pci.device))
        *m->misc0 = m->misc0_orig & 0xbfffffffu;

    m->misc1_orig = *m->misc1;
    if (m->pci.device == DEV_RAMPAGE)
        *m->misc1 = (m->misc1_orig & 0xfdffffffu) | 0x04u;
    else
        *m->misc1 = (m->misc1_orig & 0xfdffffffu) | 0x10u;

    usleep(200);
}

static void end_rom(struct mapped_dev *m) {
    *m->misc0 = m->misc0_orig;
    *m->misc1 = m->misc1_orig;
    write_config_fd(m->cfg_fd, &m->pci.rom_bar_orig, sizeof(m->pci.rom_bar_orig), PCI_ROM_BAR);
    write_config_fd(m->cfg_fd, &m->pci.command_orig, sizeof(m->pci.command_orig), PCI_COMMAND);
}

static void map_device(const struct pci_dev *dev, struct mapped_dev *m) {
    memset(m, 0, sizeof(*m));
    m->pci = *dev;
    m->cfg_fd = -1;
    m->res0_fd = -1;

    char cfg_path[640], res_path[640];
    path_join(cfg_path, sizeof(cfg_path), dev->path, "config");
    path_join(res_path, sizeof(res_path), dev->path, "resource0");

    m->cfg_fd = open(cfg_path, O_RDWR);
    if (m->cfg_fd < 0) die("open config");

    m->res0_fd = open(res_path, O_RDWR | O_SYNC);
    if (m->res0_fd < 0) die("open resource0");

    m->mmio = mmap(NULL, VSA_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m->res0_fd, 0);
    if (m->mmio == MAP_FAILED) die("mmap resource0");
    m->rom = m->mmio + VSA_ROM_OFFSET;
    m->misc0 = (volatile uint32_t *)(m->mmio + MISC_INIT0);
    m->misc1 = (volatile uint32_t *)(m->mmio + MISC_INIT1);

#if defined(__i386__) || defined(__x86_64__)
    if (m->pci.io_base && iopl(3) != 0)
        fprintf(stderr, "warning: iopl(3) failed, using MMIO reads for bus flushes: %s\n", strerror(errno));
#endif
}

static void unmap_device(struct mapped_dev *m) {
    if (m->mmio && m->mmio != MAP_FAILED) munmap((void *)m->mmio, VSA_MAP_SIZE);
    if (m->res0_fd >= 0) close(m->res0_fd);
    if (m->cfg_fd >= 0) close(m->cfg_fd);
}

static void get_rom_id(struct mapped_dev *m, uint8_t *man, uint8_t *dev, unsigned *sector) {
    send_rom_command(m, 0xf0);
    send_rom_command(m, 0x90);
    *man = rom_read(m, 0);
    *dev = rom_read(m, 1);
    send_rom_command(m, 0xf0);

    *sector = 1;
    if (*man == MANID_ATMEL && *dev != ATMEL_49BV512 && *dev != ATMEL_49F010)
        *sector = 128;
    if (*man == MANID_SST && (*dev == SST_29EE010 || *dev == SST_29LE512))
        *sector = 128;
}

static const char *flash_name(uint8_t man, uint8_t dev) {
    if (man == MANID_AMD_TI && dev == AMD_TI_29010) return "AMD/TI 29F010";
    if (man == MANID_ATMEL && dev == ATMEL_29LV512) return "Atmel AT29LV512";
    if (man == MANID_ATMEL && dev == ATMEL_49BV512) return "Atmel AT49BV512";
    if (man == MANID_ATMEL && dev == ATMEL_49F010) return "Atmel AT49F010";
    if (man == MANID_SST && dev == SST_29LE512) return "SST 29LE512";
    if (man == MANID_SST && dev == SST_29EE010) return "SST 29EE010";
    if (man == MANID_SST && dev == SST_39SF010) return "SST 39SF010";
    if (man == MANID_SST && dev == SST_39VF512) return "SST 39VF512";
    return "unknown";
}

static bool verify_write(struct mapped_dev *m, uint8_t man, uint8_t dev, uint16_t off, uint8_t expected) {
    if (man == MANID_ATMEL && dev == ATMEL_49BV512) {
        usleep(200);
        return rom_read(m, off) == expected;
    }

    for (int i = 0; i < 1000000; i++) {
        uint8_t v = rom_read(m, off);
        if (v == expected) return true;

        if (man == MANID_AMD_TI && (v & 0x20)) {
            v = rom_read(m, off);
            if (((v ^ expected) & 0x80) != 0) return false;
        }
    }
    return false;
}

static void erase_rom(struct mapped_dev *m) {
    printf("Erasing EEPROM...\n");
    send_rom_command(m, 0x80);
    send_rom_command(m, 0x10);
    for (int i = 0; i < 3000000; i++) {
        if (rom_read(m, 0) & 0x80) {
            usleep(15000);
            return;
        }
    }
    fprintf(stderr, "error: erase timeout\n");
    exit(1);
}

static struct image load_image(const char *path) {
    struct image img = {0};
    FILE *f = fopen(path, "rb");
    if (!f) die(path);
    if (fseek(f, 0, SEEK_END) != 0) die("fseek");
    long sz = ftell(f);
    if (sz <= 0 || sz > 1024 * 1024) {
        fprintf(stderr, "error: invalid ROM file size\n");
        exit(1);
    }
    rewind(f);
    img.data = malloc((size_t)sz);
    if (!img.data) die("malloc");
    if (fread(img.data, 1, (size_t)sz, f) != (size_t)sz) die("fread");
    fclose(f);
    img.file_size = (size_t)sz;

    if (img.file_size < 4 || img.data[0] != 0x55 || img.data[1] != 0xaa) {
        fprintf(stderr, "error: ROM signature 55 AA not found\n");
        exit(1);
    }

    size_t declared = (size_t)img.data[2] * 512u;
    if (declared == 0 || declared > img.file_size) declared = img.file_size;
    img.program_size = declared;

    unsigned sum = 0;
    for (size_t i = 0; i < img.program_size; i++) sum = (sum + img.data[i]) & 0xff;
    if (sum != 0) {
        fprintf(stderr, "error: ROM checksum over declared %zu bytes is %02X, refusing to flash\n",
                img.program_size, sum);
        exit(1);
    }
    return img;
}

static void save_rom(struct mapped_dev *m, const char *path, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) die(path);
    for (size_t i = 0; i < size; i++) {
        uint8_t v = rom_read(m, (uint16_t)i);
        if (fwrite(&v, 1, 1, f) != 1) die("fwrite");
    }
    fclose(f);
}

static void flash_image(struct mapped_dev *m, const struct image *img, uint8_t man, uint8_t dev, unsigned sector) {
    erase_rom(m);
    printf("Programming %zu bytes...\n", img->program_size);

    unsigned counter = sector;
    for (size_t i = 0; i < img->program_size; i++) {
        if (counter >= sector) {
            send_rom_command(m, 0xa0);
            counter = 0;
        }
        rom_write(m, (uint16_t)i, img->data[i]);
        counter++;
        if (counter >= sector && !verify_write(m, man, dev, (uint16_t)i, img->data[i])) {
            fprintf(stderr, "error: verify failed at 0x%04zx\n", i);
            exit(1);
        }
        if ((i & 0xfff) == 0xfff) {
            printf("\r0x%04zx", i + 1);
            fflush(stdout);
        }
    }
    printf("\r0x%04zx\n", img->program_size);
    send_rom_command(m, 0xf0);
}

static void usage(const char *argv0) {
    printf("Usage:\n");
    printf("  %s --list\n", argv0);
    printf("  %s --index N --identify\n", argv0);
    printf("  %s --index N --save file.rom [--size 65536]\n", argv0);
    printf("  %s --index N --flash file.rom --yes\n", argv0);
}

int main(int argc, char **argv) {
    bool opt_list = false, opt_identify = false, opt_yes = false;
    const char *save_path = NULL, *flash_path = NULL;
    int index = 0;
    size_t save_size = 65536;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        }
        else if (!strcmp(argv[i], "--list")) opt_list = true;
        else if (!strcmp(argv[i], "--identify")) opt_identify = true;
        else if (!strcmp(argv[i], "--yes")) opt_yes = true;
        else if (!strcmp(argv[i], "--index") && i + 1 < argc) index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_path = argv[++i];
        else if (!strcmp(argv[i], "--flash") && i + 1 < argc) flash_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) save_size = (size_t)strtoul(argv[++i], NULL, 0);
        else {
            usage(argv[0]);
            return 1;
        }
    }

    struct pci_dev *devices = NULL;
    int n = scan_devices(&devices);
    if (opt_list) {
        for (int i = 0; i < n; i++) print_device(i, &devices[i]);
        free(devices);
        return n ? 0 : 1;
    }
    if (index < 0 || index >= n) {
        fprintf(stderr, "error: board index %d not found; use --list\n", index);
        free(devices);
        return 1;
    }

    if (geteuid() != 0) {
        fprintf(stderr, "error: run as root\n");
        free(devices);
        return 1;
    }

    struct mapped_dev m;
    map_device(&devices[index], &m);
    print_device(index, &devices[index]);

    begin_rom(&m);
    uint8_t man = 0, chip = 0;
    unsigned sector = 1;
    get_rom_id(&m, &man, &chip, &sector);
    printf("Flash ID: man=%02X dev=%02X (%s), sector/program chunk=%u byte(s)\n",
           man, chip, flash_name(man, chip), sector);

    if (opt_identify) {
        end_rom(&m);
        unmap_device(&m);
        free(devices);
        return 0;
    }

    if (save_path) {
        printf("Saving %zu bytes to %s...\n", save_size, save_path);
        save_rom(&m, save_path, save_size);
    }

    if (flash_path) {
        if (!opt_yes) {
            fprintf(stderr, "error: refusing to flash without --yes\n");
            end_rom(&m);
            unmap_device(&m);
            free(devices);
            return 1;
        }
        struct image img = load_image(flash_path);
        printf("Loaded %s: file=%zu bytes, programmed=%zu bytes\n",
               flash_path, img.file_size, img.program_size);
        flash_image(&m, &img, man, chip, sector);
        free(img.data);
    }

    end_rom(&m);
    unmap_device(&m);
    free(devices);
    return 0;
}
