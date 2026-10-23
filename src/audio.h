/*
 * audio.h — PWM playback with a double buffer.
 *
 * TIM1 CH1 on PD2 runs an 8-bit PWM at 187.5 kHz; a SysTick interrupt at the
 * sample rate writes the next byte into the compare register. Samples stream
 * from the SPI flash through two 256-byte buffers: the ISR plays one while the
 * main loop refills the other. See docs/firmware-plan.md.
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* Sets up TIM1 CH1 and parks the duty at AUDIO_IDLE_DUTY. Safe to call with the
 * amp still muted — that is the intended order. */
void audio_init(void);

/* Prime both buffers from flash and start the sample clock. `format` must be
 * FLASH_FMT_PCM8; anything else is refused and audio_busy() stays 0.
 * `rate` comes from the flash header, so re-recording at a different rate needs
 * no firmware change. Ramps the duty from AUDIO_IDLE_DUTY to the first sample. */
void audio_start(uint32_t flash_addr, uint32_t len, uint8_t format, uint32_t rate);

/* Non-zero while a chant is playing. */
int  audio_busy(void);

/* Call from the main loop as often as possible while busy: refills whichever
 * buffer the ISR has finished with. Must never be called from an ISR — it talks
 * to SPI, and the ISR must not. */
void audio_service(void);

/* Ramp duty back to AUDIO_IDLE_DUTY and stop the sample clock (anti-pop). */
void audio_stop(void);

/* ---- bring-up diagnostics ------------------------------------------------ */

/* Samples handed to the PWM since the last audio_start(). Zero after a play
 * means the sample interrupt never fired; short of the chant's length means it
 * underran. Costs one increment in the ISR. */
uint32_t audio_played(void);

/* Square wave straight out of TIM1, no flash and no interrupt involved: a
 * busy-wait loop toggling the duty either side of AUDIO_IDLE_DUTY. Call it
 * with the amp already enabled. This is the halving test for "no sound" — if
 * the tone is audible the PWM, filter, amp and speaker are all fine and the
 * fault is upstream in the samples or the sample clock; if it is silent the
 * fault is in that analog chain and no amount of firmware will help. */
void audio_test_tone(uint32_t hz, uint32_t ms);

#endif /* AUDIO_H */
