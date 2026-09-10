/*
 * main.c — chant clicker player firmware.
 *
 * Sequence, per docs/firmware-plan.md:
 *   idle -> press -> debounce -> wake flash -> amp on -> stream chant ->
 *   ramp out -> amp off -> flash deep power-down -> idle
 *
 * WHAT IS NOT HERE YET
 * --------------------
 * Step 7 of the build order, MCU standby with EXTI wake, is deliberately not
 * implemented. Everything else in the power sequence is: the amp is muted and
 * the flash is in deep power-down (< 1 uA each) whenever nothing is playing, so
 * the remaining idle current is the MCU running its poll loop, a few
 * milliamps. That is fine on the bench and wrong for a keychain on a CR123A.
 *
 * It is left out on purpose rather than guessed at: standby on this part
 * changes how the debug interface behaves, and a wrong first attempt is the
 * kind of thing that looks like a dead board. Get sound out first. If a
 * WCH-LinkE ever does refuse to connect, `minichlink -u` power-cycle-unbricks
 * it. See the note at the bottom of this file.
 */

#include "ch32fun.h"
#include "board.h"
#include "spiflash.h"
#include "audio.h"
#include <stdio.h>

#define CFG_OUT_PP_10MHZ    0x1u
#define CFG_IN_PUPD         0x8u

/* printf goes out over the SWIO line to `make monitor`, not over the UART, so
 * it costs no pins and does not fight the loader. Never call it while audio is
 * playing — it blocks for milliseconds and the sample clock will not wait. */
#ifndef PLAYER_DEBUG
#define PLAYER_DEBUG 1
#endif
#if PLAYER_DEBUG
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...) do { } while (0)
#endif

static flash_header_t hdr;
static int have_chant;

/* ------------------------------------------------------------------------- */

static void amp_enable(int on)
{
    /* U2 SD is active LOW and R5 100k holds it there, so the amp is muted
     * whenever this pin is low or floating — including during reset. */
    PIN_AMP_EN_PORT->BSHR = on ? (1u << PIN_AMP_EN_NUM)
                               : (1u << (PIN_AMP_EN_NUM + 16));
}

static void board_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD;

    /* Amp first, and low before it is even an output, so the amp cannot hiss
     * while the rest of init runs. */
    amp_enable(0);
    PIN_AMP_EN_PORT->CFGLR &= ~(0xfu << (4 * PIN_AMP_EN_NUM));
    PIN_AMP_EN_PORT->CFGLR |=  (CFG_OUT_PP_10MHZ << (4 * PIN_AMP_EN_NUM));

    /* Switch: input with the internal pull-UP. CNF = 10, MODE = 00 -> 0x8;
     * writing the output data register high selects pull-up over pull-down.
     * SW1 shorts the pin to GND, so a press reads LOW. */
    PIN_SW_PORT->CFGLR &= ~(0xfu << (4 * PIN_SW_NUM));
    PIN_SW_PORT->CFGLR |=  (CFG_IN_PUPD << (4 * PIN_SW_NUM));
    PIN_SW_PORT->BSHR   =  (1u << PIN_SW_NUM);
}

static int switch_pressed(void)
{
    return !(PIN_SW_PORT->INDR & (1u << PIN_SW_NUM));   /* active LOW */
}

/* See it low, wait out the bounce, see it low again. A blue mechanical switch
 * bounces for a few milliseconds; SW_DEBOUNCE_MS is 20. */
static int debounced_press(void)
{
    if (!switch_pressed())
        return 0;
    Delay_Ms(SW_DEBOUNCE_MS);
    return switch_pressed();
}

/* ------------------------------------------------------------------------- */

static void play_chant(void)
{
    spiflash_wake();
    amp_enable(1);
    Delay_Ms(5);                    /* let the amp's own start-up settle */

    audio_start(FLASH_DATA_ADDR, hdr.length, hdr.format, hdr.rate);

    while (audio_busy())
        audio_service();            /* refill whichever buffer the ISR spent */

    audio_stop();                   /* ramps duty back to mid-scale */
    Delay_Ms(2);                    /* let the ramp reach the amp before mute */

    /* Order matters on the way down: timer, then amp, then flash. */
    amp_enable(0);
    spiflash_deep_power_down();
}

int main(void)
{
    SystemInit();

    board_init();
    spiflash_init();
    audio_init();

    LOG("\nchant-clicker player\n");

    spiflash_wake();

    uint32_t id = spiflash_jedec_id();
    LOG("flash JEDEC id %06lx (expect %06lx)\n",
        (unsigned long)id, (unsigned long)FLASH_JEDEC_ID);
    if (id != FLASH_JEDEC_ID)
        LOG("  wrong id: 000000 = MISO/power, ffffff = CS# never goes low\n");

    have_chant = (spiflash_read_header(&hdr) == 0);
    if (have_chant) {
        LOG("chant: fmt %u, %lu Hz, %lu bytes (~%lu s)\n",
            (unsigned)hdr.format, (unsigned long)hdr.rate,
            (unsigned long)hdr.length, (unsigned long)(hdr.length / hdr.rate));
        if (hdr.format != FLASH_FMT_PCM8) {
            LOG("  format %u is not 8-bit PCM — this firmware cannot play it\n",
                (unsigned)hdr.format);
            have_chant = 0;
        }
    } else {
        LOG("no valid CHNT header at 0x000000 — run tools/send_chant.py first\n");
    }

    spiflash_deep_power_down();

    for (;;) {
        if (debounced_press()) {
            if (have_chant)
                play_chant();

            /* One press, one play. Presses during playback are ignored because
             * play_chant() blocks — that is the answer to the open question in
             * docs/decisions.md, chosen because a chant that restarts halfway
             * through is worse than one that finishes. */
            while (switch_pressed())
                Delay_Ms(10);
            Delay_Ms(SW_DEBOUNCE_MS);
        }
        Delay_Ms(5);
    }
}

/*
 * If the WCH-LinkE ever refuses to connect after a firmware change:
 *
 *     minichlink -u          unbrick: hold the target in reset while erasing
 *
 * It is not needed for anything this file does today, but it is the escape
 * hatch for step 7 when standby lands, and worth knowing before you need it.
 */
