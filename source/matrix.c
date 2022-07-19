/* Algorithm controlling the LEDs. */

#include "matrix.h"

#include "board.h"
#include "hal.h"
#include "settings.h"

/* LED Matrix state */
led_t ledColors[KEY_COUNT];
led_t ledMask[KEY_COUNT];
led_t ledSticky[KEY_COUNT];

led_t noColor = {.p.red = 0, .p.green = 0, .p.blue = 0, .p.alpha = 0};

bool needToCallbackProfile = false;
bool matrixEnabled;

/* Animations */
/* If non-zero the animation is enabled; 1 is full speed */
volatile uint16_t animationSkipTicks = 0;

/* Animation tick counter used to slow down animations */
uint16_t animationTicks = 0;

/* Internal function prototypes */
static void animationCallback(void);
static void mainCallback(GPTDriver *_driver);
static void pwmRowDimmer(void);
static void pwmNextColumn(void);

const ioline_t ledColumns[NUM_COLUMN] = {
    LINE_LED_COL_1, LINE_LED_COL_2, LINE_LED_COL_3, LINE_LED_COL_4,
    LINE_LED_COL_5, LINE_LED_COL_6, LINE_LED_COL_7, LINE_LED_COL_8,
    LINE_LED_COL_9, LINE_LED_COL_10, LINE_LED_COL_11, LINE_LED_COL_12,
    LINE_LED_COL_13, LINE_LED_COL_14};

const ioline_t ledRows[NUM_ROW * 3] = {
    LINE_LED_ROW_1_R,
    LINE_LED_ROW_1_G,
    LINE_LED_ROW_1_B,

    LINE_LED_ROW_2_R,
    LINE_LED_ROW_2_G,
    LINE_LED_ROW_2_B,

    LINE_LED_ROW_3_R,
    LINE_LED_ROW_3_G,
    LINE_LED_ROW_3_B,

    LINE_LED_ROW_4_R,
    LINE_LED_ROW_4_G,
    LINE_LED_ROW_4_B,

    LINE_LED_ROW_5_R,
    LINE_LED_ROW_5_G,
    LINE_LED_ROW_5_B,
};

static mutex_t mtx;

/*
  Original firmware reference:
  ~9.81ms column cycle measured -> 100Hz.

  During each column cycle the row is "strobed" 3 - 32 times (measured on white
  with different intensity settings). This wastes some shining time, but limits
  the current.

  The row strobing is done at around 70kHz.

  We would like to achieve more colors than the original firmware.

  80kHz with pwmCounterLimit=80 will scan 14 columns at 71.4Hz - enough for
  smooth animations, and allows for 6bit LED brightness control.

  Tests on oscilloscope suggest it can reach 100kHz.
*/
static const GPTConfig bftm0Config = {.frequency = 80000,
                                      .callback = mainCallback};

/* Currently scanned column */
static uint8_t currentColumn = 0;

/*
 * Time each row has left to shine within the current column cycle.
 * Row1 R-G-B, Row2 R-G-B, Row3 R-G-B, ...
 */
static uint8_t rowTimes[NUM_ROW * 3];

/* Number of still enabled rows in current column */
static uint8_t rowsEnabled;

/*
 * pwmCounter which counts time of lit rows within each column cycle.
 */
uint16_t pwmCounter;

/*
 * pwmCounter goes over 64 a bit to limit the current of completely white
 * board (0.5A) vs <0.3A for original firmware.
 *
 * You can get brighter LEDs if you set this to 64. And possibly burn the
 * board in the longer period of time.
 */
const uint16_t pwmCounterLimit = 80;

/* Disable timeouted LEDs */
static inline void pwmRowDimmer() {
    for (size_t ledRow = 0; ledRow < NUM_ROW * 3; ++ledRow) {
        const uint8_t time = rowTimes[ledRow];
        if (pwmCounter == time) {
            palClearLine(ledRows[ledRow]);
            rowsEnabled--;
        }
    }
    if (rowsEnabled == 0) {
        /* Limit color bleed by disabling the column as early as possible */
        palClearLine(ledColumns[currentColumn]);
    }
}

/* Start new PWM cycle */
static inline void pwmNextColumn() {
    /* Disable previously lit column */
    palClearLine(ledColumns[currentColumn]);

    currentColumn = (currentColumn + 1) % NUM_COLUMN;

    /* Prepare the PWM data and enable leds for non-zero colors */
    rowsEnabled = 0;
    for (size_t keyRow = 0; keyRow < NUM_ROW; ++keyRow) {
        const uint8_t ledIndex = ROWCOL2IDX(keyRow, currentColumn);
        led_t cl;
        /* TODO: Maybe... weight with alpha? */
        if (ledSticky[ledIndex].p.alpha) {
            cl = ledSticky[ledIndex];
        } else if (ledMask[ledIndex].p.alpha && !backlightDisabled) {
            cl = ledMask[ledIndex];
        } else if (!backlightDisabled) {
            cl = ledColors[ledIndex];
        } else {
            // user disabled backlight, but sticky keys are keeping
            // it alive, unless a sticky key exists, led should
            // be turned off
            cl = noColor;
        }

        for (size_t colorIdx = 0; colorIdx < 3; ++colorIdx) {
            const uint8_t ledRow = 3 * keyRow + colorIdx;

            /* Compute adjustments */
            uint8_t color = cl.pv[2 - colorIdx];

            uint8_t cc = color_correction.pv[2 - colorIdx];
            uint8_t ct = color_temperature.pv[2 - colorIdx];

            if (cc > 0 && ct > 0) {
                uint32_t work = (((uint32_t)cc) + 1) * (((uint32_t)ct) + 1) * 0xFF;
                work /= 0x10000L;
                uint8_t adj = work & 0xFF;
                color = (color * adj) / 0xFF;
            }

            /* >>2 to decrease the color resolution from 0-255 to 0-63 */
            color = color >> 2;
            if (color > 0) {
                /* Each led is enabled for color>0 even for a short while. */
                palSetLine(ledRows[ledRow]);
                ++rowsEnabled;
            }
            rowTimes[ledRow] = color;
        }
    }

    /* Enable the current LED column if at least one row needs this. Limit bleed
       and maybe power consumption on reactive profiles. */
    if (rowsEnabled) {
        palSetLine(ledColumns[currentColumn]);
    }
}

/*
 * Update lighting table as per animation
 */
static inline void animationCallback() {
    profiles[currentProfile].callback(ledColors);
}

/*
 * mainCallback is called by GPT timer periodically
 * and is responsible for 2 things:
 * - software PWM
 * - calling animation callback for animated profiles
 */
void mainCallback(GPTDriver *_driver) {
    (void)_driver;

    if (pwmCounter++ < pwmCounterLimit) {
        pwmRowDimmer();
        return;
    }

    /* Update profile if required before starting new cycle */
    if (!manualControl && needToCallbackProfile) {
        needToCallbackProfile = false;
        profiles[currentProfile].callback(ledColors);
    }

    /* Animation can be updated after each full column cycle. On
     * pwmCounterLimit=80 + 80kHz timer this refreshes at 80kHz/80/14 = 71Hz and
     * should be a sensible maximum speed for a fluent smooth animation.
     */
    if (!manualControl && animationSkipTicks > 0 && currentColumn == 13) {
        ++animationTicks;
        if (animationTicks >= animationSkipTicks) {
            animationTicks = 0;
            animationCallback();
        }
    }

    /* We start a new PWM column cycle. */
    pwmCounter = 0;

    pwmNextColumn();
}

/*
 * Turn off LEDs and PWM interrupt.
 */
void matrixDisable() {
    backlightDisabled = 1;
    if (stickyKeysExist) {
        return;
    }

    chMtxLock(&mtx);

    // stop timer, clock is still enabled
    if (GPTD_BFTM0.state == GPT_CONTINUOUS) {
        gptStopTimer(&GPTD_BFTM0);
    }
    // enter low power mode
    if (GPTD_BFTM0.state == GPT_READY) {
        gptStop(&GPTD_BFTM0);
    }

    palClearLine(LINE_LED_PWR);

    for (int ledRow = 0; ledRow < NUM_ROW * 3; ++ledRow) {
        palClearLine(ledRows[ledRow]);
    }
    for (int i = 0; i < NUM_COLUMN; ++i) {
        palClearLine(ledColumns[i]);
    }
    matrixEnabled = false;
    chMtxUnlock(&mtx);
}

/*
 * Turn on LED power and start PWM interrupt.
 */
void matrixEnable(void) {
    backlightDisabled = 0;
    chMtxLock(&mtx);

    palSetLine(LINE_LED_PWR);

    // start PWM handling interval
    gptStart(&GPTD_BFTM0, &bftm0Config);
    gptStartContinuous(&GPTD_BFTM0, 1);

    matrixEnabled = true;
    chMtxUnlock(&mtx);
}

/* Initialize matrix module */
void matrixInit() {
    chMtxObjectInit(&mtx);
    matrixEnabled = false;
}
