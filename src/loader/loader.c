/*
 * loader.c — one-off firmware that writes the chant into U3 over UART.
 *
 * This is route C from docs/audio-pipeline.md, and the only route left once the
 * flash is soldered down. Flash this instead of the player, run
 * tools/send_chant.py, then flash the player back. It is deliberately dumb:
 * the host drives everything, the MCU only executes one command at a time and
 * answers. No state machine to get out of sync, no timeouts on this side.
 *
 * Wiring (see docs/hardware.md and the ISP header on the schematic):
 *
 *   adapter RX  <- PD5  (U1 pin 2, labelled TX on the header)
 *   adapter TX  -> PD6  (U1 pin 3, labelled RX on the header)
 *   adapter GND -- GND
 *
 * The labels are from the MCU's point of view, so they cross. 115200 8N1.
 *
 * PC3 (amp enable) is driven LOW first thing and never touched again — a loader
 * that makes noise while it works would be unpleasant and would also put a
 * couple of hundred milliamps of class-D switching next to the SPI bus.
 *
 * PROTOCOL
 * --------
 * Every command is one ASCII byte, optionally followed by arguments. Every
 * reply starts with a status byte: 'K' = ok, 'E' = flash timed out,
 * 'C' = checksum mismatch. Multi-byte integers on the wire are BIG-endian
 * (MSB first), matching the order the flash chip itself wants its addresses in.
 * The little-endian fields inside the 16-byte header are payload, not protocol.
 *
 *   '?'                              -> 'K' "CL01"        ping and version
 *   'I'                              -> 'K' id2 id1 id0   JEDEC ID (A1 40 14)
 *   'U'                              -> 'K' | 'E'         clear block protect
 *   'S' a2 a1 a0                     -> 'K' | 'E'         erase 4 KB sector
 *   'P' a2 a1 a0 <256 bytes> sum     -> 'K' | 'C' | 'E'   program one page
 *   'X' a2 a1 a0 n3 n2 n1 n0         -> 'K' c3 c2 c1 c0   CRC32 over a range
 *
 * 'P' always carries exactly 256 bytes; the host pads the final page with 0xFF,
 * which is the erased state, so the padding costs nothing. `sum` is the low byte
 * of the sum of the three address bytes plus all 256 data bytes.
 *
 * 'X' is why a full 1 MB load verifies in seconds instead of minutes: reading
 * the whole chip back over the UART would take as long as writing it, so the
 * MCU computes the CRC32 itself and sends four bytes. Same polynomial as
 * zlib.crc32, so tools/send_chant.py compares directly.
 */

#include "ch32fun.h"
#include "board.h"
#include "spiflash.h"
#include <stdint.h>

#ifndef USART_CTLR1_RE
#define USART_CTLR1_RE      (1u << 2)
#endif
#ifndef USART_CTLR1_TE
#define USART_CTLR1_TE      (1u << 3)
#endif
#ifndef USART_CTLR1_UE
#define USART_CTLR1_UE      (1u << 13)
#endif
#ifndef USART_STATR_RXNE
#define USART_STATR_RXNE    (1u << 5)
#endif
#ifndef USART_STATR_TXE
#define USART_STATR_TXE     (1u << 7)
#endif

#define CFG_OUT_PP_10MHZ    0x1u
#define CFG_OUT_AFPP_10MHZ  0x9u
#define CFG_IN_PUPD         0x8u

#define LOADER_VERSION      "CL01"

static uint8_t page[FLASH_PAGE_SIZE];

/* ---- uart ---------------------------------------------------------------- */

static void uart_init(uint32_t baud)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD | RCC_APB2Periph_USART1;

    /* TX: alternate-function push-pull. */
    PIN_UART_PORT->CFGLR &= ~(0xfu << (4 * PIN_UART_TX_NUM));
    PIN_UART_PORT->CFGLR |=  (CFG_OUT_AFPP_10MHZ << (4 * PIN_UART_TX_NUM));

    /* RX: input, pull-up. The pull-up keeps the line idle-high if the adapter is
     * unplugged, so a disconnected cable reads as silence rather than a stream
     * of framing errors. */
    PIN_UART_PORT->CFGLR &= ~(0xfu << (4 * PIN_UART_RX_NUM));
    PIN_UART_PORT->CFGLR |=  (CFG_IN_PUPD << (4 * PIN_UART_RX_NUM));
    PIN_UART_PORT->BSHR   =  (1u << PIN_UART_RX_NUM);

    /* BRR holds 16 x USARTDIV as a plain integer: mantissa in [15:4], the
     * sixteenths in [3:0]. 48 MHz / 115200 = 416.67 -> 417, which is 0.08 % off.
     * The internal RC oscillator's own 1 % error dwarfs that, which is the real
     * reason not to push this much past 115200. */
    USART1->BRR   = (FUNCONF_SYSTEM_CORE_CLOCK + (baud / 2u)) / baud;
    USART1->CTLR2 = 0;              /* 1 stop bit */
    USART1->CTLR3 = 0;              /* no flow control */
    USART1->CTLR1 = USART_CTLR1_TE | USART_CTLR1_RE | USART_CTLR1_UE;
}

/* Blocking, and that is the point: the host never sends a byte we are not
 * already waiting for, because it waits for our reply to the previous command.
 * Reading STATR before DATAR also clears a stale overrun for free. */
static uint8_t uart_rx(void)
{
    while (!(USART1->STATR & USART_STATR_RXNE))
        ;
    return (uint8_t)USART1->DATAR;
}

/* Bounded receive. Returns 0 if no byte arrived within UART_RX_TIMEOUT_MS.
 *
 * Only cmd_program uses this, and it is what makes a dropped byte survivable.
 * A page is 261 bytes sent back-to-back; if noise costs us one, the unbounded
 * uart_rx() above would sit forever waiting for a byte the host has already
 * finished sending, while the host sits forever waiting for our status byte.
 * Both ends wedge, and the host's '?' resync bytes get eaten as page data.
 *
 * 50 ms is chosen to sit in the gap between the two timescales that matter:
 * far above the 87 us between bytes of a back-to-back burst at 115200 (so a
 * healthy transfer never trips it, and USB's 1 ms frame scheduling cannot
 * either), and far below the host's 3 s status timeout (so we are back at the
 * command loop, having answered 'C', long before the host gives up). Because
 * it only fires after 50 ms of silence, the host has demonstrably stopped
 * sending - so there are no stray page bytes left in flight to be misread as
 * commands once we return. */
#define UART_RX_TIMEOUT_MS  50u

static int uart_rx_to(uint8_t *out)
{
    uint32_t start = funSysTick32();
    uint32_t limit = (uint32_t)DELAY_MS_TIME * UART_RX_TIMEOUT_MS;

    for (;;) {
        if (USART1->STATR & USART_STATR_RXNE) {
            *out = (uint8_t)USART1->DATAR;
            return 1;
        }
        if ((uint32_t)(funSysTick32() - start) > limit)
            return 0;
    }
}

static void uart_tx(uint8_t c)
{
    while (!(USART1->STATR & USART_STATR_TXE))
        ;
    USART1->DATAR = c;
}

static void uart_tx_n(const uint8_t *p, uint32_t n)
{
    while (n--)
        uart_tx(*p++);
}

/* Bounded too, and for the same reason as cmd_program: an opcode whose operand
 * bytes never arrive must not park the loader mid-command. If it does, the
 * host's '?' resync probes get eaten as operand bytes - three of them complete
 * an address and trigger a real sector erase, which answers 'K'. That is how a
 * single corrupted byte turns into a storm of spurious erases and replies that
 * the host cannot tell apart from real ones. Returning to the command loop on
 * silence is what makes '?' mean '?' again. */
static int rx_addr24(uint32_t *out)
{
    uint32_t a = 0;
    uint8_t  b;

    for (int i = 0; i < 3; i++) {
        if (!uart_rx_to(&b))
            return 0;
        a = (a << 8) | b;
    }
    *out = a;
    return 1;
}

static int rx_u32(uint32_t *out)
{
    uint32_t v = 0;
    uint8_t  b;

    for (int i = 0; i < 4; i++) {
        if (!uart_rx_to(&b))
            return 0;
        v = (v << 8) | b;
    }
    *out = v;
    return 1;
}

static void tx_u32(uint32_t v)
{
    uart_tx((uint8_t)(v >> 24));
    uart_tx((uint8_t)(v >> 16));
    uart_tx((uint8_t)(v >> 8));
    uart_tx((uint8_t)v);
}

/* ---- crc ----------------------------------------------------------------- */

/* Bitwise CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — identical to
 * Python's zlib.crc32. A 1 kB table would be four times faster, but 1 MB still
 * takes only a few seconds at 48 MHz and RAM is 2 KB total. */
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

static void cmd_jedec(void)
{
    uint32_t id = spiflash_jedec_id();

    uart_tx('K');
    uart_tx((uint8_t)(id >> 16));
    uart_tx((uint8_t)(id >> 8));
    uart_tx((uint8_t)id);
}

static void cmd_program(void)
{
    uint32_t addr = 0;
    uint8_t  b;

    /* Every read here is bounded. Any short read is reported as 'C' - the same
     * answer as a bad checksum, and the host already knows how to retry that. */
    for (int i = 0; i < 3; i++) {
        if (!uart_rx_to(&b)) {
            uart_tx('C');
            return;
        }
        addr = (addr << 8) | b;
    }

    uint8_t sum = (uint8_t)((addr >> 16) + (addr >> 8) + addr);

    for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
        if (!uart_rx_to(&b)) {
            uart_tx('C');
            return;
        }
        page[i] = b;
        sum = (uint8_t)(sum + b);
    }

    if (!uart_rx_to(&b) || b != sum) {
        /* Say so and write nothing. The host retries the page. A UART with no
         * flow control on a hand-wired board does drop bytes occasionally; a
         * silent bad byte in the middle of a chant is very hard to hear. */
        uart_tx('C');
        return;
    }

    uart_tx(spiflash_page_program(addr, page, FLASH_PAGE_SIZE) == 0 ? 'K' : 'E');
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
    spiflash_init();

    /* If the player firmware left the chip in deep power-down, it will ignore
     * every command until it gets ABh. Costs 20 us and removes a whole class of
     * "the loader says E to everything" confusion. */
    spiflash_wake();

    uart_init(LOADER_BAUD);

    for (;;) {
        switch (uart_rx()) {
        case '?':
            uart_tx('K');
            uart_tx_n((const uint8_t *)LOADER_VERSION, 4);
            break;

        case 'I':
            cmd_jedec();
            break;

        case 'U':
            uart_tx(spiflash_unprotect() == 0 ? 'K' : 'E');
            break;

        case 'S': {
            uint32_t addr;
            if (!rx_addr24(&addr)) {
                /* Operand never turned up. Answer 'E' so the host retries at
                 * once rather than waiting out its full status timeout, and
                 * erase nothing - a half-specified address is not a sector. */
                uart_tx('E');
                break;
            }
            uart_tx(spiflash_sector_erase(addr) == 0 ? 'K' : 'E');
            break;
        }

        case 'P':
            cmd_program();
            break;

        case 'X': {
            uint32_t addr, len;
            if (!rx_addr24(&addr) || !rx_u32(&len)) {
                uart_tx('E');
                break;
            }
            uint32_t crc = crc32_flash(addr, len);
            uart_tx('K');
            tx_u32(crc);
            break;
        }

        default:
            /* Unknown byte: ignore it silently. Line noise at connect time, or
             * a leftover reply the host did not read, must not desynchronise
             * anything — the host resyncs by sending '?' until it gets 'K'. */
            break;
        }
    }
}
