/*
 * main.c — chant clicker player firmware (SKELETON — nothing implemented yet).
 *
 * Sequence, per docs/firmware-plan.md:
 *   standby -> press -> debounce -> wake flash -> amp on -> stream chant ->
 *   ramp out -> amp off -> flash deep power-down -> standby
 *
 * Build it in the order of docs/bring-up.md; do not write all of this at once.
 */

#include "ch32v003fun.h"      /* TODO: vendored or submodule — see Makefile */
#include "board.h"
#include "spiflash.h"
#include "audio.h"

/* ------------------------------------------------------------------------- */

static void board_init(void)
{
    /* TODO step 1: clocks, then GPIO:
     *  - PIN_AUDIO      : alternate function push-pull (TIM1 CH1)
     *  - PIN_SW         : input with internal pull-up, EXTI falling edge
     *  - PIN_AMP_EN     : output push-pull, driven LOW at reset
     *  - PIN_FLASH_CS   : output push-pull, idle HIGH
     *  - SCK/MOSI       : alternate function push-pull; MISO input floating
     * Drive AMP_EN low before anything else so the amp cannot hiss during init.
     */
}

/* Returns 1 if the press is real after SW_DEBOUNCE_MS. */
static int debounce_press(void)
{
    /* TODO step 2: delay SW_DEBOUNCE_MS, re-read PIN_SW, return (still low). */
    return 0;
}

static void play_chant(void)
{
    /* TODO steps 5-6:
     *   spiflash_wake();
     *   read the 16-byte header at 0x000000, check magic "CHNT", get length+format
     *   amp_enable(1); delay a few ms for the amp to settle
     *   audio_start(offset, length, format);   // primes both buffers, starts TIM1
     *   while (audio_busy()) { audio_service(); }   // refills the idle buffer
     *   audio_stop();                          // ramps duty back to AUDIO_IDLE_DUTY
     *   amp_enable(0);
     *   spiflash_deep_power_down();
     *
     * Order matters on the way down: timer, then amp, then flash, then sleep.
     */
}

static void enter_standby(void)
{
    /* TODO step 7: arm EXTI on PIN_SW, then MCU standby (~9 uA).
     * The flash must already be in deep power-down (B9h) or the sleep budget
     * is blown by tens of uA of idle flash current. */
}

int main(void)
{
    SystemInit();
    board_init();
    spiflash_init();
    audio_init();

    spiflash_deep_power_down();   /* start from the low-power state */

    for (;;) {
        enter_standby();          /* returns after the PD3 interrupt wakes us */

        if (!debounce_press())
            continue;             /* bounce or noise — back to sleep */

        play_chant();

        /* Open question (docs/decisions.md): presses during playback are
         * currently ignored, because play_chant() blocks. If restart-on-press
         * is wanted, poll PIN_SW inside the service loop instead. */
    }
}
