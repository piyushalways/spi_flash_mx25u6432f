#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/devicetree.h>

#include <string.h>

#include <zephyr/drivers/gpio.h> //pj_change
static const struct device *gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static const struct device *gpio2_dev = DEVICE_DT_GET(DT_NODELABEL(gpio2)); //pj_change
/* GPIO pins (as provided earlier) */ //pj_change
#define GPIO_PIN_8   8   /* optional power or control pin - kept for parity */
#define GPIO_PIN_15  15
#define GPIO_PIN_10 10


/* ===================== Flash Configuration ===================== */
#define FLASH_START_ADDR     0x000000u
#define TOTAL_SIZE_BYTES     (500u * 1024u)
#define SECTOR_SIZE          4096u
#define PAGE_SIZE            256u
#define WORD_SIZE            4u
#define TOTAL_WORDS          (TOTAL_SIZE_BYTES / WORD_SIZE)

/* ===================== Globals ===================== */
static const struct device *flash_dev;
static uint8_t sector_buf[SECTOR_SIZE];
static uint8_t page_buf[PAGE_SIZE];

/* ===================== Write 500KB pattern ===================== */
static int write_pattern(void)
{
    printk("=== WRITE PHASE ===\n");
    printk("Erasing %u bytes...\n", (unsigned)TOTAL_SIZE_BYTES);

    /* Erase all flash */
    for (uint32_t off = 0; off < TOTAL_SIZE_BYTES; off += SECTOR_SIZE) {
        if (flash_erase(flash_dev, FLASH_START_ADDR + off, SECTOR_SIZE) != 0) {
            printk("Erase failed at 0x%08X\n", (unsigned)off);
            return -1;
        }
        if ((off % (64u * 1024u)) == 0 && off > 0) {
            printk("  Erased: %u / %u bytes\n", (unsigned)off, (unsigned)TOTAL_SIZE_BYTES);
        }
    }
    printk("Erase complete\n");

    printk("Writing %u word pattern...\n", (unsigned)TOTAL_WORDS);

    /* Write consecutive uint32 pattern */
    for (uint32_t off = 0; off < TOTAL_SIZE_BYTES; off += PAGE_SIZE) {
        uint32_t first_word = off / WORD_SIZE;
        for (uint32_t i = 0; i < PAGE_SIZE; i += 4) {
            uint32_t val = first_word + (i / 4);
            sys_put_le32(val, &page_buf[i]);
        }
        if (flash_write(flash_dev, FLASH_START_ADDR + off, page_buf, PAGE_SIZE) != 0) {
            printk("Write failed at 0x%08X\n", (unsigned)off);
            return -1;
        }
        if ((off % (64u * 1024u)) == 0 && off > 0) {
            printk("  Written: %u / %u bytes\n", (unsigned)off, (unsigned)TOTAL_SIZE_BYTES);
        }
    }
    printk("Write complete\n\n");
    return 0;
}

/* ===================== Quick verify (sample check only) ===================== */
static int verify_pattern(void)
{
    printk("=== VERIFY PHASE ===\n");
    
    /* Check first sector */
    printk("Checking first sector...\n");
    if (flash_read(flash_dev, FLASH_START_ADDR, sector_buf, SECTOR_SIZE) != 0) {
        printk("First sector read failed\n");
        return -1;
    }
    for (uint32_t i = 0; i < 16; i += 4) {
        uint32_t val = sys_le32_to_cpu(*(uint32_t*)&sector_buf[i]);
        uint32_t expected = i / 4;
        if (val != expected) {
            printk("First sector mismatch at word %u: got 0x%08X, expected 0x%08X\n", 
                   (unsigned)(i/4), (unsigned)val, (unsigned)expected);
            return -1;
        }
    }
    printk("First sector OK\n");

    /* Check middle sector */
    printk("Checking middle sector...\n");
    uint32_t mid_off = TOTAL_SIZE_BYTES / 2;
    if (flash_read(flash_dev, FLASH_START_ADDR + mid_off, sector_buf, SECTOR_SIZE) != 0) {
        printk("Middle sector read failed\n");
        return -1;
    }
    uint32_t expected_first = (mid_off / WORD_SIZE);
    uint32_t val = sys_le32_to_cpu(*(uint32_t*)&sector_buf[0]);
    if (val != expected_first) {
        printk("Middle sector mismatch: got 0x%08X, expected 0x%08X\n", 
               (unsigned)val, (unsigned)expected_first);
        return -1;
    }
    printk("Middle sector OK\n");

    /* Check last sector */
    printk("Checking last sector...\n");
    uint32_t last_off = TOTAL_SIZE_BYTES - SECTOR_SIZE;
    if (flash_read(flash_dev, FLASH_START_ADDR + last_off, sector_buf, SECTOR_SIZE) != 0) {
        printk("Last sector read failed\n");
        return -1;
    }
    expected_first = (last_off / WORD_SIZE);
    val = sys_le32_to_cpu(*(uint32_t*)&sector_buf[0]);
    if (val != expected_first) {
        printk("Last sector mismatch: got 0x%08X, expected 0x%08X\n", 
               (unsigned)val, (unsigned)expected_first);
        return -1;
    }
    printk("Last sector OK\n");
    printk("All checks passed\n\n");

    return 0;
}

/* ===================== Erase flash ===================== */
static int erase_flash(void)
{
    printk("=== ERASE PHASE ===\n");
    printk("Erasing %u bytes...\n", (unsigned)TOTAL_SIZE_BYTES);

    for (uint32_t off = 0; off < TOTAL_SIZE_BYTES; off += SECTOR_SIZE) {
        if (flash_erase(flash_dev, FLASH_START_ADDR + off, SECTOR_SIZE) != 0) {
            printk("Erase failed at 0x%08X\n", (unsigned)off);
            return -1;
        }
        if ((off % (64u * 1024u)) == 0 && off > 0) {
            printk("  Erased: %u / %u bytes\n", (unsigned)off, (unsigned)TOTAL_SIZE_BYTES);
        }
    }
    printk("Erase complete\n\n");
    return 0;
}

/* ===================== Main ===================== */
int main(void)
{
    printk("\n========================================\n");
    printk("Flash Power Test - 500KB Write/Read/Erase\n");
    printk("========================================\n\n");

    if (device_is_ready(gpio1_dev) && device_is_ready(gpio2_dev)) { //pj_change
        int rc;
        rc = gpio_pin_configure(gpio1_dev, GPIO_PIN_15, GPIO_OUTPUT_ACTIVE);
        if (rc == 0) { gpio_pin_set(gpio1_dev, GPIO_PIN_15, 1); printk("GPIO 1.15 HIGH AVDD\n"); }
        rc = gpio_pin_configure(gpio1_dev, GPIO_PIN_8, GPIO_OUTPUT_INACTIVE);
        if (rc == 0) { gpio_pin_set(gpio1_dev, GPIO_PIN_8, 0); printk("GPIO 1.08 LOW EN_ACCEL\n"); }
        rc = gpio_pin_configure(gpio2_dev, GPIO_PIN_10, GPIO_OUTPUT_INACTIVE);
        if (rc == 0) { gpio_pin_set(gpio2_dev, GPIO_PIN_10, 0); printk("GPIO 2.10 LOW EN_ASVDD\n"); }
    } else {
        printk("GPIO device not ready (optional).");
    }


    flash_dev = DEVICE_DT_GET(DT_NODELABEL(mx25u64));
    if (!device_is_ready(flash_dev)) {
        printk("ERROR: Flash device not ready\n");
        return -1;
    }
    printk("Flash device ready: %s\n\n", flash_dev->name);

    /* Initial idle period for baseline measurement */
    printk("Waiting 2 seconds (baseline measurement)...\n");
    k_msleep(2000);
    printk("Starting operations\n\n");

    /* Write test */
    if (write_pattern() != 0) {
        printk("ERROR: Write operation failed\n");
        return -1;
    }

    /* Verify test */
    if (verify_pattern() != 0) {
        printk("ERROR: Verification failed\n");
        return -1;
    }

    /* Erase test */
    if (erase_flash() != 0) {
        printk("ERROR: Erase operation failed\n");
        return -1;
    }

    /* Sleep */
    printk("========================================\n");
    printk("All tests completed successfully\n");
    printk("Entering sleep mode (measurement window)\n");
    printk("========================================\n");
    k_sleep(K_FOREVER);

    return 0;
}

