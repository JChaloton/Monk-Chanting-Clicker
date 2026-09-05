/*
 * audio.h — PWM playback with a double buffer. SKELETON.
 *
 * TIM1 CH1 on PD2 runs an 8-bit PWM at 187.5 kHz; a sample tick at 8 kHz writes
 * the next byte into the compare register. Samples stream from the SPI flash
 * through two 256-byte buffers: the ISR plays one while the main loop refills
 * the other. See docs/firmware-plan.md.
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

void audio_init(void);

/* Prime both buffers from flash and start the timer. `format` is one of
 * FLASH_FMT_PCM8 / FLASH_FMT_ADPCM4. Ramps duty from AUDIO_IDLE_DUTY. */
void audio_start(uint32_t flash_addr, uint32_t len, uint8_t format);

/* Non-zero while a chant is playing. */
int  audio_busy(void);

/* Call from the main loop as often as possible while busy: refills whichever
 * buffer the ISR has finished with. Must never be called from the ISR. */
void audio_service(void);

/* Ramp duty back to AUDIO_IDLE_DUTY and stop the timer (anti-pop). */
void audio_stop(void);

#endif /* AUDIO_H */
