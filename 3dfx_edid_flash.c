#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TDFX_VENDOR_ID 0x121a
#define DEV_BANSHEE   0x0003
#define DEV_AVENGER1  0x0004
#define DEV_AVENGER2  0x0005
#define DEV_NAPALM    0x0009
#define DEV_NAPALM2   0x000b
#define DEV_RAMPAGE   0x0010

/* vidSerialParallelPort (BAR0 + 0x78): the chip's DDC/I2C + GPIO control.
 * SCL/SDA are open-drain: write 0 to drive the line low, 1 to release it. */
#define VIDSERIALPARALLELPORT 0x78
#define VSP_ENABLE_IIC0 (1u << 18)   /* DDC port enable                      */
#define VSP_SCL0_OUT    (1u << 19)
#define VSP_SDA0_OUT    (1u << 20)
#define VSP_SCL0_IN     (1u << 21)   /* read-only line state                 */
#define VSP_SDA0_IN     (1u << 22)
#define VSP_GPIO1_OUT   (1u << 29)   /* board mux select -> digital DDC side */

#define EEPROM_ADDR 0xA0             /* 24LC08B block 0 -- the EDID lives here */
#define EDID_SIZE   256
#define MAP_SIZE    0x1000u

static const uint8_t EDID_HDR[8] = {0,0xff,0xff,0xff,0xff,0xff,0xff,0};

struct pci_dev {
    char bdf[64];
    char path[512];
    uint16_t vendor, device;
    uint64_t bar0, bar0_size;
};

struct mapped_dev {
    struct pci_dev pci;
    int res0_fd;
    volatile uint8_t *mmio;
    uint32_t vsp_orig;     /* original VSP, restored on exit                 */
    uint32_t base;         /* VSP with DDC enabled + mux on the digital side */
};

static void die(const char *msg) {
    fprintf(stderr, "error: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static bool is_supported_3dfx_device(uint16_t d) {
    return d == DEV_BANSHEE || d == DEV_AVENGER1 || d == DEV_AVENGER2 ||
           d == DEV_NAPALM  || d == DEV_NAPALM2  || d == DEV_RAMPAGE;
}

static const char *device_name(uint16_t d) {
    switch (d) {
    case DEV_BANSHEE:  return "Banshee";
    case DEV_AVENGER1:
    case DEV_AVENGER2: return "Avenger/Voodoo3";
    case DEV_NAPALM:   return "VSA-100/Napalm";
    case DEV_NAPALM2:  return "Napalm2";
    case DEV_RAMPAGE:  return "Rampage";
    default:           return "unknown";
    }
}

static void path_join(char *out, size_t n, const char *a, const char *b) {
    snprintf(out, n, "%s/%s", a, b);
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

static bool read_resource0(struct pci_dev *dev) {
    char p[640];
    path_join(p, sizeof(p), dev->path, "resource");
    FILE *f = fopen(p, "r");
    if (!f) return false;
    unsigned long long start = 0, end = 0, flags = 0;
    if (fscanf(f, "%llx %llx %llx", &start, &end, &flags) != 3) { fclose(f); return false; }
    fclose(f);
    dev->bar0 = start;
    dev->bar0_size = (end >= start) ? (end - start + 1) : 0;
    return dev->bar0_size >= MAP_SIZE;
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
        size_t bl = strlen(de->d_name);
        if (bl >= sizeof(dev.bdf)) bl = sizeof(dev.bdf) - 1;
        memcpy(dev.bdf, de->d_name, bl);
        dev.bdf[bl] = 0;
        snprintf(dev.path, sizeof(dev.path), "/sys/bus/pci/devices/%s", de->d_name);

        char p[640];
        path_join(p, sizeof(p), dev.path, "vendor");
        dev.vendor = read_hex16_file(p);
        if (dev.vendor != TDFX_VENDOR_ID) continue;

        path_join(p, sizeof(p), dev.path, "device");
        dev.device = read_hex16_file(p);
        if (!is_supported_3dfx_device(dev.device)) continue;
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
    printf("%d: %s %s VEN_%04X DEV_%04X BAR0=0x%llx size=0x%llx\n",
           idx, d->bdf, device_name(d->device), d->vendor, d->device,
           (unsigned long long)d->bar0, (unsigned long long)d->bar0_size);
}

/* ---------------- raw register access ---------------- */

static uint32_t vsp_rd(struct mapped_dev *m) {
    return *(volatile uint32_t *)(m->mmio + VIDSERIALPARALLELPORT);
}
static void vsp_wr(struct mapped_dev *m, uint32_t v) {
    *(volatile uint32_t *)(m->mmio + VIDSERIALPARALLELPORT) = v;
    (void)*(volatile uint32_t *)(m->mmio + VIDSERIALPARALLELPORT);  /* flush/order */
}

/* ---------------- bit-banged I2C on the DDC pins ---------------- */

static void idly(void) { struct timespec t = {0, 5000}; nanosleep(&t, NULL); }  /* ~5us */

static void i2c_set(struct mapped_dev *m, int scl, int sda) {
    uint32_t v = m->base;
    v = scl ? (v | VSP_SCL0_OUT) : (v & ~VSP_SCL0_OUT);
    v = sda ? (v | VSP_SDA0_OUT) : (v & ~VSP_SDA0_OUT);
    vsp_wr(m, v);
    idly();
}
static int  i2c_sda(struct mapped_dev *m) { return (vsp_rd(m) & VSP_SDA0_IN) ? 1 : 0; }
static void i2c_start(struct mapped_dev *m){ i2c_set(m,1,1); i2c_set(m,1,0); i2c_set(m,0,0); }
static void i2c_stop(struct mapped_dev *m) { i2c_set(m,0,0); i2c_set(m,1,0); i2c_set(m,1,1); }
static void i2c_wbit(struct mapped_dev *m, int b){ i2c_set(m,0,b); i2c_set(m,1,b); i2c_set(m,0,b); }
static int  i2c_rbit(struct mapped_dev *m){ i2c_set(m,0,1); i2c_set(m,1,1); int b=i2c_sda(m); i2c_set(m,0,1); return b; }
static int  i2c_wbyte(struct mapped_dev *m, int x){ for(int i=7;i>=0;i--) i2c_wbit(m,(x>>i)&1); return i2c_rbit(m); } /* 0=ACK */
static int  i2c_rbyte(struct mapped_dev *m, int ack){ int v=0; for(int i=0;i<8;i++) v=(v<<1)|i2c_rbit(m); i2c_wbit(m, ack?0:1); return v; }

static bool eeprom_acks(struct mapped_dev *m) {
    i2c_start(m);
    bool ack = (i2c_wbyte(m, EEPROM_ADDR) == 0);
    i2c_stop(m);
    return ack;
}
static void eeprom_read(struct mapped_dev *m, uint8_t *buf, int n) {
    i2c_start(m); i2c_wbyte(m, EEPROM_ADDR); i2c_wbyte(m, 0x00);   /* set addr ptr */
    i2c_start(m); i2c_wbyte(m, EEPROM_ADDR | 1);                   /* repeated start, read */
    for (int k = 0; k < n; k++) buf[k] = (uint8_t)i2c_rbyte(m, k < n - 1);
    i2c_stop(m);
}
static bool eeprom_write(struct mapped_dev *m, const uint8_t *buf, int n) {
    for (int a = 0; a < n; a++) {
        i2c_start(m);
        if (i2c_wbyte(m, EEPROM_ADDR) || i2c_wbyte(m, a & 0xFF) || i2c_wbyte(m, buf[a])) {
            i2c_stop(m);
            fprintf(stderr, "error: no ACK writing byte %d (WP asserted? wrong bus?)\n", a);
            return false;
        }
        i2c_stop(m);
        usleep(6000);                                             /* 24LCxx write cycle */
        if ((a & 0x3f) == 0x3f) { printf("\r  %d/%d", a + 1, n); fflush(stdout); }
    }
    printf("\r  %d/%d\n", n, n);
    return true;
}

/* ---------------- device mapping ---------------- */

static void map_device(const struct pci_dev *dev, struct mapped_dev *m) {
    memset(m, 0, sizeof(*m));
    m->pci = *dev;
    m->res0_fd = -1;

    char res_path[640];
    path_join(res_path, sizeof(res_path), dev->path, "resource0");
    m->res0_fd = open(res_path, O_RDWR | O_SYNC);
    if (m->res0_fd < 0) die("open resource0");

    m->mmio = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m->res0_fd, 0);
    if (m->mmio == MAP_FAILED) die("mmap resource0");

    m->vsp_orig = vsp_rd(m);
    m->base = m->vsp_orig | VSP_ENABLE_IIC0 | VSP_GPIO1_OUT;       /* DDC on, mux=digital */
}
static void unmap_device(struct mapped_dev *m) {
    if (m->mmio && m->mmio != MAP_FAILED) {
        vsp_wr(m, m->vsp_orig);
        munmap((void *)m->mmio, MAP_SIZE);
    }
    if (m->res0_fd >= 0) close(m->res0_fd);
}

/* ---------------- helpers ---------------- */

static void edid_summary(const uint8_t *e) {
    if (memcmp(e, EDID_HDR, 8) != 0) { printf("  (no valid EDID header on the bus)\n"); return; }
    uint16_t id = (uint16_t)((e[8] << 8) | e[9]);
    char mfg[4] = { (char)('@' + ((id >> 10) & 0x1f)),
                    (char)('@' + ((id >> 5)  & 0x1f)),
                    (char)('@' +  (id        & 0x1f)), 0 };
    printf("  EDID OK: mfg \"%s\"  product 0x%04X\n", mfg, e[10] | (e[11] << 8));
}

static bool load_file(const char *path, uint8_t *buf, int n) {
    FILE *f = fopen(path, "rb");
    if (!f) die(path);
    size_t rd = fread(buf, 1, n, f);
    fclose(f);
    if (rd != (size_t)n) {
        fprintf(stderr, "error: %s is %zu bytes, expected %d\n", path, rd, n);
        return false;
    }
    return true;
}

static void save_file(const char *path, const uint8_t *buf, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) die(path);
    if (fwrite(buf, 1, n, f) != (size_t)n) die("fwrite");
    fclose(f);
}

static void usage(const char *a0) {
    printf("Usage:\n");
    printf("  %s --list\n", a0);
    printf("  %s --index N --identify\n", a0);
    printf("  %s --index N --save edid.bin\n", a0);
    printf("  %s --index N --flash edid.bin --yes [--force]\n", a0);
    printf("\nFlashes a 256-byte EDID into the board's 24LC08B EEPROM over the\n");
    printf("VSA-100 DDC bus (single bus, selected to the digital side by GPIO_1).\n");
    printf("Stop your display manager first so X is not driving the card.\n");
    printf("--force programs a blank/unprogrammed chip (one with no EDID yet).\n");
}

int main(int argc, char **argv) {
    bool opt_list = false, opt_identify = false, opt_yes = false, opt_force = false;
    const char *save_path = NULL, *flash_path = NULL;
    int index = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(argv[i], "--list"))     opt_list = true;
        else if (!strcmp(argv[i], "--identify")) opt_identify = true;
        else if (!strcmp(argv[i], "--yes"))      opt_yes = true;
        else if (!strcmp(argv[i], "--force"))    opt_force = true;
        else if (!strcmp(argv[i], "--index") && i + 1 < argc) index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--save")  && i + 1 < argc) save_path = argv[++i];
        else if (!strcmp(argv[i], "--flash") && i + 1 < argc) flash_path = argv[++i];
        else { usage(argv[0]); return 1; }
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

    uint8_t cur[EDID_SIZE];
    eeprom_read(&m, cur, EDID_SIZE);
    bool header_ok = (memcmp(cur, EDID_HDR, 8) == 0);
    printf("current EEPROM: first8 %02x%02x%02x%02x%02x%02x%02x%02x\n",
           cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7]);
    edid_summary(cur);

    int rc = 0;
    if (opt_identify) {
        rc = header_ok ? 0 : 1;
        goto out;
    }
    if (save_path) {
        save_file(save_path, cur, EDID_SIZE);
        printf("saved current EDID -> %s\n", save_path);
    }
    if (flash_path) {
        if (!opt_yes) { fprintf(stderr, "error: refusing to flash without --yes\n"); rc = 1; goto out; }

        uint8_t img[EDID_SIZE];
        if (!load_file(flash_path, img, EDID_SIZE)) { rc = 1; goto out; }
        if (memcmp(img, EDID_HDR, 8) != 0) {
            fprintf(stderr, "error: %s does not start with an EDID header (00 FF FF ...)\n", flash_path);
            rc = 1; goto out;
        }

        /* Bus must prove itself: a valid existing EDID, or (for a blank chip,
         * with --force) an I2C ACK so we never write into a wrong bus/mux. */
        if (!header_ok) {
            if (!eeprom_acks(&m)) {
                fprintf(stderr, "error: nothing ACKs at 0x%02X -> wrong board/mux or no EEPROM, NOT writing\n", EEPROM_ADDR);
                rc = 1; goto out;
            }
            if (!opt_force) {
                fprintf(stderr, "error: EEPROM responds but holds no EDID (looks blank). "
                                "Re-run with --force to program a fresh chip.\n");
                rc = 1; goto out;
            }
            printf("EEPROM present but blank/unprogrammed -> proceeding (--force)\n");
        }

        save_file("edid_backup.bin", cur, EDID_SIZE);
        printf("backed up current EEPROM -> edid_backup.bin\n");

        if (memcmp(cur, img, EDID_SIZE) == 0) {
            printf("EEPROM already matches %s. Nothing to do.\n", flash_path);
            goto out;
        }
        printf("writing %d bytes...\n", EDID_SIZE);
        if (!eeprom_write(&m, img, EDID_SIZE)) { rc = 1; goto out; }

        uint8_t back[EDID_SIZE];
        eeprom_read(&m, back, EDID_SIZE);
        if (memcmp(back, img, EDID_SIZE) == 0) {
            printf("VERIFY OK -- EEPROM now matches %s.\n", flash_path);
        } else {
            int j = 0;
            while (j < EDID_SIZE && back[j] == img[j]) j++;
            fprintf(stderr, "error: VERIFY FAILED at byte %d (got %02x want %02x)\n", j, back[j], img[j]);
            rc = 1;
        }
    }

out:
    unmap_device(&m);
    free(devices);
    return rc;
}
