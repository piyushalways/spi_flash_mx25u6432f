

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <zephyr/devicetree.h>
#include <zephyr/devicetree/fixed-partitions.h>

#include <errno.h>
#include <string.h>

/* ===================== External flash dataset ===================== */
#define FLASH_START_ADDR     0x000000u
#define TOTAL_SIZE_BYTES     (500u * 1024u)   /* 512000 bytes */
#define SECTOR_SIZE          4096u
#define PAGE_SIZE            256u

#define WORD_SIZE            4u
#define TOTAL_WORDS          (TOTAL_SIZE_BYTES / WORD_SIZE) /* 128000 */

/* ===================== Globals ===================== */
static const struct device *flash_dev;

static uint8_t sector_buf[SECTOR_SIZE];

static uint32_t dataset_crc32;

/* progress */
static volatile uint32_t confirmed_offset;

/* ===================== Optional NVS persistence ===================== */
#if IS_ENABLED(CONFIG_NVS) && DT_NODE_EXISTS(DT_CHOSEN(zephyr_storage))
#include <zephyr/fs/nvs.h>
static struct nvs_fs nvs;
static bool nvs_ok;
#define NVS_ID_OFFSET  1

static int sync_nvs_init(void)
{
    const struct device *fdev =
        DEVICE_DT_GET(DT_MTD_FROM_FIXED_PARTITION(DT_CHOSEN(zephyr_storage)));

    if (!device_is_ready(fdev)) {
        printk("NVS: storage flash device not ready\n");
        return -ENODEV;
    }

    nvs.flash_device = fdev;
    nvs.offset = DT_FIXED_PARTITION_OFFSET(DT_CHOSEN(zephyr_storage));

    struct flash_pages_info info;
    int err = flash_get_page_info_by_offs(nvs.flash_device, nvs.offset, &info);
    if (err) {
        printk("NVS: page_info failed (%d)\n", err);
        return err;
    }

    nvs.sector_size = info.size;
    nvs.sector_count = 3;

    err = nvs_mount(&nvs);
    if (err) {
        printk("NVS: mount failed (%d)\n", err);
        return err;
    }

    nvs_ok = true;
    printk("NVS: mounted\n");
    return 0;
}

static void sync_nvs_load_offset(void)
{
    if (!nvs_ok) return;

    uint32_t off = 0;
    ssize_t rc = nvs_read(&nvs, NVS_ID_OFFSET, &off, sizeof(off));
    if (rc == (ssize_t)sizeof(off) && off <= TOTAL_SIZE_BYTES) {
        confirmed_offset = off & ~0x3u;
        printk("NVS: loaded confirmed_offset=%u\n", (unsigned)confirmed_offset);
    }
}

static void sync_nvs_store_offset(uint32_t off)
{
    if (!nvs_ok) return;
    off &= ~0x3u;
    (void)nvs_write(&nvs, NVS_ID_OFFSET, &off, sizeof(off));
}
#else
static int  sync_nvs_init(void) { return 0; }
static void sync_nvs_load_offset(void) {}
static void sync_nvs_store_offset(uint32_t off) { ARG_UNUSED(off); }
#endif

/* ===================== Pattern generation: consecutive uint32 ===================== */
static bool pattern_present(void)
{
    uint32_t head[4];
    uint32_t tail[4];

    if (flash_read(flash_dev, FLASH_START_ADDR, head, sizeof(head)) != 0) return false;

    uint32_t tail_addr = FLASH_START_ADDR + TOTAL_SIZE_BYTES - sizeof(tail);
    if (flash_read(flash_dev, tail_addr, tail, sizeof(tail)) != 0) return false;

    for (uint32_t i = 0; i < 4; i++) {
        if (sys_le32_to_cpu(head[i]) != i) return false;
    }

    uint32_t base = TOTAL_WORDS - 4;
    for (uint32_t i = 0; i < 4; i++) {
        if (sys_le32_to_cpu(tail[i]) != (base + i)) return false;
    }

    return true;
}

static int write_consecutive_u32_pattern(void)
{
    printk("Erasing %u bytes...\n", (unsigned)TOTAL_SIZE_BYTES);

    for (uint32_t off = 0; off < TOTAL_SIZE_BYTES; off += SECTOR_SIZE) {
        int err = flash_erase(flash_dev, FLASH_START_ADDR + off, SECTOR_SIZE);
        if (err) {
            printk("Erase failed at 0x%08X (err=%d)\n", FLASH_START_ADDR + off, err);
            return err;
        }
    }

    printk("Writing consecutive uint32 pattern (%u words)...\n", (unsigned)TOTAL_WORDS);

    uint8_t page_buf[PAGE_SIZE];

    for (uint32_t off = 0; off < TOTAL_SIZE_BYTES; off += PAGE_SIZE) {
        uint32_t first_word = off / WORD_SIZE;

        for (uint32_t i = 0; i < PAGE_SIZE; i += 4) {
            uint32_t val = first_word + (i / 4);
            sys_put_le32(val, &page_buf[i]);
        }

        int err = flash_write(flash_dev, FLASH_START_ADDR + off, page_buf, PAGE_SIZE);
        if (err) {
            printk("Write failed at 0x%08X (err=%d)\n", FLASH_START_ADDR + off, err);
            return err;
        }
    }

    printk("Pattern written OK.\n");
    return 0;
}

static uint32_t compute_dataset_crc32(void)
{
    uint32_t crc = 0;
    for (uint32_t off = 0; off < TOTAL_SIZE_BYTES; off += SECTOR_SIZE) {
        uint32_t len = MIN(SECTOR_SIZE, (TOTAL_SIZE_BYTES - off));
        if (flash_read(flash_dev, FLASH_START_ADDR + off, sector_buf, len) != 0) {
            printk("CRC read failed at off=%u\n", (unsigned)off);
            return 0;
        }
        crc = crc32_ieee_update(crc, sector_buf, len);
    }
    return crc;
}



/* ===================== Main ===================== */
int main(void)
{
    printk("FlashSync v2 (MTU-agnostic, CRC, resume, optional ACK)\n");

    /* Wait 2 seconds before starting test */
    printk("Waiting 2 seconds...\n");
    k_msleep(2000);
    printk("Starting test...\n\n");

    flash_dev = DEVICE_DT_GET(DT_NODELABEL(mx25u64));
    if (!device_is_ready(flash_dev)) {
        printk("External flash not ready\n");
        return 0;
    }

    (void)sync_nvs_init();
    sync_nvs_load_offset();

    if (!pattern_present()) {
        printk("Pattern not found. Creating 500KB consecutive uint32 pattern...\n");
        if (write_consecutive_u32_pattern() != 0) {
            printk("Pattern write failed\n");
            return 0;
        }
    } else {
        printk("Pattern already present\n");
    }

    dataset_crc32 = compute_dataset_crc32();
    printk("Dataset CRC32 = 0x%08X\n", (unsigned)dataset_crc32);

    /* Print all flash data to UART */
    printk("\n=== Reading and Printing Flash Data ===\n");
    
    uint32_t off = 0;
    while (off < TOTAL_SIZE_BYTES) {
        uint32_t sector_base = (off / SECTOR_SIZE) * SECTOR_SIZE;
        uint32_t in_sector = off - sector_base;

        int err = flash_read(flash_dev, FLASH_START_ADDR + sector_base, sector_buf, SECTOR_SIZE);
        if (err) {
            printk("Read failed at sector 0x%08X (err=%d)\n", FLASH_START_ADDR + sector_base, err);
            break;
        }

        while (in_sector < SECTOR_SIZE && off < TOTAL_SIZE_BYTES) {
            uint32_t remain = TOTAL_SIZE_BYTES - off;
            uint16_t chunk = (uint16_t)MIN((uint32_t)256, remain);  /* Print 256 bytes at a time */
            
            /* Print the chunk */
            printk("Offset 0x%08X (%u bytes):\n", (unsigned)off, (unsigned)chunk);
            for (uint16_t i = 0; i < chunk; i += 16) {
                printk("  0x%08X: ", (unsigned)(off + i));
                for (uint16_t j = 0; j < 16 && (i + j) < chunk; j += 4) {
                    uint32_t val = sys_le32_to_cpu(*(uint32_t*)&sector_buf[in_sector + i + j]);
                    printk("%08X ", (unsigned)val);
                }
                printk("\n");
            }
            
            off += chunk;
            in_sector += chunk;

            if ((off % (16u * 1024u)) == 0) {
                printk("Progress: %u / %u\n", (unsigned)off, (unsigned)TOTAL_SIZE_BYTES);
            }
        }
    }

    printk("=== Flash Data Print Complete ===\n");

    /* Erase the flash */
    printk("\n=== Erasing Flash ===\n");
    for (uint32_t off = 0; off < TOTAL_SIZE_BYTES; off += SECTOR_SIZE) {
        int err = flash_erase(flash_dev, FLASH_START_ADDR + off, SECTOR_SIZE);
        if (err) {
            printk("Erase failed at 0x%08X (err=%d)\n", FLASH_START_ADDR + off, err);
            return 0;
        }
        
        if ((off % (16u * 1024u)) == 0) {
            printk("Erase Progress: %u / %u\n", (unsigned)off, (unsigned)TOTAL_SIZE_BYTES);
        }
    }
    printk("=== Flash Erase Complete ===\n");

    /* Go to sleep */
    printk("\nEntering sleep mode...\n");
    k_sleep(K_FOREVER);

    return 0;
}