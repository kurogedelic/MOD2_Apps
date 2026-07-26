/* HAGIWO MOD2 / Sampler v1.0  (Seeed XIAO RP2350)
 *
 * Record a sound, then fire it back with a gate.
 *
 * Recording arms rather than starts. Hold the button and the LED blinks, waiting; the
 * take begins the moment something loud enough arrives, so the attack lands at the head
 * of the sample instead of behind a stretch of silence. Press again to end it. There is
 * no erase, because arming again is all it takes to replace what is there.
 *
 *   POT1   : pitch, quarter speed to four times, which moves length with it
 *   POT2   : start point, anywhere in the first 90% of the sample
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : trigger, plays from the start point
 *   IN2    : same, but plays backwards
 *   BUTTON : click to play. Hold past 1.2s to arm; click again to end the take, or to
 *            back out while still waiting.
 *   LED    : blinks while armed, follows the input while recording, lit during playback
 *
 * Requirements: same as the other apps here. The Eurorack +/-12V rail is mandatory, the
 * rear jumper goes to the MCU side, and BIAS below must be calibrated per unit.
 *
 * MIT License - Copyright (c) 2026 Leo Kuroshita
 */

#include <math.h>
#include <hardware/adc.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/gpio.h>

#define SL_AUD 0          // GPIO1 = slice0 channel B
#define SL_TIK 1          // timebase only, not routed to a pin
#define SL_LED 2          // GPIO5 = slice2 channel B
#define BIAS   1937.0f    // measured no-signal ADC center, calibrate per unit
#define FS     48828.1f   // sample rate (150MHz / 3072)
#define MAXLEN 180000     // 360KB of SRAM, 3.7 seconds
#define MINLEN 1000       // anything shorter than this was a slip, not a take
#define FADE   96.0f      // samples of fade at each end, enough to kill the edge click
#define OUTLVL 0.5f
#define DRY    0.5f       // live input passes through, so you can hear what you are doing

/* How loud the input has to get before an armed sampler starts recording. The idle noise
 * floor sits near 0.01 of full scale, so this leaves room without needing a hard hit.
 */
#define THRESH 0.04f

#define IDLE    0
#define STANDBY 1
#define RECORD  2

// --- the sample ---
static int16_t samp[MAXLEN];
static volatile uint32_t sampLen = 0;
static volatile int state = IDLE;
static uint32_t recIdx = 0;
static float playPos = 0.0f;
static bool  playing = false;
static int   playDir = 1;

// --- DSP state ---
static float dcx, dcy, e1, e2;
// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float pitch = 1.0f, startFrac = 0.0f;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool armFwd = false, armRev = false, armArm = false, armEnd = false;
static volatile float inLevel = 0.0f;
static volatile bool playMon = false;

/* The LED runs on the same 146.5kHz carrier as the audio output, and for the same reason.
 * At exactly three times the sample rate its switching folds to DC when the ADC picks it
 * up off the shared 3.3V rail. This does not make the LED silent: its current still moves
 * the rail, and a plain on/off blink is audible on this board. It only keeps the switching
 * itself from landing somewhere you can hear, which 100kHz did not, folding back to 2.3kHz.
 */
static inline void ledSet(int v) {                 // 0..255
  if (v < 0) v = 0; else if (v > 255) v = 255;
  pwm_set_gpio_level(5, (uint16_t)(v << 2));
}

static inline uint16_t adc_oneshot(uint ch) {
  adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) | (ch << ADC_CS_AINSEL_LSB);
  adc_hw->cs |= ADC_CS_START_ONCE_BITS;
  while (!(adc_hw->cs & ADC_CS_READY_BITS)) tight_loop_contents();
  return (uint16_t)adc_hw->result;
}

static void __not_in_flash_func(isr)(void) {
  pwm_hw->intr = 1u << SL_TIK;
  int raw = adc_hw->result;                        // audio sample requested last time

  static uint32_t tick = 0;
  if ((++tick & 0xFF) == 0) {                      // steal an ADC slot every 256 samples
    adc_oneshot(0); pot1raw = adc_oneshot(0);      // discard one, then keep (ch0 = POT1)
    adc_oneshot(1); pot2raw = adc_oneshot(1);      // the throwaway drains charge left by
    adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) // the previous channel
               | (2u << ADC_CS_AINSEL_LSB);
  }
  hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS); // queue the next audio sample

  float d = BIAS - (float)raw;                     // the analog front end inverts
  dcy = d - dcx + 0.9995f * dcy; dcx = d;          // DC block, ~4Hz corner
  float in = dcy * (1.0f / 2048.0f);               // normalise to roughly +/-1

  float mag = in < 0.0f ? -in : in;                // peak follower, for the LED
  if (mag > inLevel) inLevel = mag; else inLevel *= 0.9993f;

  // --- commands handed over by loop() ---
  if (armArm) { armArm = false; state = STANDBY; playing = false; }
  if (armEnd) {
    armEnd = false;
    if (state == RECORD) sampLen = (recIdx >= MINLEN) ? recIdx : 0;
    state = IDLE;                                  // from STANDBY this just backs out
  }
  if ((armFwd || armRev) && state == IDLE && sampLen) {
    uint32_t s = (uint32_t)(startFrac * (float)sampLen);
    playPos = armRev ? (float)(sampLen - 1) : (float)s;
    playDir = armRev ? -1 : 1;
    playing = true;
  }
  armFwd = armRev = false;

  // --- arm and record ---
  // Waiting, not running. The take starts on the transient, so nothing has to be trimmed
  // off the front afterwards.
  if (state == STANDBY && mag > THRESH) { state = RECORD; recIdx = 0; }
  if (state == RECORD) {
    int q = (int)(in * 32767.0f);
    if (q >  32767) q =  32767;
    if (q < -32768) q = -32768;
    samp[recIdx++] = (int16_t)q;
    if (recIdx >= MAXLEN) { sampLen = MAXLEN; state = IDLE; }
  }

  // --- play head ---
  float voice = 0.0f;
  if (playing) {
    uint32_t len = sampLen;
    int   i0 = (int)playPos;
    int   i1 = i0 + 1; if ((uint32_t)i1 >= len) i1 = i0;
    float fr = playPos - (float)i0;
    voice = ((float)samp[i0] + ((float)samp[i1] - (float)samp[i0]) * fr) * (1.0f / 32767.0f);

    // Ease in and out. Starting mid-waveform is a step edge, and a click on every hit is
    // the fastest way to make a sampler sound cheap.
    float toEnd   = (playDir > 0) ? (float)(len - 1) - playPos : playPos;
    float fromTop = (playDir > 0) ? playPos - (float)((uint32_t)(startFrac * (float)len))
                                  : (float)(len - 1) - playPos;
    float g = 1.0f;
    if (fromTop < FADE) g  = fromTop * (1.0f / FADE);
    if (toEnd   < FADE) g *= toEnd   * (1.0f / FADE);
    if (g < 0.0f) g = 0.0f;
    voice *= g;

    playPos += pitch * (float)playDir;
    if (playPos >= (float)(len - 1) || playPos < 0.0f) playing = false;
  }
  playMon = playing;

  float out = voice + in * DRY;

  float t = 512.0f + out * 512.0f * OUTLVL + 1.5f * e1 - 0.5f * e2;  // 2nd-order error FB
  int q = (int)(t + 0.5f);
  if (q < 0) q = 0; else if (q > 1023) q = 1023;
  e2 = e1; e1 = t - (float)q;
  pwm_hw->slice[SL_AUD].cc = (uint32_t)q << 16;
}

void setup() {
  set_sys_clock_khz(150000, true);
  Serial.begin(115200);
  // Run the ADC off PLL_SYS, not the default PLL_USB: separate clock sources drift against
  // the PWM timebase and drop or duplicate samples. 150/48 = 3.125 divides cleanly.
  clock_configure(clk_adc, 0, CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                  150000000, 48000000);
  adc_init();
  adc_gpio_init(26); adc_gpio_init(27); adc_gpio_init(28);
  adc_select_input(2);

  pinMode(6, INPUT_PULLUP);                        // button is active low
  pinMode(7, INPUT); pinMode(0, INPUT);            // gates, externally pulled
  delay(10);

  gpio_set_function(1, GPIO_FUNC_PWM);
  gpio_set_function(5, GPIO_FUNC_PWM);             // LED, see ledSet()
  pwm_set_wrap(SL_AUD, 1023);                      // 146.5kHz carrier
  pwm_set_wrap(SL_LED, 1023);                      // the LED shares it, locked to Fs
  pwm_set_wrap(SL_TIK, 3071);                      // 48.83kHz ISR
  pwm_set_mask_enabled((1u << SL_AUD) | (1u << SL_TIK) | (1u << SL_LED));
  pwm_hw->intr = 1u << SL_TIK;
  pwm_hw->inte |= 1u << SL_TIK;
  irq_set_exclusive_handler(PWM_IRQ_WRAP_0, isr);
  irq_set_enabled(PWM_IRQ_WRAP_0, true);
  hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);
}

static int btnRel = 1, gateIdle = 0;

void loop() {
  static int n = 0;
  static bool init = false;
  if (!init) { init = true; btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);
  pitch     = 0.25f * powf(16.0f, x1);            // 0.25x .. 4x, log
  startFrac = x2 * 0.9f;                          // never right at the end

  bool g1  = (digitalRead(7) != gateIdle);
  bool g2  = (digitalRead(0) != gateIdle);
  bool btn = (digitalRead(6) != btnRel);

  // --- button ---
  static bool wasBtn = false;
  static int  heldMs = 0;
  static bool consumed = false;
  if (btn && !wasBtn) {                            // press
    heldMs = 0; consumed = false;
    if (state == RECORD || state == STANDBY) {     // ends the take, or backs out of arming
      armEnd = true; consumed = true;
    } else {
      armFwd = true;                               // idle, so it plays
    }
  }
  if (btn) {
    heldMs += 2;
    if (heldMs > 1200 && !consumed && state == IDLE) {
      consumed = true;
      armArm  = true;                              // hold arms the next take
      Serial.println(">> ARMED, waiting for signal");
    }
  }
  wasBtn = btn;

  // --- gates play, and are ignored while a take is in progress ---
  static bool prevF = false, prevR = false;
  if (g1 && !prevF) armFwd = true;
  if (g2 && !prevR) armRev = true;
  prevF = g1; prevR = g2;

  // --- LED ---
  if (state == STANDBY) {
    ledSet((n & 64) ? 255 : 0);        // blink while waiting
  } else if (state == RECORD) {
    float l = inLevel * 3.0f; if (l > 1.0f) l = 1.0f;
    ledSet((int)(l * 255.0f));             // follow what is going down
  } else {
    ledSet(playMon ? 255 : 0);
  }

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    const char *st = (state == RECORD) ? "REC " : (state == STANDBY ? "ARM " : "IDLE");
    Serial.printf("[%s] len=%5.2fs pitch=%4.2f start=%3d%% lvl=%4.2f %s\n",
                  st, (double)((float)(state == RECORD ? recIdx : sampLen) / FS),
                  (double)pitch, (int)(startFrac * 100.0f), (double)inLevel,
                  playMon ? "*" : "");
  }
  delay(2);
}
