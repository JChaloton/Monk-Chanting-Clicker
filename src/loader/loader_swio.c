/*
 * loader_swio.c — one-off firmware that writes the chant into U3 with no UART.
 *
 * This is route D from docs/audio-pipeline.md: the same job as loader.c, but
 * the host reaches the MCU over the single SWIO wire the WCH-LinkE already
 * uses for programming. Nothing else is connected. On a board whose PD5/PD6
 * are not wired to a serial adapter — which is every board after rev B — this
 * is the only in-circuit route.
 *
 *   host --USB--> WCH-LinkE --SWIO (PD1)--> MCU --SPI (PC4..PC7)--> U3
 *
 * SPI still does the actual writing, exactly as in loader.c. What changes is
 * only how the bytes reach the MCU.
 *
 * WHY A RAM MAILBOX AND NOT A BYTE STREAM
 * ---------------------------------------
 * ch32fun does have a byte channel over SWIO — the one `minichlink -T` uses for
 * printf. It is useless here for two reasons. It moves three bytes per USB
 * round trip, so 82 KB would take tens of thousands of transactions; and on
 * Windows minichlink reads the host side of it with _kbhit()/_getch(), which
 * only see a real console, so nothing can pipe into it from a script.
 *
 * So instead the host uses the debug module's plain memory access — the same
 * mechanism that writes firmware — to drop a request straight into this chip's
 * RAM, and `minichlink -w` moves a whole 512-byte chunk per invocation. The
 * debug module halts the core to do that and resumes it afterwards, so from
 * this firmware's point of view the mailbox simply changes between two reads.
 *
 * THE MAILBOX
 * -----------
 * A fixed address, because the host has to know where to write without parsing
 * our ELF. 0x20000300 sits above this firmware's .data/.bss (a few hundred
 * bytes, growing up from 0x20000000) and below the stack (growing down from
 * 0x20000800). main() checks the lower boundary at boot and refuses to run if
 * .bss ever grows into it, rather than corrupting itself quietly.
 *
 * The host writes magic..data in one contiguous blob, so a request arrives
 * atomically: the core is halted for the whole write.
 *
 * PROTOCOL
 * --------
 * Host: fill addr/len/data, set a fresh `seq`, set `cmd` last-but-same-blob.
 * Loader: execute, write `status` and `reply`, copy `seq` into `ack`, clear
 * `cmd`. The host knows a request is finished when `ack == seq`.
 *
 *   cmd 1 PING       -> reply = 'CLSW' version word
 *   cmd 2 JEDEC      -> reply = 0x00A14014
 *   cmd 3 UNPROTECT  -> clears the status-register BP bits
 *   cmd 4 ERASE      -> 4 KB sector containing addr
 *   cmd 5 PROGRAM    -> len bytes of data[] to addr  (len <= 512)
 *   cmd 6 CRC32      -> reply = CRC32 over addr..addr+len
 *
 * The host does not wait for `ack` after every PROGRAM. A 512-byte program is
 * two page writes, under 6 ms worst case, while one minichlink invocation
 * costs upwards of 200 ms — so the work is always long finished before the
 * next chunk arrives, and the CRC32 at the end would catch it if it were not.
 * Erases DO get waited on: tSE is up to 300 ms and that margin is not there.
 *
 * PC3 (amp enable) is driven LOW first thing and never touched again, for the
 * same reason as in loader.c: no class-D switching next to the SPI bus.
 */

#include "ch32fun.h"
#include "board.h"
#include "spiflash.h"
#include <stdint.h>

#define CFG_OUT_PP_10MHZ    0x1u

/* ---- the mailbox --------------------------------------------------------- */

#define MBOX_ADDR           0x20000300u
#define MBOX_MAGIC          0x434C5357u     /* "CLSW" — tools/swio_link.py agrees */
#define MBOX_DATA_MAX       512u
#define LOADER_VERSION      0x53570100u     /* 'S' 'W' 1.0 — bumped if this breaks */

#define CMD_NONE            0u
#define CMD_PING            1u
#define CMD_JEDEC           2u
#define CMD_UNPROTECT       3u
#define CMD_ERASE           4u
#define CMD_PROGRAM         5u
#define CMD_CRC32           6u

#define ST_OK               0u
#define ST_FLASH_TIMEOUT    1u
#define ST_BAD_ARGS         2u
#define ST_BAD_CMD          3u

typedef struct {
    uint32_t magic;                 /*  0  MBOX_MAGIC once we are alive       */
    uint32_t cmd;                   /*  4  host sets, we clear                */
    uint32_t addr;                  /*  8  flash byte address                 */
    uint32_t len;                   /* 12  bytes in data[], or CRC length     */
    uint32_t seq;                   /* 16  host's request number              */
    uint32_t ack;                   /* 20  we echo seq here when done         */
    uint32_t status;                /* 24  ST_*                               */
    uint32_t reply;                 /* 28  JEDEC id, CRC32, version           */
    uint8_t  data[MBOX_DATA_MAX];   /* 32                                     */
} mbox_t;

static volatile mbox_t * const mb = (volatile mbox_t *)MBOX_ADDR;

/* One flash page, copied out of the mailbox before it is programmed.
 *
 * Not an optimisation — a safety margin. The host may halt us and overwrite
 * data[] at any instant. Programming out of a private copy means the worst a
 * badly timed halt can do is write a stale page, which the CRC32 catches,
 * rather than a page spliced together from two different chunks. */
static uint8_t page[FLASH_PAGE_SIZE];

/* ---- crc ----------------------------------------------------------------- */

/* Bitwise CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — identical to
 * Python's zlib.crc32, so the host compares the four bytes directly. Same
 * routine as loader.c; a table would be four times faster and cost 1 kB of the
 * 2 kB of RAM, which is not a trade worth making for a few seconds. */
static uint32_t crc32_flash(uint32_t addr, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    spiflash_read_begin(addr);
    while (len--) {
        crc ^= (uint32_t)spiflash_read_next();
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    spiflash_read_end();

    return ~crc;
}

/* ---- commands ------------------------------------------------------------ */

/* Program up to MBOX_DATA_MAX bytes, one flash page at a time.
 *
 * The chip wraps a page program within its own 256-byte page instead of
 * carrying into the next one, so a transfer that straddles a boundary must be
 * split. The host always sends page-aligned addresses, but splitting here as
 * well costs nothing and means a hand-built request cannot silently corrupt a
 * page. */
static uint32_t do_program(uint32_t addr, uint32_t len)
{
    uint32_t done = 0;

    if (len == 0 || len > MBOX_DATA_MAX)
        return ST_BAD_ARGS;
    if (addr + len > FLASH_SIZE_BYTES)
        return ST_BAD_ARGS;

    while (done < len) {
        uint32_t dst   = addr + done;
        uint32_t room  = FLASH_PAGE_SIZE - (dst & (FLASH_PAGE_SIZE - 1u));
        uint32_t chunk = len - done;

        if (chunk > room)
            chunk = room;

        for (uint32_t i = 0; i < chunk; i++)
            page[i] = mb->data[done + i];

        if (spiflash_page_program(dst, page, chunk) != 0)
            return ST_FLASH_TIMEOUT;

        done += chunk;
    }

    return ST_OK;
}

static uint32_t execute(uint32_t cmd, uint32_t addr, uint32_t len, uint32_t *reply)
{
    *reply = 0;

    switch (cmd) {
    case CMD_PING:
        *reply = LOADER_VERSION;
        return ST_OK;

    case CMD_JEDEC:
        *reply = spiflash_jedec_id();
        return ST_OK;

    case CMD_UNPROTECT:
        return spiflash_unprotect() == 0 ? ST_OK : ST_FLASH_TIMEOUT;

    case CMD_ERASE:
        if (addr >= FLASH_SIZE_BYTES)
            return ST_BAD_ARGS;
        return spiflash_sector_erase(addr) == 0 ? ST_OK : ST_FLASH_TIMEOUT;

    case CMD_PROGRAM:
        return do_program(addr, len);

    case CMD_CRC32:
        if (addr + len > FLASH_SIZE_BYTES)
            return ST_BAD_ARGS;
        *reply = crc32_flash(addr, len);
        return ST_OK;

    default:
        return ST_BAD_CMD;
    }
}

/* ---- main ---------------------------------------------------------------- */

static void amp_off(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    PIN_AMP_EN_PORT->BSHR   =  (1u << (PIN_AMP_EN_NUM + 16));   /* LOW = muted */
    PIN_AMP_EN_PORT->CFGLR &= ~(0xfu << (4 * PIN_AMP_EN_NUM));
    PIN_AMP_EN_PORT->CFGLR |=  (CFG_OUT_PP_10MHZ << (4 * PIN_AMP_EN_NUM));
}

int main(void)
{
    SystemInit();

    amp_off();

    /* The mailbox address is a constant, so prove the assumption behind it
     * before anything depends on it. If .bss ever grows past MBOX_ADDR the two
     * overlap and every symptom after that is nonsense — a silent hang here is
     * far easier to diagnose, because the host times out waiting for the magic
     * word and says so. */
    if ((uintptr_t)(page + sizeof(page)) >= MBOX_ADDR)
        for (;;)
            ;

    spiflash_init();

    /* The player leaves the chip in deep power-down, where it ignores every
     * command. Costs 20 us and removes a whole class of "the loader answers
     * nothing but errors" confusion. */
    spiflash_wake();

    mb->cmd    = CMD_NONE;
    mb->ack    = 0;
    mb->seq    = 0;
    mb->status = ST_OK;
    mb->reply  = 0;

    /* Written last. The host polls this word to know the loader has booted and
     * the rest of the mailbox is initialised, so it must not appear early. */
    mb->magic = MBOX_MAGIC;

    for (;;) {
        uint32_t cmd = mb->cmd;

        if (cmd == CMD_NONE)
            continue;

        /* Snapshot the arguments. The host cannot halt us between these reads
         * and the execute() call often enough to matter, but reading each
         * field once keeps the request we execute self-consistent. */
        uint32_t seq  = mb->seq;
        uint32_t addr = mb->addr;
        uint32_t len  = mb->len;
        uint32_t reply;

        uint32_t status = execute(cmd, addr, len, &reply);

        mb->status = status;
        mb->reply  = reply;
        mb->ack    = seq;

        /* Clear last. `cmd == 0` is what the host reads as "idle", so every
         * other field must already be valid when it becomes true. */
        mb->cmd = CMD_NONE;
    }
}
