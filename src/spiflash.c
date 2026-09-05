/*
 * spiflash.c — FM25Q08B driver. SKELETON, nothing implemented.
 *
 * Start with spiflash_init() + spiflash_jedec_id() only: bring-up stage 5 is
 * "print the reply to 0x9F and expect A1 40 14". Do not write the rest until
 * that passes.
 */

#include "ch32v003fun.h"
#include "board.h"
#include "spiflash.h"

/* ---- low level ----------------------------------------------------------- */

static void cs_low(void)  { /* TODO: PIN_FLASH_CS -> 0 */ }
static void cs_high(void) { /* TODO: PIN_FLASH_CS -> 1 */ }

static uint8_t xfer(uint8_t out)
{
    /* TODO: write DATAR, poll for TXE/RXNE, return the received byte.
     * Streaming needs only 64 kbit/s against a 50 MHz-capable part — there is
     * no reason to push the clock. Start slow, speed up later if ever needed. */
    (void)out;
    return 0;
}

static void send_addr24(uint32_t addr)
{
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)(addr));
}

/* ---- api ----------------------------------------------------------------- */

void spiflash_init(void)
{
    /* TODO: enable SPI1 clock, configure as master, MSB first, mode 0
     * (CPOL=0, CPHA=0 — what 25-series parts expect), software NSS, CS idle high. */
}

uint32_t spiflash_jedec_id(void)
{
    uint8_t a, b, c;
    cs_low();
    xfer(FLASH_CMD_JEDEC_ID);
    a = xfer(0xFF);
    b = xfer(0xFF);
    c = xfer(0xFF);
    cs_high();
    return ((uint32_t)a << 16) | ((uint32_t)b << 8) | c;   /* expect 0x00A14014 */
}

void spiflash_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    cs_low();
    xfer(FLASH_CMD_READ);
    send_addr24(addr);
    while (len--)
        *dst++ = xfer(0xFF);
    cs_high();
}

int spiflash_page_program(uint32_t addr, const uint8_t *src, uint32_t len)
{
    /* TODO: 06h write-enable, 02h + addr + data (must not cross a 256-byte page
     * boundary), then poll 05h until BUSY clears. Return 0 on success. */
    (void)addr; (void)src; (void)len;
    return -1;
}

int spiflash_sector_erase(uint32_t addr)
{
    /* TODO: 06h, 20h + addr, poll 05h BUSY. */
    (void)addr;
    return -1;
}

void spiflash_deep_power_down(void)
{
    cs_low();
    xfer(FLASH_CMD_DEEP_PWRDN);
    cs_high();
    /* < 1 uA after this. Nothing may talk to the chip until spiflash_wake(). */
}

void spiflash_wake(void)
{
    cs_low();
    xfer(FLASH_CMD_RELEASE_DPD);
    cs_high();
    /* TODO: wait tRES1 (a few us — check the FM25Q08B datasheet) before any
     * further command, or the first read comes back as garbage. */
}
