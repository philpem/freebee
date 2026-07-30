#ifndef _DIALER_H
#define _DIALER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief	The 838A dialer chip -- DTMF tone generation and output routing.
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

/*
 * Control word layout.
 *
 * The low byte drives the tone generator. A zero low byte is silence -- that's
 * how the kernel stops a tone: beepoff() writes 0, and soundDTMF() writes
 * kCallProg, whose low byte is zero.
 */
#define DIALER_TONE_MASK	0x00FF	///< tone generator field
#define DIALER_TONE_DIGIT	0x000F	///< DTMF row/column select
#define DIALER_TONE_LEVEL	0x00F0	///< volume / ringer mode

/*
 * The high byte selects where the tone goes. dialer.h names whole control words
 * rather than bits, but they decompose cleanly:
 *
 *		kOffSpeaker		0x0800		base
 *		kNoHandset		0x0900		base | 0x0100
 *		kOnSpeaker		0x0A00		base | 0x0200
 *		kTTtoLine		0x0930		base | 0x0100, tone level 0x30
 *		kCallProg		0x1000
 *		kNoSpeakerPhone	0x1700
 *		kHandset		0x2900
 *		kOpenCircuit	0x4000
 *
 * soundDTMF() ORs kTTtoLine with kOnSpeaker to get 0x0B30, so 0x0100 and 0x0200
 * are independent enables rather than alternatives. phsub.c's beep records the
 * control word being changed "0xB90 to 0xA90 ... to remove potential echo
 * problem", which is exactly dropping 0x0100 -- the beep had been going to the
 * phone line as well as the speaker.
 */
#define DIALER_TO_LINE		0x0100	///< tone routed to the phone line
#define DIALER_SPEAKER		0x0200	///< speaker enabled
#define DIALER_CALL_PROG	0x1000	///< call progress monitoring
#define DIALER_HANDSET		0x2000	///< handset path
#define DIALER_OPEN_CCT		0x4000	///< kOpenCircuit / relay control

/**
 * @brief	Decoded state of the dialer.
 *
 * Kept separate from the audio output so that a future telephony or modem
 * implementation can see what the dialer is doing -- in particular a tone with
 * @p to_line set, which is a DTMF digit being dialled down the line and which
 * we currently only log.
 */
typedef struct {
	bool		active;			///< tone generator running
	int			f_low;			///< low (row) DTMF frequency in Hz, if active
	int			f_high;			///< high (column) DTMF frequency in Hz, if active
	uint8_t		level;			///< volume/ringer mode nibble, uninterpreted
	bool		to_speaker;		///< audible to the user
	bool		to_line;		///< sent down the phone line
	bool		to_handset;		///< handset path selected
	bool		call_progress;	///< call progress monitoring enabled
	bool		open_circuit;	///< circuit/relay control asserted
} DIALER_STATE;

/**
 * @brief	Initialise the dialer.
 *
 * Reads the beeper volume from the config. Opens an SDL audio device unless the
 * volume is zero; failure to open one is not fatal, the emulator just runs
 * without sound.
 */
void dialer_init(void);

/**
 * @brief	Shut down the dialer.
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

/**
 * @brief	Current decoded dialer state.
 * @return	Pointer to the live state block. Never NULL.
 */
const DIALER_STATE *dialer_get_state(void);

#endif
