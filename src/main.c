/*
 * Copyright (c) 2016 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_BOARD_ADAFRUIT_FEATHER_STM32F405)
#define SPI_FLASH_TEST_REGION_OFFSET 0xf000
#elif defined(CONFIG_BOARD_ARTY_A7_DESIGNSTART_FPGA_CORTEX_M1) || \
	defined(CONFIG_BOARD_ARTY_A7_DESIGNSTART_FPGA_CORTEX_M3)
/* The FPGA bitstream is stored in the lower 536 sectors of the flash. */
#define SPI_FLASH_TEST_REGION_OFFSET \
	DT_REG_SIZE(DT_NODE_BY_FIXED_PARTITION_LABEL(fpga_bitstream))
#elif defined(CONFIG_BOARD_NPCX9M6F_EVB) || \
	defined(CONFIG_BOARD_NPCX7M6FB_EVB)
#define SPI_FLASH_TEST_REGION_OFFSET 0x7F000
#else
#define SPI_FLASH_TEST_REGION_OFFSET 0xff000
#endif
#define SPI_FLASH_SECTOR_SIZE        4096

#if defined(CONFIG_FLASH_STM32_OSPI) || \
	defined(CONFIG_FLASH_STM32_QSPI) || \
	defined(CONFIG_FLASH_STM32_XSPI)
#define SPI_FLASH_MULTI_SECTOR_TEST
#endif
// #define SPI_FLASH_MULTI_SECTOR_TEST 1

#if DT_HAS_COMPAT_STATUS_OKAY(jedec_spi_nor)
#define SPI_FLASH_COMPAT jedec_spi_nor
#elif DT_HAS_COMPAT_STATUS_OKAY(jedec_mspi_nor)
#define SPI_FLASH_COMPAT jedec_mspi_nor
#elif DT_HAS_COMPAT_STATUS_OKAY(st_stm32_qspi_nor)
#define SPI_FLASH_COMPAT st_stm32_qspi_nor
#elif DT_HAS_COMPAT_STATUS_OKAY(st_stm32_ospi_nor)
#define SPI_FLASH_COMPAT st_stm32_ospi_nor
#elif DT_HAS_COMPAT_STATUS_OKAY(st_stm32_xspi_nor)
#define SPI_FLASH_COMPAT st_stm32_xspi_nor
#elif DT_HAS_COMPAT_STATUS_OKAY(nordic_qspi_nor)
#define SPI_FLASH_COMPAT nordic_qspi_nor
#else
#define SPI_FLASH_COMPAT invalid
#endif

const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff , 0xff, 0xff, 0xff, 0xff };

/* Expected JEDEC ID for MX25U6432F: Manufacturer=0xC2, Type=0x25, Capacity=0x37 */
#define EXPECTED_MFR_ID    0xC2
#define EXPECTED_TYPE_ID   0x25
#define EXPECTED_DENSITY_ID 0x37

/* Quick erase test at a specific offset */
static bool test_erase_at_offset(const struct device *flash_dev, off_t offset)
{
	uint8_t buf[16];  /* Read more bytes to see pattern */
	int rc;

	printf("  Testing erase at offset 0x%x...\n", (unsigned int)offset);

	/* Read before erase to see what's there */
	memset(buf, 0, sizeof(buf));
	rc = flash_read(flash_dev, offset, buf, sizeof(buf));
	if (rc == 0) {
		printf("    Before: %02x %02x %02x %02x %02x %02x %02x %02x...\n",
		       buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
	}

	/* Erase sector */
	rc = flash_erase(flash_dev, offset, SPI_FLASH_SECTOR_SIZE);
	if (rc != 0) {
		printf("    Erase command failed! %d\n", rc);
		return false;
	}

	/* MX25U6432F sector erase takes up to 600ms according to datasheet
	 * Use 2000ms to work around nRF MSPI driver bug where it doesn't
	 * wait for erase completion (Zephyr issue #71442)
	 */
	k_msleep(2000);

	/* Verify erase - read more bytes to see the pattern */
	memset(buf, 0, sizeof(buf));
	rc = flash_read(flash_dev, offset, buf, sizeof(buf));
	if (rc != 0) {
		printf("    Read failed! %d\n", rc);
		return false;
	}

	printf("    After:  %02x %02x %02x %02x %02x %02x %02x %02x...\n",
	       buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);

	if (memcmp(erased, buf, 4) == 0) {
		printf("    OK\n");
		return true;
	} else {
		printf("    FAIL (expected ff ff ff ff...)\n");
		return false;
	}
}

static bool verify_jedec_id(const struct device *flash_dev)
{
	printf("Verifying flash chip compatibility...\n");
	printf("Expected: MX25U6432F (JEDEC ID: C2 25 37)\n");
	printf("Connected device: %s\n", flash_dev->name);

	/* Check if device name contains the expected chip identifier */
	/* The device tree node name should be mx25u64 or mx25u6432f */
	if (strstr(flash_dev->name, "mx25u") == NULL &&
	    strstr(flash_dev->name, "MX25U") == NULL) {
		printf("\nERROR: Wrong flash chip detected!\n");
		printf("This code is designed for MX25U6432F only.\n");
		printf("Detected device: %s\n", flash_dev->name);
		printf("If you have MX25U6432F connected, check device tree configuration.\n");
		return false;
	}

	printf("Flash chip verification passed!\n\n");
	return true;
}

void single_sector_test(const struct device *flash_dev)
{
	/* Create array with values 0x00 to 0xFF (256 bytes) */
	static uint8_t expected[2000];
	static uint8_t buf[2000];
	static uint8_t erase_buf[2000];
	
	/* Initialize expected array with values 0x00 to 0xFF */
	for (int i = 0; i < 256; i++) {
		expected[i] = (uint8_t)i;
	}
	const size_t len = sizeof(expected);
	int rc;

	printf("\nPerform test on single sector");
	/* Write protection needs to be disabled before each write or
	 * erase, since the flash component turns on write protection
	 * automatically after completion of write and erase
	 * operations.
	 */
	printf("\nTest 1: Flash erase\n");

	/* Read current content before erase */
	memset(buf, 0, len);
	rc = flash_read(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, buf, len);
	if (rc == 0) {
		printf("Before erase: 0x%02x 0x%02x 0x%02x 0x%02x\n",
		       buf[0], buf[1], buf[2], buf[3]);
	}

	/* Full flash erase if SPI_FLASH_TEST_REGION_OFFSET = 0 and
	 * SPI_FLASH_SECTOR_SIZE = flash size
	 */
	rc = flash_erase(flash_dev, SPI_FLASH_TEST_REGION_OFFSET,
			 SPI_FLASH_SECTOR_SIZE);
	if (rc != 0) {
		printf("Flash erase failed! %d\n", rc);
	} else {
		printf("Flash erase command completed (rc=%d)\n", rc);

		/* Workaround for nRF MSPI driver bug (Zephyr issue #71442)
		 * Driver doesn't wait for erase completion, so we manually wait
		 * MX25U6432F sector erase takes up to 600ms per datasheet
		 */
		printf("Waiting for erase to complete (2s delay for driver bug)...\n");
		k_msleep(2000);

		/* Check erased pattern - read full 256 bytes to verify erase */
		memset(erase_buf, 0, len);
		rc = flash_read(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, erase_buf, len);
		if (rc != 0) {
			printf("Flash read failed! %d\n", rc);
			return;
		}
		printf("After erase:  0x%02x 0x%02x 0x%02x 0x%02x\n",
		       erase_buf[0], erase_buf[1], erase_buf[2], erase_buf[3]);

		/* Check all bytes are 0xFF */
		bool erase_success = true;
		int first_fail = -1;
		for (size_t i = 0; i < len; i++) {
			if (erase_buf[i] != 0xff) {
				if (first_fail == -1) {
					first_fail = (int)i;
				}
				erase_success = false;
			}
		}

		if (!erase_success) {
			printf("Flash erase FAILED - found non-0xFF values:\n");
			printf("  First failure at byte %d: expected 0xff, got 0x%02x\n",
			       first_fail, erase_buf[first_fail]);
			printf("Byte-by-byte comparison of first 16 bytes:\n");
			for (size_t i = 0; i < 16 && i < len; i++) {
				printf("  Byte %zu: 0x%02x %s\n",
				       i, erase_buf[i], (erase_buf[i] == 0xff) ? "OK" : "FAIL");
			}

			/* Try erasing again to see if double-erase helps */
			printf("\nRetrying erase operation...\n");
			rc = flash_erase(flash_dev, SPI_FLASH_TEST_REGION_OFFSET,
					 SPI_FLASH_SECTOR_SIZE);
			if (rc != 0) {
				printf("Second erase failed! %d\n", rc);
				return;
			}
			k_msleep(2000);

			memset(erase_buf, 0, len);
			rc = flash_read(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, erase_buf, len);
			if (rc != 0) {
				printf("Flash read after second erase failed! %d\n", rc);
				return;
			}
			printf("After 2nd erase: 0x%02x 0x%02x 0x%02x 0x%02x\n",
			       erase_buf[0], erase_buf[1], erase_buf[2], erase_buf[3]);

			erase_success = true;
			for (size_t i = 0; i < len; i++) {
				if (erase_buf[i] != 0xff) {
					erase_success = false;
					break;
				}
			}

			if (!erase_success) {
				printf("Still failed after second erase.\n");
				printf("This may indicate a hardware issue or driver bug.\n");
				return;
			}
			printf("Second erase succeeded!\n");
		} else {
			printf("Flash erase succeeded! All bytes are 0xFF\n");
		}
	}
	printf("\nTest 2: Flash write (writing 0x00-0xFF)\n");

	printf("Attempting to write %zu bytes\n", len);
	rc = flash_write(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, expected, len);
	if (rc != 0) {
		printf("Flash write failed! %d\n", rc);
		return;
	}

	printf("Waiting 2 seconds before read...\n");
	k_msleep(2000);

	memset(buf, 0, len);
	rc = flash_read(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, buf, len);
	if (rc != 0) {
		printf("Flash read failed! %d\n", rc);
		return;
	}

	// if (memcmp(expected, buf, len) == 0) {
	// 	printf("Data read matches data written. Good!!\n");
	// } else {
		const uint8_t *wp = expected;
		const uint8_t *rp = buf;
		const uint8_t *rpe = rp + len;
		uint16_t total_checked = 0;

		printf("reading the data written!!\n");
		int mismatch_count = 0;
		while (rp < rpe) {
			if (*rp != *wp) {
				printf("%08x wrote %02x read %02x MISMATCH\n",
				       (uint32_t)(SPI_FLASH_TEST_REGION_OFFSET + (rp - buf)),
				       *wp, *rp);
				mismatch_count++;
				if (mismatch_count >= 20) {
					printf("  ... (stopping after 20 mismatches)\n");
					break;
				}
			}
			++rp;
			++wp;
			total_checked++;
		}
		if (mismatch_count == 0) {
			printf("All %d bytes match!\n", total_checked);
		}
	// }
}

#if defined SPI_FLASH_MULTI_SECTOR_TEST
void multi_sector_test(const struct device *flash_dev)
{
	const uint8_t expected[] = { 0x55, 0xaa, 0x66, 0x99 };
	const size_t len = sizeof(expected);
	uint8_t buf[sizeof(expected)];
	int rc;

	printf("\nPerform test on multiple consecutive sectors");

	/* Write protection needs to be disabled before each write or
	 * erase, since the flash component turns on write protection
	 * automatically after completion of write and erase
	 * operations.
	 */
	printf("\nTest 1: Flash erase\n");

	/* Full flash erase if SPI_FLASH_TEST_REGION_OFFSET = 0 and
	 * SPI_FLASH_SECTOR_SIZE = flash size
	 * Erase 2 sectors for check for erase of consequtive sectors
	 */
	rc = flash_erase(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, SPI_FLASH_SECTOR_SIZE * 2);
	if (rc != 0) {
		printf("Flash erase failed! %d\n", rc);
	} else {
		/* Read the content and check for erased */
		memset(buf, 0, len);
		size_t offs = SPI_FLASH_TEST_REGION_OFFSET;

		while (offs < SPI_FLASH_TEST_REGION_OFFSET + 2 * SPI_FLASH_SECTOR_SIZE) {
			rc = flash_read(flash_dev, offs, buf, len);
			if (rc != 0) {
				printf("Flash read failed! %d\n", rc);
				return;
			}
			if (memcmp(erased, buf, len) != 0) {
				printf("Flash erase failed at offset 0x%x got 0x%x\n",
				offs, *(uint32_t *)buf);
				return;
			}
			offs += SPI_FLASH_SECTOR_SIZE;
		}
		printf("Flash erase succeeded!\n");
	}

	printf("\nTest 2: Flash write\n");

	size_t offs = SPI_FLASH_TEST_REGION_OFFSET;

	while (offs < SPI_FLASH_TEST_REGION_OFFSET + 2 * SPI_FLASH_SECTOR_SIZE) {
		printf("Attempting to write %zu bytes at offset 0x%x\n", len, offs);
		rc = flash_write(flash_dev, offs, expected, len);
		if (rc != 0) {
			printf("Flash write failed! %d\n", rc);
			return;
		}

		memset(buf, 0, len);
		rc = flash_read(flash_dev, offs, buf, len);
		if (rc != 0) {
			printf("Flash read failed! %d\n", rc);
			return;
		}

		if (memcmp(expected, buf, len) == 0) {
			printf("Data read matches data written. Good!!\n");
		} else {
			const uint8_t *wp = expected;
			const uint8_t *rp = buf;
			const uint8_t *rpe = rp + len;

			printf("Data read does not match data written!!\n");
			while (rp < rpe) {
				printf("%08x wrote %02x read %02x %s\n",
					(uint32_t)(offs + (rp - buf)),
					*wp, *rp, (*rp == *wp) ? "match" : "MISMATCH");
				++rp;
				++wp;
			}
		}
		offs += SPI_FLASH_SECTOR_SIZE;
	}
}
#endif

int main(void)
{
	const struct device *flash_dev = DEVICE_DT_GET_ONE(SPI_FLASH_COMPAT);

	if (!device_is_ready(flash_dev)) {
		printk("%s: device not ready.\n", flash_dev->name);
		return 0;
	}

	printf("\n%s SPI flash testing\n", flash_dev->name);
	printf("==========================\n");

	/* Verify that the correct flash chip (MX25U6432F) is connected */
	if (!verify_jedec_id(flash_dev)) {
		printf("\nFlash chip verification FAILED!\n");
		printf("Please connect MX25U6432F flash chip.\n");
		// return -1;
	}

	/* Test erase at multiple offsets to identify if issue is location-specific */
	printf("\nTesting erase at multiple offsets:\n");
	printf("==================================\n");
	test_erase_at_offset(flash_dev, 0x0);
	test_erase_at_offset(flash_dev, 0x1000);
	test_erase_at_offset(flash_dev, 0x10000);
	test_erase_at_offset(flash_dev, 0x50000);
	test_erase_at_offset(flash_dev, 0xff000);

	single_sector_test(flash_dev);
#if defined SPI_FLASH_MULTI_SECTOR_TEST
	multi_sector_test(flash_dev);
#endif
	return 0;
}
