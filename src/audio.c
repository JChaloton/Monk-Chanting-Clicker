/*
 * audio.c — PWM output stage and double-buffered streaming from SPI flash.
 *
 * Two things here are worth understanding before changing anything.
 *
 * 1. The sample clock is SysTick, not TIM1. This settles the open question in
 *    docs/decisions.md. The carrier is 187.5 kHz and 187500 / 8000 = 23.4375, so
 *    no whole-number division of the TIM1 update event lands on 8 kHz. Dividing
 *    the core clock instead lands exactly: 48 000 000 / 8000 = 6000. SysTick is
 *    32-bit with hardware auto-reload, is otherwise unused, and its ISR is a
 *    load and a store. Carrier stays at exactly 187.5 kHz, sample rate at
 *    exactly the header's rate, and the only pitch error left is the
 *    oscillator's. Proved first on src/demo/led_wave_demo.c.
 *
 * 2. The ISR never touches SPI. It reads out of whichever buffer is active and
 *    hands the spent one to the main loop. A 256-byte page is 32 ms of audio and
 *    the SPI refill costs well under a millisecond, so there is enormous slack —
 *    but only if the swap discipline is kept. If you ever find yourself wanting
 *    to read flash from the ISR, that is the bug.
 */

#include "ch32fun.h"
#include "board.h"
#include "spiflash.h"
#include "audio.h"

#define CFG_OUT_AFPP_10MHZ  0x9u

/* ---- state shared with the ISR ------------------------------------------- */

static volatile uint8_t  buf[AUDIO_BUF_COUNT][AUDIO_BUF_BYTES];
static volatile uint16_t fill[AUDIO_BUF_COUNT];  /* valid bytes; 0 = empty     */
static volatile uint8_t  play_idx;               /* buffer the ISR is reading  */
static volatile uint16_t play_pos;               /* byte offset inside it      */
static volatile int8_t   refill_idx;             /* -1 = nothing to refill     */
static volatile uint8_t  running;

/* Main-loop only. */
static uint32_t src_addr;
static uint32_t src_remaining;

/* ---- output stage -------------------------------------------------------- */

/*
 * TIM1 is an advanced-control timer, so its outputs stay dead until MOE is set
 * in BDTR. Forgetting that is the classic "PWM looks configured, pin does
 * nothing" on this part.
 */
static void pwm_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD | RCC_APB2Periph_TIM1
                    | RCC_APB2Periph_AFIO;

    PIN_AUDIO_PORT->CFGLR &= ~(0xfu << (4 * PIN_AUDIO_NUM));
    PIN_AUDIO_PORT->CFGLR |=  (CFG_OUT_AFPP_10MHZ << (4 * PIN_AUDIO_NUM));

    TIM1->PSC     = 0;                          /* no prescale: full 48 MHz   */
    TIM1->ATRLR   = AUDIO_PWM_PERIOD - 1u;      /* 255 -> 8-bit, 187.5 kHz    */
    TIM1->CHCTLR1 = TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1PE;   /* PWM mode 1      */
    TIM1->CCER   |= TIM_CC1E;
    TIM1->BDTR   |= TIM_MOE;                    /* advanced timer: arm output */
    TIM1->CH1CVR  = AUDIO_IDLE_DUTY;            /* park at mid-scale, ~1.5 V  */
    TIM1->SWEVGR |= TIM_UG;                     /* latch the preloaded values */
    TIM1->CTLR1  |= TIM_ARPE | TIM_CEN;
}

/*
 * Anti-pop. The PWM idle level is 50 % duty, not 0, because the amp input is
 * capacitively coupled and only sees changes. Stepping from 128 straight to the
 * first sample (or back again at the end) is a step change of up to half the
 * rail, which the amp faithfully reproduces as a click. A few milliseconds of
 * ramp makes it inaudible.
 */
static void ramp_to(uint16_t target)
{
    const int32_t steps = 64;
    int32_t from = (int32_t)TIM1->CH1CVR;
    int32_t delta = (int32_t)target - from;

    for (int32_t i = 1; i <= steps; i++) {
        TIM1->CH1CVR = (uint16_t)(from + (delta * i) / steps);
        Delay_Us((AUDIO_RAMP_MS * 1000u) / (uint32_t)steps);
    }
    TIM1->CH1CVR = target;
}

/* ---- sample clock -------------------------------------------------------- */

static void tick_start(uint32_t rate)
{
    SysTick->CTLR = 0;
    SysTick->CNT  = 0;
    SysTick->CMP  = (FUNCONF_SYSTEM_CORE_CLOCK / rate) - 1u;
    SysTick->SR   = 0;
    /* STE | STIE | STCLK (count HCLK, not HCLK/8) | STRE (auto-reload). */
    SysTick->CTLR = 0x0f;
    /* Note the capital K — that is how the WCH header spells this enumerator.
     * If your ch32v003fun checkout spells it SysTick_IRQn, change it here. */
    NVIC_EnableIRQ(SysTick_IRQn);
}

static void tick_stop(void)
{
    SysTick->CTLR = 0;
    SysTick->SR   = 0;
}

/*
 * Sample ISR. Short on purpose: one bounds check, one store to the compare
 * register, and the buffer swap. No SPI, no arithmetic, no printf.
 */
void SysTick_Handler(void) __attribute__((interrupt));
void SysTick_Handler(void)
{
    SysTick->SR = 0;               /* clear first, or it immediately re-fires */

    if (!running)
        return;

    uint8_t i = play_idx;

    if (play_pos >= fill[i]) {
        /* A short buffer means the main loop had nothing more to give: this was
         * the tail of the chant. A full one means we simply reached its end. */
        if (fill[i] < AUDIO_BUF_BYTES) {
            running = 0;
            return;
        }
        fill[i]    = 0;
        refill_idx = (int8_t)i;    /* hand the spent buffer to the main loop */
        play_idx   = i ^ 1u;
        play_pos   = 0;
        i          = play_idx;

        /* Either the chant ended exactly on a page boundary, or the main loop
         * did not refill in time. Stop rather than replay stale bytes — an
         * underrun that loops old audio is far more confusing to debug than
         * one that goes quiet. */
        if (fill[i] == 0u) {
            running = 0;
            return;
        }
    }

    TIM1->CH1CVR = buf[i][play_pos++];
}

/* ---- api ----------------------------------------------------------------- */

void audio_init(void)
{
    running    = 0;
    refill_idx = -1;
    fill[0] = fill[1] = 0;
    pwm_init();
}

static uint32_t prime(uint8_t idx)
{
    uint32_t n = src_remaining;

    if (n > AUDIO_BUF_BYTES)
        n = AUDIO_BUF_BYTES;
    if (n) {
        spiflash_read(src_addr, (uint8_t *)buf[idx], n);
        src_addr      += n;
        src_remaining -= n;
    }
    fill[idx] = (uint16_t)n;
    return n;
}

void audio_start(uint32_t flash_addr, uint32_t len, uint8_t format, uint32_t rate)
{
    /* ADPCM would decode two samples per byte here. Not implemented — the
     * player refuses rather than plays noise. See docs/decisions.md. */
    if (format != FLASH_FMT_PCM8 || len == 0u || rate == 0u)
        return;

    running       = 0;
    play_idx      = 0;
    play_pos      = 0;
    refill_idx    = -1;
    src_addr      = flash_addr;
    src_remaining = len;

    if (prime(0) == 0u)
        return;
    prime(1);

    ramp_to(buf[0][0]);
    running = 1;
    tick_start(rate);
}

int audio_busy(void)
{
    return running;
}

void audio_service(void)
{
    int8_t i = refill_idx;

    if (i < 0)
        return;

    /* Only one buffer is ever pending at a time — the ISR is always at least a
     * whole buffer ahead of us — so there is no race worth locking against. */
    prime((uint8_t)i);
    refill_idx = -1;
}

void audio_stop(void)
{
    running = 0;
    tick_stop();
    ramp_to(AUDIO_IDLE_DUTY);
}
