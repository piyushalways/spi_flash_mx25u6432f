#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/devicetree.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include <string.h>

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

/* BLE Globals */
static struct bt_conn *current_conn;
static volatile bool notify_enabled;
static volatile bool test_start_requested;
static struct k_sem test_sem;

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
    printk("  Offset: 0x%08X\n", (unsigned)(FLASH_START_ADDR + last_off));
    
    if (flash_read(flash_dev, FLASH_START_ADDR + last_off, sector_buf, SECTOR_SIZE) != 0) {
        printk("Last sector read failed\n");
        return -1;
    }
    
    expected_first = (last_off / WORD_SIZE);
    val = sys_le32_to_cpu(*(uint32_t*)&sector_buf[0]);
    
    printk("  Expected: 0x%08X, Got: 0x%08X\n", (unsigned)expected_first, (unsigned)val);
    
    /* Debug: print first few words */
    for (int i = 0; i < 4; i++) {
        uint32_t w = sys_le32_to_cpu(*(uint32_t*)&sector_buf[i*4]);
        printk("    [%d]: 0x%08X\n", i, (unsigned)w);
    }
    
    if (val != expected_first) {
        printk("Last sector MISMATCH\n");
        return -1;
    }
    printk("Last sector OK\n");
    printk("All checks passed\n\n");

    return 0;
}

/* ===================== BLE Callbacks ===================== */
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) return;
    if (current_conn) {
        bt_conn_unref(current_conn);
    }
    current_conn = bt_conn_ref(conn);
    printk("BLE connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    notify_enabled = false;
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    printk("BLE disconnected\n");
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* CCC callback */
static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Notify %s\n", notify_enabled ? "enabled" : "disabled");
}

/* Control write callback - receive 0x01 to start test */
static ssize_t ctrl_write(struct bt_conn *conn,
                          const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len,
                          uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0 || len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *cmd = (const uint8_t *)buf;
    if (cmd[0] == 0x01) {
        printk("Start command received\n");
        test_start_requested = true;
        k_sem_give(&test_sem);
    }
    return len;
}

/* GATT Service Definition */
BT_GATT_SERVICE_DEFINE(power_test_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(
        0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78,
        0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78)),

    /* Status notify characteristic */
    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                           0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11),
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        NULL, NULL, NULL),
    BT_GATT_CCC(ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* Control write characteristic */
    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
                           0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22),
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL, ctrl_write, NULL)
);

#define STATUS_NOTIFY_ATTR (&power_test_svc.attrs[1])

/* Send status notification */
static int send_status(const char *status_msg)
{
    if (!current_conn || !notify_enabled) {
        return -ENOTCONN;
    }

    struct bt_gatt_notify_params params = {
        .attr = STATUS_NOTIFY_ATTR,
        .data = (const uint8_t *)status_msg,
        .len = strlen(status_msg),
    };

    return bt_gatt_notify_cb(current_conn, &params);
}

/* BLE advertising data */
static const struct bt_data ad[] = {
    BT_DATA(BT_DATA_FLAGS, &(uint8_t){BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR}, 1),
    BT_DATA(BT_DATA_NAME_COMPLETE, (const uint8_t *)CONFIG_BT_DEVICE_NAME, strlen(CONFIG_BT_DEVICE_NAME)),
};

/* BLE init */
static int ble_init(void)
{
    int err = bt_enable(NULL);
    if (err) {
        printk("bt_enable failed (%d)\n", err);
        return err;
    }

    err = bt_le_adv_start(
        BT_LE_ADV_CONN,
        ad, ARRAY_SIZE(ad),
        NULL, 0);

    if (err && err != -EALREADY) {
        printk("bt_le_adv_start failed (%d)\n", err);
        return err;
    }

    printk("BLE advertising started\n");
    return 0;
}

/* ===================== Main ===================== */
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
    printk("Flash Power Test - BLE Controlled\n");
    printk("========================================\n\n");

    k_sem_init(&test_sem, 0, 1);

    /* Initialize BLE first */
    if (ble_init() != 0) {
        printk("ERROR: BLE init failed\n");
        return -1;
    }

    /* Check flash device */
    flash_dev = DEVICE_DT_GET(DT_NODELABEL(mx25u64));
    if (!device_is_ready(flash_dev)) {
        printk("ERROR: Flash device not ready\n");
        /* Wait for BLE connection and command */
        printk("Waiting for BLE command...\n");
        k_sem_take(&test_sem, K_FOREVER);
        send_status("flash device not detected");
        printk("Notified: Flash device not detected\n");
        return -1;
    }
    printk("Flash device ready: %s\n\n", flash_dev->name);

    /* Wait for test start command via BLE */
    printk("Waiting for BLE command (0x01) to start test...\n");
    k_sem_take(&test_sem, K_FOREVER);

    if (!test_start_requested) {
        printk("Test start failed\n");
        return -1;
    }

    printk("\nTest starting...\n\n");

    /* Erase test */
    printk("=== ERASE PHASE ===\n");
    if (erase_flash() != 0) {
        printk("ERROR: Erase operation failed\n");
        send_status("erase failed");
        return -1;
    }
    send_status("erase done");
    printk("Notified: Erase done\n\n");

    /* Write test */
    printk("=== WRITE PHASE ===\n");
    if (write_pattern() != 0) {
        printk("ERROR: Write operation failed\n");
        send_status("write failed");
        return -1;
    }
    send_status("write done");
    printk("Notified: Write done\n\n");

    /* Read/Verify test */
    printk("=== READ PHASE ===\n");
    if (verify_pattern() != 0) {
        printk("ERROR: Read verification failed\n");
        send_status("read failed");
        return -1;
    }
    send_status("read done");
    printk("Notified: Read done\n\n");

    /* Test complete */
    printk("========================================\n");
    printk("All tests completed successfully\n");
    printk("========================================\n");
    send_status("test done");
    printk("Notified: Test complete\n");

    return 0;
}

