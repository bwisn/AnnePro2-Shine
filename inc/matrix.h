#ifndef MATRIX_INCLUDED
#define MATRIX_INCLUDED

#include "light_utils.h"

/* PWM algorithm which controls the LED matrix */

/* Calculate position within the ledColors array */
#define ROWCOL2IDX(row, col) (NUM_COLUMN * (row) + (col))

/* Current matrix state */
extern led_t ledColors[KEY_COUNT];

/* Color override by main chip. */
extern led_t ledMask[KEY_COUNT];

/* Color override by main chip that stays on even after LED_OFF. */
extern led_t ledSticky[KEY_COUNT];

/* In case we switched to a new profile, the mainCallback should call the
 * profile handler initially when this flag is set to true. */
extern bool needToCallbackProfile;

/* Animations */
/* If non-zero the animation is enabled; 1 is full speed */
extern volatile uint16_t animationSkipTicks;

/* Is matrix enabled? */
extern bool matrixEnabled;

/* Animation tick counter used to slow down animations */
extern uint16_t animationTicks;

/* Forced colors by main chip */
// Flag to check if there is a foreground color currently active
extern bool foregroundColorSet;
extern uint32_t foregroundColor;

void matrixInit(void);
void matrixEnable(void);
void matrixDisable(void);

#endif
