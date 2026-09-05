/*
 * spiflash.h — FM25Q08B (8 Mbit / 1 MB) over hardware SPI. SKELETON.
 *
 * Wiring: CS# PC4, CLK PC5, DI PC6, DO PC7. WP# and HOLD# are tied to the rail
 * in hardware and are never driven. See docs/hardware.md.
 */

#ifndef SPIFLASH_H
#define SPIFLASH_H

#include <stdint.h>

/* Commands (docs/firmware-plan.md has the table). */
#define FLASH_CMD_JEDEC_ID      0x9Fu
#define FLASH_CMD_READ          0x03u
#define FLASH_CMD_FAST_READ     0x0Bu
#define FLASH_CMD_WRITE_ENABLE  0x06u
#define FLASH_CMD_PAGE_PROGRAM  0x02u
#define FLASH_CMD_SECTOR_ERASE  0x20u   /* 4 KB  */
#define FLASH_CMD_BLOCK_ERASE   0xD8u   /* 64 KB */
#define FLASH_CMD_READ_STATUS1  0x05u
#define FLASH_CMD_DEEP_PWRDN    0xB9u
#define FLASH_CMD_RELEASE_DPD   0xABu

/* Expected reply to 0x9F. Getting exactly these three bytes proves all four
 * SPI wires in both directions — the single best bring-up test (stage 5). */
#define FLASH_JEDEC_ID_0        0xA1u
#define FLASH_JEDEC_ID_1        0x40u
#define FLASH_JEDEC_ID_2        0x14u

#define FLASH_PAGE_SIZE         256u
#define FLASH_SIZE_BYTES        (1024u * 1024u)

/* Header written at address 0 by tools/wav2raw.py — keep in sync. */
#define FLASH_HDR_ADDR          0x000000u
#define FLASH_HDR_SIZE          16u
#define FLASH_HDR_MAGIC         "CHNT"
#define FLASH_FMT_PCM8          0u
#define FLASH_FMT_ADPCM4        1u

void     spiflash_init(void);
uint32_t spiflash_jedec_id(void);                                  /* 0x00A14014 */
void     spiflash_read(uint32_t addr, uint8_t *dst, uint32_t len);
int      spiflash_page_program(uint32_t addr, const uint8_t *src, uint32_t len);
int      spiflash_sector_erase(uint32_t addr);
void     spiflash_deep_power_down(void);   /* B9h — before MCU standby, always */
void     spiflash_wake(void);              /* ABh + recovery delay */

#endif /* SPIFLASH_H */
