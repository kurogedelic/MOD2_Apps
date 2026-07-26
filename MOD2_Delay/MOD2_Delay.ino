/* HAGIWO MOD2 / Delay v1.0  (Seeed XIAO RP2350)
 *
 * A delay line with feedback, and the one thing a delay in a rack really wants: it can
 * lock to a clock. Hold the button to hand IN1 over to a clock signal, and the time knob
 * stops setting milliseconds and starts picking a ratio against it, dotted and triplet
 * divisions included.
 *
 *   POT1   : free running, delay time from 20ms to 1.3s.
 *            clocked, the ratio against the incoming clock.
 *   POT2   : feedback, reaching just past unity
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : clock, when clock mode is on
 *   IN2    : freeze. Feedback to unity with the input cut.
 *   BUTTON : tap tempo. Hold past 1.2s to switch between free and clocked; the LED blinks
 *            once for free and twice for clocked.
 *   LED    : output level
 *
 * Turning the time knob slews the read head rather than jumping it, so the pitch bends on
 * the way like a tape or bucket brigade delay. Repeats also lose a little top each pass.
 * Neither is strictly necessary and both are why the thing sounds like a delay rather than
 * a buffer.
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
#define OUTLVL 0.5f

#define MAXDLY 65536      // 1.34 seconds, 262KB as floats
#define MINDLY 977        // 20ms
#define SLEW   0.00008f   // how fast the read head chases a new time
#define DAMP   0.30f      // top end lost on each pass round the loop
#define DRY    0.7f
#define WET    0.7f

static float buf[MAXDLY];
static uint32_t wr = 0;
static float curDly = 12000.0f;

// --- DSP state ---
static float dcx, dcy, e1, e2, fbLp = 0.0f;

// --- clock measurement, done in the ISR so the period is sample accurate ---
static volatile uint32_t sampCnt = 0;
static volatile uint32_t clkPeriod = 0;   // 0 until two edges have been seen
static volatile bool clkSeen = false;

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float tgtDly = 12000.0f, feedback = 0.4f;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool frozen = false, clkMode = false;
static volatile float outMon = 0.0f;

/* The LED runs on the same 146.5kHz carrier as the audio output, and for the same reason.
 * At exactly three times the sample rate its switching folds to DC when the ADC picks it
 * up off the shared 3.3V rail, where the DC blocker removes it. analogWrite defaults to
 * 1kHz, which is simply audible; raising it to 100kHz only moved the problem, since
 * anything above Nyquist folds back and 100kHz lands at 2.3kHz and whines.
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
  sampCnt++;

  static uint32_t tick = 0;
  if ((++tick & 0xFF) == 0) {                      // steal an ADC slot every 256 samples
    adc_oneshot(0); pot1raw = adc_oneshot(0);      // discard one, then keep (ch0 = POT1)
    adc_oneshot(1); pot2raw = adc_oneshot(1);      // the throwaway drains charge left by
    adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) // the previous channel
               | (2u << ADC_CS_AINSEL_LSB);
  }
  hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS); // queue the next audio sample

  // --- clock in ---
  // Timed here rather than in loop(), because loop() runs every 2ms and a delay locked to
  // a clock measured that coarsely drifts audibly against everything else in the rack.
  static bool prevClk = false;
  static uint32_t lastEdge = 0;
  bool clk = gpio_get(7);
  if (clkMode && clk && !prevClk) {
    uint32_t now = sampCnt;
    uint32_t dt = now - lastEdge;
    if (dt > 480 && dt < MAXDLY * 4) { clkPeriod = dt; clkSeen = true; }
    lastEdge = now;
  }
  prevClk = clk;

  float d = BIAS - (float)raw;                     // the analog front end inverts
  dcy = d - dcx + 0.9995f * dcy; dcx = d;          // DC block, ~4Hz corner
  float in = dcy * (1.0f / 2048.0f);               // normalise to roughly +/-1

  // Chase the target rather than jumping to it. A delay line whose length changes
  // instantly clicks; one that slides bends pitch on the way, which is what every analog
  // delay does and what people expect when they grab the time knob.
  curDly += (tgtDly - curDly) * SLEW;

  float rd = (float)wr - curDly;
  while (rd < 0.0f) rd += (float)MAXDLY;
  int   i0 = (int)rd;
  int   i1 = i0 + 1; if (i1 >= MAXDLY) i1 = 0;
  float fr = rd - (float)i0;
  float dl = buf[i0] + (buf[i1] - buf[i0]) * fr;

  fbLp += (dl - fbLp) * (1.0f - DAMP);             // repeats darken as they go round
  float src = frozen ? 0.0f : in;
  float w = src + fbLp * feedback;
  if (w >  1.6f) w =  1.6f;                        // the loop needs a ceiling to sit under
  if (w < -1.6f) w = -1.6f;
  buf[wr] = w;
  wr = (wr + 1 >= MAXDLY) ? 0 : wr + 1;

  float out = in * DRY + dl * WET;
  outMon = out < 0.0f ? -out : out;

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

  for (int i = 0; i < MAXDLY; i++) buf[i] = 0.0f;

  pinMode(6, INPUT_PULLUP);                        // button is active low
  pinMode(7, INPUT); pinMode(0, INPUT);            // gates, externally pulled

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

/* Ratios against the incoming clock. Triplets and dotted values are in here because a
 * delay that can only do straight divisions is half a delay.
 */
static const float RATIO[8]     = { 0.25f, 1.0f/3.0f, 0.5f, 2.0f/3.0f, 0.75f, 1.0f, 1.5f, 2.0f };
static const char *RATIONAME[8] = { "1/4", "1/3", "1/2", "2/3", "3/4", "1", "3/2", "2" };

void loop() {
  static int n = 0, ratSel = 5;
  static bool init = false;
  static uint32_t lastTap = 0, tapPeriod = 0;
  if (!init) { init = true; delay(10); btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  frozen   = (digitalRead(0) != gateIdle);         // IN2 holds the loop
  feedback = frozen ? 1.0f : x2 * 1.02f;

  // --- button: tap, or hold to change what IN1 is for ---
  static bool wasBtn = false;
  static int  heldMs = 0;
  static bool consumed = false;
  bool btn = (digitalRead(6) != btnRel);
  if (btn) {
    if (!wasBtn) { heldMs = 0; consumed = false; }
    heldMs += 2;
    if (heldMs > 1200 && !consumed) {
      consumed = true;
      clkMode = !clkMode;
      clkSeen = false;
      for (int i = 0; i <= (clkMode ? 1 : 0); i++) {
        ledSet(255); delay(120); ledSet(0); delay(120);
      }
      Serial.printf(">> %s\n", clkMode ? "CLOCKED (IN1)" : "FREE");
    }
  } else if (wasBtn && !consumed) {
    uint32_t now = sampCnt;                        // a tap, timed against the audio clock
    uint32_t dt = now - lastTap;
    if (dt > 480 && dt < MAXDLY) tapPeriod = dt;
    lastTap = now;
  }
  wasBtn = btn;

  if (clkMode) {
    ratSel = (int)(x1 * 7.99f);                    // the knob picks a ratio, not a time
    if (clkSeen) {
      float t = (float)clkPeriod * RATIO[ratSel];
      if (t < MINDLY) t = MINDLY;
      if (t > MAXDLY - 4) t = MAXDLY - 4;
      tgtDly = t;
    }
  } else if (tapPeriod) {
    tgtDly = (float)tapPeriod;                     // a tap overrides the knob
  } else {
    tgtDly = MINDLY * powf((float)(MAXDLY - 4) / (float)MINDLY, x1);   // 20ms .. 1.3s, log
  }
  // Moving the knob takes control back from a tap.
  static float lastX1 = -1.0f;
  if (!clkMode && fabsf(x1 - lastX1) > 0.02f) { tapPeriod = 0; lastX1 = x1; }
  if (lastX1 < 0.0f) lastX1 = x1;

  float l = outMon * 2.0f; if (l > 1.0f) l = 1.0f;
  ledSet((int)(l * l * 255.0f));

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    if (clkMode)
      Serial.printf("[CLK] ratio=%-3s clock=%5.0fms delay=%5.0fms fb=%4.2f %s\n",
                    RATIONAME[ratSel], (double)(clkPeriod * 1000.0f / FS),
                    (double)(tgtDly * 1000.0f / FS), (double)feedback,
                    clkSeen ? "" : "no clock");
    else
      Serial.printf("[FREE] delay=%5.0fms fb=%4.2f %s%s\n",
                    (double)(tgtDly * 1000.0f / FS), (double)feedback,
                    tapPeriod ? "TAP " : "", frozen ? "FROZEN" : "");
  }
  delay(2);
}
