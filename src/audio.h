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

#endif /* AUDIO_H */
