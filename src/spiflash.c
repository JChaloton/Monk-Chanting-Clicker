/*
 * spiflash.c — FM25Q08B driver over CH32V003 SPI1.
 *
 * Bring-up order (docs/bring-up.md stage 5): call spiflash_init() then
 * spiflash_jedec_id() and expect 0x00A14014. If that reads back as 0x000000 the
 * MISO wire or the chip's power is wrong; 0xFFFFFF usually means CS# never went
 * low. Do not debug anything else until this one number is right.
 */

#include "ch32fun.h"
#include "board.h"
#include "spiflash.h"

/* Some ch32v003fun checkouts spell these differently or omit them. The numbers
 * are from the CH32V003 reference manual and are not going to change. */
#ifndef SPI_CTLR1_MSTR
#define SPI_CTLR1_MSTR      (1u << 2)
#endif
#ifndef SPI_CTLR1_SPE
#define SPI_CTLR1_SPE       (1u << 6)
#endif
#ifndef SPI_CTLR1_SSI
#define SPI_CTLR1_SSI       (1u << 8)
#endif
#ifndef SPI_CTLR1_SSM
#define SPI_CTLR1_SSM       (1u << 9)
#endif
#ifndef SPI_STATR_RXNE
#define SPI_STATR_RXNE      (1u << 0)
#endif
#ifndef SPI_STATR_TXE
#define SPI_STATR_TXE       (1u << 1)
#endif

#define CS_BIT              (1u << PIN_FLASH_CS_NUM)

/* GPIO CFGLR nibbles: value = (CNF << 2) | MODE. */
#define CFG_OUT_PP_10MHZ    0x1u
#define CFG_OUT_AFPP_50MHZ  0xBu
#define CFG_IN_PUPD         0x8u

/* ---- low level ----------------------------------------------------------- */

/* BSHR: writing bit n sets the pin, writing bit n+16 clears it. One store, no
 * read-modify-write, so it is safe to call from an ISR. */
static inline void cs_low(void)  { PIN_FLASH_CS_PORT->BSHR = CS_BIT << 16; }
static inline void cs_high(void) { PIN_FLASH_CS_PORT->BSHR = CS_BIT; }

static uint8_t xfer(uint8_t out)
{
    while (!(SPI1->STATR & SPI_STATR_TXE))
        ;
    SPI1->DATAR = out;
    while (!(SPI1->STATR & SPI_STATR_RXNE))
        ;
    return (uint8_t)SPI1->DATAR;
}

static void send_addr24(uint32_t addr)
{
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)(addr));
}

static uint8_t read_status1(void)
{
    uint8_t sr;
    cs_low();
    xfer(FLASH_CMD_READ_STATUS1);
    sr = xfer(0xFF);
    cs_high();
    return sr;
}

/* Poll BUSY until it clears. Returns 0 on success, -1 on timeout. Every program
 * and erase must go through this — a command issued while BUSY is set is
 * silently ignored by the chip, which looks exactly like a bad solder joint. */
static int wait_ready(uint32_t timeout_ms)
{
    uint32_t ticks = timeout_ms * 10u;      /* 100 us per poll */

    while (read_status1() & FLASH_SR1_BUSY) {
        if (ticks-- == 0u)
            return -1;
        Delay_Us(100);
    }
    return 0;
}

static void write_enable(void)
{
    cs_low();
    xfer(FLASH_CMD_WRITE_ENABLE);
    cs_high();
}

/* ---- api ----------------------------------------------------------------- */

void spiflash_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_SPI1;

    /* Raise CS in the output data register BEFORE switching the pin to an
     * output, so it never spends a moment driven low. A stray low CS at
     * power-on can leave the flash mid-command. */
    cs_high();
    PIN_FLASH_CS_PORT->CFGLR &= ~(0xfu << (4 * PIN_FLASH_CS_NUM));
    PIN_FLASH_CS_PORT->CFGLR |=  (CFG_OUT_PP_10MHZ << (4 * PIN_FLASH_CS_NUM));

    /* SCK and MOSI: alternate-function push-pull. */
    PIN_FLASH_CS_PORT->CFGLR &= ~(0xfu << (4 * PIN_FLASH_SCK_NUM));
    PIN_FLASH_CS_PORT->CFGLR |=  (CFG_OUT_AFPP_50MHZ << (4 * PIN_FLASH_SCK_NUM));
    PIN_FLASH_CS_PORT->CFGLR &= ~(0xfu << (4 * PIN_FLASH_MOSI_NUM));
    PIN_FLASH_CS_PORT->CFGLR |=  (CFG_OUT_AFPP_50MHZ << (4 * PIN_FLASH_MOSI_NUM));

    /* MISO: input with the pull-UP enabled. The flash tri-states DO whenever CS
     * is high; a floating input there drifts around the switching threshold and
     * burns microamps, which matters on a primary-cell budget. */
    PIN_FLASH_CS_PORT->CFGLR &= ~(0xfu << (4 * PIN_FLASH_MISO_NUM));
    PIN_FLASH_CS_PORT->CFGLR |=  (CFG_IN_PUPD << (4 * PIN_FLASH_MISO_NUM));
    PIN_FLASH_CS_PORT->BSHR   =  (1u << PIN_FLASH_MISO_NUM);

    /* Master, software NSS (SSM+SSI — without SSI the peripheral sees itself
     * deselected and refuses to clock), mode 0, 8-bit, MSB first. */
    SPI1->CTLR1 = SPI_CTLR1_MSTR | SPI_CTLR1_SSM | SPI_CTLR1_SSI
                | (FLASH_SPI_BR << 3);
    SPI1->CTLR1 |= SPI_CTLR1_SPE;
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

void spiflash_read_begin(uint32_t addr)
{
    cs_low();
    xfer(FLASH_CMD_READ);
    send_addr24(addr);
}

uint8_t spiflash_read_next(void)
{
    return xfer(0xFF);
}

void spiflash_read_end(void)
{
    cs_high();
}

int spiflash_page_program(uint32_t addr, const uint8_t *src, uint32_t len)
{
    /* A page program wraps around inside its own 256-byte page instead of
     * carrying into the next one — writing across a boundary silently corrupts
     * the start of the page. Refuse rather than produce a wrong flash image. */
    if (len == 0u || len > FLASH_PAGE_SIZE)
        return -1;
    if (((addr & (FLASH_PAGE_SIZE - 1u)) + len) > FLASH_PAGE_SIZE)
        return -1;

    write_enable();
    cs_low();
    xfer(FLASH_CMD_PAGE_PROGRAM);
    send_addr24(addr);
    while (len--)
        xfer(*src++);
    cs_high();

    return wait_ready(10);          /* datasheet max is ~3 ms */
}

int spiflash_sector_erase(uint32_t addr)
{
    write_enable();
    cs_low();
    xfer(FLASH_CMD_SECTOR_ERASE);
    send_addr24(addr);
    cs_high();

    return wait_ready(1000);        /* datasheet max is ~400 ms */
}

int spiflash_chip_erase(void)
{
    write_enable();
    cs_low();
    xfer(FLASH_CMD_CHIP_ERASE);
    cs_high();

    return wait_ready(60000);       /* whole 1 MB — tens of seconds */
}

int spiflash_unprotect(void)
{
    /* Clear the block-protect bits in status register 1. Parts usually ship
     * unprotected, but a chip that has been in a programmer before may not be,
     * and a protected chip accepts erase and program commands and quietly does
     * nothing. Cheap to do once at the top of a load. */
    write_enable();
    cs_low();
    xfer(FLASH_CMD_WRITE_STATUS);
    xfer(0x00);
    xfer(0x00);
    cs_high();

    return wait_ready(100);
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
    /* tRES1 is a few microseconds on this part. 20 us costs nothing and covers
     * a cold chip at 3.0 V; skipping it makes the first read after every wake
     * come back as garbage. */
    Delay_Us(20);
}

int spiflash_read_header(flash_header_t *hdr)
{
    uint8_t raw[FLASH_HDR_SIZE];

    spiflash_read(FLASH_HDR_ADDR, raw, FLASH_HDR_SIZE);

    if (raw[0] != 'C' || raw[1] != 'H' || raw[2] != 'N' || raw[3] != 'T')
        return -1;

    /* Byte at a time: offsets 6 and 10 are not 4-byte aligned and this core
     * traps unaligned word loads. See the layout comment in spiflash.h. */
    hdr->format = raw[4];
    hdr->rate   = (uint32_t)raw[6]  | ((uint32_t)raw[7]  << 8)
                | ((uint32_t)raw[8] << 16) | ((uint32_t)raw[9] << 24);
    hdr->length = (uint32_t)raw[10] | ((uint32_t)raw[11] << 8)
                | ((uint32_t)raw[12] << 16) | ((uint32_t)raw[13] << 24);

    if (hdr->length == 0u || hdr->length > (FLASH_SIZE_BYTES - FLASH_DATA_ADDR))
        return -1;
    if (hdr->rate < 2000u || hdr->rate > 24000u)
        return -1;

    return 0;
}
