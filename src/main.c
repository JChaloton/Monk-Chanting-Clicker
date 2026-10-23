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

/* Bring-up aids: a 1 kHz tone straight out of TIM1, and a sweep that replays
 * the flash at several candidate sample rates. Both earned their keep finding
 * the "slow and faint" bug (the chant was a 44.1 kHz file with 8000 stamped in
 * its header) and are kept, switched off, for the next time something is
 * silent. See docs/bring-up.md. */
#ifndef PLAYER_SELFTEST
#define PLAYER_SELFTEST 0
#endif

static flash_header_t hdr;
static int have_chant;

/* ------------------------------------------------------------------------- */

static void amp_enable(int on)
{
    /* U2 SD is active LOW and R4 100k holds it there, so the amp is muted
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

/*
 * Plays `len` bytes from the start of the audio and reports the rate it
 * actually achieved. `len` is a parameter so bring-up can measure on a two
 * second slice instead of sitting through the whole chant.
 *
 * The measured rate is the number that matters: pitch is wrong exactly when
 * this disagrees with the header's rate. It is timed off SysTick, which is
 * ch32fun's free-running counter at HCLK/8 and is independent of TIM2 — so a
 * TIM2 misconfiguration cannot hide by skewing its own measurement.
 */
static void play_range(uint32_t len, uint32_t rate)
{
    spiflash_wake();
    amp_enable(1);
    Delay_Ms(5);                    /* let the amp's own start-up settle */

    audio_start(FLASH_DATA_ADDR, len, hdr.format, rate);

    if (!audio_busy())
        LOG("audio_start refused: fmt %u rate %lu len %lu\n",
            (unsigned)hdr.format, (unsigned long)rate,
            (unsigned long)len);

    /* Bounded wait. If the sample interrupt never fires, audio_busy() stays 1
     * forever and this loop would spin with the amp enabled — silence plus
     * hiss, and no clue why. Give it the chant's own length plus 3 s, then
     * bail and say so. SysTick is ch32fun's free-running timebase; audio no
     * longer touches it, which is exactly why this is safe to read here. */
    uint32_t start  = SysTick->CNT;
    uint32_t budget = ((len / rate) * 1000u + 3000u) * DELAY_MS_TIME;
    int      stalled = 0;

    while (audio_busy()) {
        audio_service();            /* refill whichever buffer the ISR spent */
        if ((uint32_t)(SysTick->CNT - start) > budget) {
            stalled = 1;
            break;
        }
    }

    uint32_t elapsed_ms = (uint32_t)(SysTick->CNT - start) / DELAY_MS_TIME;

    audio_stop();                   /* ramps duty back to mid-scale */
    Delay_Ms(2);                    /* let the ramp reach the amp before mute */

    /* Order matters on the way down: timer, then amp, then flash. */
    amp_enable(0);
    spiflash_deep_power_down();

    /* All printing happens here, after the amp is muted — printf blocks for
     * milliseconds and the sample clock will not wait for it. */
    if (stalled)
        LOG("playback stalled — sample interrupt is not running\n");

    uint32_t got = audio_played();

    LOG("played %lu of %lu samples in %lu ms",
        (unsigned long)got, (unsigned long)len, (unsigned long)elapsed_ms);
    if (elapsed_ms)
        LOG(" = %lu Hz (asked for %lu)",
            (unsigned long)((got * 1000u) / elapsed_ms), (unsigned long)rate);
    LOG("\n");
}

static void play_chant(void)
{
    play_range(hdr.length, hdr.rate);
}

/*
 * Bring-up only. Set PLAYER_SELFTEST to 1 to get the 1 kHz tone and the rate
 * sweep back at boot; both are off in the normal player.
 *
 * The player takes its sample rate from the flash header, and send_chant.py
 * writes whatever --rate says (default 8000) without ever looking at the file.
 * So a chant converted at some other rate loads happily and plays at the wrong
 * speed, which is what "slow and low-pitched" was. Reloading 450 KB over the
 * UART to test each guess costs 80 seconds a try; playing the same bytes back
 * at each candidate rate costs two seconds a try and needs no reload at all.
 */
#if PLAYER_SELFTEST
static void rate_sweep(void)
{
    static const uint32_t candidates[] = { 22050u, 32000u, 44100u, 48000u };

    LOG("rate sweep — 2 s of the chant at each candidate; say which sounds right\n");
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        LOG("  [%u] %lu Hz\n", i + 1u, (unsigned long)candidates[i]);
        play_range(2u * candidates[i], candidates[i]);
        Delay_Ms(600);              /* a gap, so the four are tellable apart */
    }
}
#endif

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

    /* Read back a slice of the audio itself. The header being right only proves
     * the first 16 bytes landed. Real speech centres near 128 and swings wide;
     * min == max means the data is flat (0xFF = never written) and the flash
     * would play as silence no matter how well the rest of this works. */
    if (have_chant) {
        uint8_t s[64];
        uint8_t lo = 255, hi = 0;

        spiflash_read(FLASH_DATA_ADDR + 8000u, s, sizeof(s));   /* 1 s in */
        for (unsigned i = 0; i < sizeof(s); i++) {
            if (s[i] < lo) lo = s[i];
            if (s[i] > hi) hi = s[i];
        }
        LOG("samples at 1 s: %02x %02x %02x %02x %02x %02x %02x %02x  min %u max %u\n",
            s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
            (unsigned)lo, (unsigned)hi);
    }

    spiflash_deep_power_down();

#if PLAYER_SELFTEST
    /* Halving test: PWM -> filter -> amp -> speaker, with no flash and no
     * interrupt in the path. Audible tone = that whole chain is good, so a
     * silent chant is upstream of it. */
    LOG("test tone: 1 kHz for 500 ms\n");
    amp_enable(1);
    Delay_Ms(5);
    audio_test_tone(1000u, 500u);
    amp_enable(0);

    if (have_chant)
        rate_sweep();
#endif

    LOG("switch idle level: %s (expect HIGH — it is pulled up and pressed = LOW)\n",
        switch_pressed() ? "LOW/pressed" : "HIGH/released");
    LOG("ready — press the switch\n");

    for (;;) {
        if (debounced_press()) {
            LOG("press\n");
            if (have_chant)
                play_chant();
            else
                LOG("  ignored: no playable chant in flash\n");

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
