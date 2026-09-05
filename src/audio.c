/*
 * audio.c — PWM + double-buffered streaming. SKELETON, nothing implemented.
 *
 * Bring-up order (docs/bring-up.md):
 *   stage 4: fixed 50 % duty, verify ~1.5 V after R1 and ~0.16 V at the divider
 *   stage 6: 440 Hz tone from a table, then the real chant from flash
 */

#include "ch32v003fun.h"
#include "board.h"
#include "spiflash.h"
#include "audio.h"

static volatile uint8_t  buf[AUDIO_BUF_COUNT][AUDIO_BUF_BYTES];
static volatile uint8_t  playing_idx;     /* buffer the ISR is reading      */
static volatile uint16_t play_pos;        /* byte offset inside it          */
static volatile uint8_t  refill_needed;   /* set by ISR, cleared by service */
static volatile uint8_t  running;

static uint32_t src_addr;                 /* next flash address to read     */
static uint32_t src_remaining;

/* ------------------------------------------------------------------------- */

void audio_init(void)
{
    /* TODO: TIM1 clock on, ARR = AUDIO_PWM_PERIOD - 1, no prescaler at 48 MHz
     * -> 187.5 kHz carrier. CH1 PWM mode 1, preload enabled, output disabled
     * for now. Compare register parked at AUDIO_IDLE_DUTY.
     *
     * Sample tick: 187500 / 8000 = 23.4375, so a whole-number TIM1 update
     * divisor does not land on exactly 8 kHz. Either use a second timer for the
     * tick, or accept a slightly shifted carrier. Decide and record the choice
     * in docs/decisions.md. */
}

/* Sample-rate ISR — keep it short, and never touch SPI from here. */
void audio_tick_isr(void)
{
    /* TODO:
     *   TIM1->CH1CVR = buf[playing_idx][play_pos++];
     *   if (play_pos == AUDIO_BUF_BYTES) {
     *       play_pos = 0;
     *       refill_needed = playing_idx;      // hand the spent buffer to main
     *       playing_idx ^= 1;
     *   }
     * For ADPCM, decode two samples per byte here instead. */
}

void audio_start(uint32_t flash_addr, uint32_t len, uint8_t format)
{
    /* TODO: (void)format for now — PCM8 only.
     * Fill both buffers from flash, reset indices, ramp duty from
     * AUDIO_IDLE_DUTY over AUDIO_RAMP_MS, enable the CH1 output and the tick. */
    (void)flash_addr; (void)len; (void)format;
    running = 0;
}

int audio_busy(void)
{
    return running;
}

void audio_service(void)
{
    /* TODO: if refill_needed is set, read the next AUDIO_BUF_BYTES from flash
     * into that buffer, advance src_addr/src_remaining, clear the flag.
     * A 256-byte page is 32 ms of audio and the SPI read takes well under a
     * millisecond — enormous slack. It is the swap discipline, not bandwidth,
     * that prevents stutter. Zero-pad the tail on the last partial buffer. */
}

void audio_stop(void)
{
    /* TODO: ramp the compare value back to AUDIO_IDLE_DUTY over AUDIO_RAMP_MS,
     * then disable the CH1 output and the tick. Skipping the ramp gives an
     * audible pop at the end of every chant. */
    running = 0;
}
