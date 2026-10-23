/*
 * spiflash.h — FM25Q08B (8 Mbit / 1 MB) over hardware SPI.
 *
 * Wiring: CS# PC4, CLK PC5, DI PC6, DO PC7. WP# and HOLD# are tied to the rail
 * in hardware and are never driven. See docs/hardware.md.
 *
 * Mode 0 (CPOL=0, CPHA=0), MSB first, software chip select. That is what every
 * 25-series part expects; nothing here is FM25Q08B-specific except the JEDEC ID
 * and the timeouts.
 */

#ifndef SPIFLASH_H
#define SPIFLASH_H

#include <stdint.h>

/* Commands (docs/firmware-plan.md has the table). */
#define FLASH_CMD_JEDEC_ID      0x9Fu
#define FLASH_CMD_READ          0x03u
#define FLASH_CMD_FAST_READ     0x0Bu
#define FLASH_CMD_WRITE_ENABLE  0x06u
#define FLASH_CMD_WRITE_STATUS  0x01u
#define FLASH_CMD_PAGE_PROGRAM  0x02u
#define FLASH_CMD_SECTOR_ERASE  0x20u   /* 4 KB  */
#define FLASH_CMD_BLOCK_ERASE   0xD8u   /* 64 KB */
#define FLASH_CMD_CHIP_ERASE    0xC7u
#define FLASH_CMD_READ_STATUS1  0x05u
#define FLASH_CMD_DEEP_PWRDN    0xB9u
#define FLASH_CMD_RELEASE_DPD   0xABu

#define FLASH_SR1_BUSY          0x01u
#define FLASH_SR1_WEL           0x02u

/* Expected reply to 0x9F. Getting exactly these three bytes proves all four
 * SPI wires in both directions — the single best bring-up test (stage 5). */
#define FLASH_JEDEC_ID_0        0xA1u
#define FLASH_JEDEC_ID_1        0x40u
#define FLASH_JEDEC_ID_2        0x14u
#define FLASH_JEDEC_ID          0x00A14014u

#define FLASH_PAGE_SIZE         256u
#define FLASH_SECTOR_SIZE       4096u
#define FLASH_SIZE_BYTES        (1024u * 1024u)

/* Header written at address 0 by the host tool — keep in sync with
 * tools/send_chant.py and docs/audio-pipeline.md.
 *
 *   0  "CHNT"          4 B
 *   4  format          1 B
 *   5  reserved        1 B
 *   6  sample rate     4 B  little-endian, UNALIGNED — read it byte by byte
 *  10  data length     4 B  little-endian, UNALIGNED — read it byte by byte
 *  14  reserved        2 B
 *
 * The two u32s sit at offsets 6 and 10, which are not 4-byte aligned. The
 * QingKe V2A core does not do unaligned loads, so never cast the buffer to a
 * uint32_t* — spiflash_read_header() assembles them a byte at a time.
 */
#define FLASH_HDR_ADDR          0x000000u
#define FLASH_HDR_SIZE          16u
#define FLASH_HDR_MAGIC         "CHNT"
#define FLASH_DATA_ADDR         0x000010u   /* audio starts right after it */
#define FLASH_FMT_PCM8          0u
#define FLASH_FMT_ADPCM4        1u

typedef struct {
    uint8_t  format;
    uint32_t rate;
    uint32_t length;
} flash_header_t;

void     spiflash_init(void);
uint32_t spiflash_jedec_id(void);                                  /* 0x00A14014 */
void     spiflash_read(uint32_t addr, uint8_t *dst, uint32_t len);

/* Streaming read: one 03h command, then as many bytes as you like with CS held
 * low. Used by the loader's CRC pass, where issuing a fresh command per byte
 * would triple the time on the wire. */
void     spiflash_read_begin(uint32_t addr);
uint8_t  spiflash_read_next(void);
void     spiflash_read_end(void);

int      spiflash_page_program(uint32_t addr, const uint8_t *src, uint32_t len);
int      spiflash_sector_erase(uint32_t addr);
int      spiflash_chip_erase(void);
int      spiflash_unprotect(void);         /* clear the status-register BP bits */

void     spiflash_deep_power_down(void);   /* B9h — before MCU standby, always */
void     spiflash_wake(void);              /* ABh + recovery delay */

/* Reads and validates the 16-byte header. Returns 0 on success, -1 if the
 * magic is wrong or the fields are not plausible. */
int      spiflash_read_header(flash_header_t *hdr);

#endif /* SPIFLASH_H */
