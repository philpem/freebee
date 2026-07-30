#ifndef _DIALER_H
#define _DIALER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief	Tone output from the 838A dialer chip.
 *
 * The dialer is written as an address-encoded pair of byte loads (see the
 * TM/DIALWR decode in memory.c). Per uts/kern/sys/dialer.h the resulting 16-bit
 * control word splits as "the low byte is used for tone generation, high byte
 * for the path and output control".
 *
 * This is also where the UNIX PC's system beep comes from -- the kernel has no
 * separate beeper. io/phsub.c does:
 *		beepon()  { writedialer(0xA90); }		// kOnSpeaker | 0x90
 *		beepoff() { writedialer(0); }
 * so BEL, DTMF dialling and call-progress audio all arrive through here.
 */

/**
 * @brief	Initialise the dialer's tone output.
 *
 * Opens an SDL audio device. Failure is not fatal -- the emulator simply runs
 * without sound.
 */
void dialer_init(void);

/**
 * @brief	Shut down the dialer's tone output.
 */
void dialer_done(void);

/**
 * @brief	Handle a completed write to the dialer control word.
 * @param	ctrl	The 16-bit control word which was shifted out.
 *
 * Call this once the upper byte has been loaded, which is what starts the real
 * chip shifting the word out.
 */
void dialer_write(uint16_t ctrl);

#endif
