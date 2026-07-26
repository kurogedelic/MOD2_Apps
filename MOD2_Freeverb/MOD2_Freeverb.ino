/* HAGIWO MOD2 / Freeverb v1.0  (Seeed XIAO RP2350)
 *
 * Jezar at Dreampoint's Freeverb, the Schroeder-Moorer reverb that ended up in half the
 * free software of the late nineties. Eight comb filters in parallel build the density,
 * four allpasses in series smear what is left, and a lowpass inside each comb's feedback
 * loop takes the top off a little more on every pass.
 *
 * Where Spring in this repo is all dispersion and character, this is the opposite
 * approach: a room rather than a piece of hardware.
 *
 *   POT1   : room size
 *   POT2   : damping, how fast the highs die away relative to the lows
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : freeze. Comb feedback goes to unity, damping to nothing and the input is cut,
 *            which is the freeze mode from the original source.
 *   IN2    : kick the tank with a burst, handy for hearing the tail on its own
 *   BUTTON : steps the wet balance, 25 / 50 / 75 / 100%
 *   LED    : follows the tail
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
#define BIAS   1937.0f    // measured no-signal ADC center, calibrate per unit
#define FS     48828.1f   // sample rate (150MHz / 3072)
#define OUTLVL 0.5f

#define NCOMB  8
#define NAP    4

/* The original tunings are quoted for 44.1kHz. Running at 48.83kHz they have to be
 * stretched by the same ratio, or the whole reverb sits a semitone or so wrong: these are
 * the stock numbers times 48828/44100. They stay mutually prime, which is the point of
 * choosing odd values like these in the first place.
 */
static const int COMBLEN[NCOMB] = { 1236, 1315, 1414, 1501, 1574, 1651, 1724, 1790 };
static const int APLEN[NAP]     = {  616,  488,  378,  249 };
#define COMBMAX 1790
#define APMAX    616

#define FIXEDGAIN 0.015f  // the input trim from the original; eight combs add up fast
#define APFB      0.5f    // Freeverb's allpasses are fixed at this
#define SCALEROOM 0.28f
#define OFFSETROOM 0.7f
#define SCALEDAMP 0.4f

// --- the tank ---
static float combBuf[NCOMB][COMBMAX];
static float combLp[NCOMB];               // the damping lowpass, one per comb
static int   combIdx[NCOMB];
static float apBuf[NAP][APMAX];
static int   apIdx[NAP];

// --- DSP state ---
static float dcx, dcy, e1, e2;
static uint32_t rng = 0x1234567;

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float feedback = 0.84f, damp1 = 0.2f, damp2 = 0.8f, wet = 0.5f;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool frozen = false;
static volatile uint32_t kick = 0;
static volatile float tailMon = 0.0f;

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

  float drive = frozen ? 0.0f : in;                // freeze cuts the input, as in the original
  if (kick) {
    kick--;
    rng = rng * 1664525u + 1013904223u;
    drive += ((float)(int32_t)rng * (1.0f / 2147483648.0f)) * 0.4f;
  }
  float src = drive * FIXEDGAIN;

  float fb = feedback, d1 = damp1, d2 = damp2;

  // --- eight combs in parallel ---
  // Each is a delay fed back on itself with a one-pole lowpass in the loop, so every trip
  // around loses a little more top than bottom. That is the whole damping control: a room
  // full of soft furnishings rather than tile.
  float out = 0.0f;
  for (int c = 0; c < NCOMB; c++) {
    int   i = combIdx[c];
    float y = combBuf[c][i];
    combLp[c] = y * d2 + combLp[c] * d1;
    combBuf[c][i] = src + combLp[c] * fb;
    combIdx[c] = (i + 1 >= COMBLEN[c]) ? 0 : i + 1;
    out += y;
  }

  // --- four allpasses in series ---
  // The combs give density but leave it grainy. These smear the result without colouring
  // the magnitude response.
  for (int a = 0; a < NAP; a++) {
    int   i = apIdx[a];
    float buf = apBuf[a][i];
    float y = buf - out;
    apBuf[a][i] = out + buf * APFB;
    apIdx[a] = (i + 1 >= APLEN[a]) ? 0 : i + 1;
    out = y;
  }

  tailMon = out < 0.0f ? -out : out;

  float mix = in * (1.0f - wet) + out * wet;

  float t = 512.0f + mix * 512.0f * OUTLVL + 1.5f * e1 - 0.5f * e2;  // 2nd-order error FB
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

  for (int c = 0; c < NCOMB; c++) {
    for (int j = 0; j < COMBMAX; j++) combBuf[c][j] = 0.0f;
    combIdx[c] = 0; combLp[c] = 0.0f;
  }
  for (int a = 0; a < NAP; a++) {
    for (int j = 0; j < APMAX; j++) apBuf[a][j] = 0.0f;
    apIdx[a] = 0;
  }

  pinMode(5, OUTPUT);                              // LED
  analogWriteFreq(100000);                         // see the README on LED noise
  pinMode(6, INPUT_PULLUP);                        // button is active low
  pinMode(7, INPUT); pinMode(0, INPUT);            // gates, externally pulled

  gpio_set_function(1, GPIO_FUNC_PWM);
  pwm_set_wrap(SL_AUD, 1023);                      // 146.5kHz carrier
  pwm_set_wrap(SL_TIK, 3071);                      // 48.83kHz ISR
  pwm_set_mask_enabled((1u << SL_AUD) | (1u << SL_TIK));
  pwm_hw->intr = 1u << SL_TIK;
  pwm_hw->inte |= 1u << SL_TIK;
  irq_set_exclusive_handler(PWM_IRQ_WRAP_0, isr);
  irq_set_enabled(PWM_IRQ_WRAP_0, true);
  hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);
}

static int btnRel = 1, gateIdle = 0;
static const float WETMIX[4] = { 0.25f, 0.5f, 0.75f, 1.0f };

void loop() {
  static int n = 0, wetSel = 1;
  static bool init = false;
  if (!init) { init = true; delay(10); btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  frozen = (digitalRead(7) != gateIdle);

  // Freeze is not a mute on the tail: comb feedback goes to unity and the damping lowpass
  // is taken out of the loop, so whatever is in the tank keeps circulating undiminished.
  if (frozen) {
    feedback = 1.0f; damp1 = 0.0f; damp2 = 1.0f;
  } else {
    feedback = x1 * SCALEROOM + OFFSETROOM;        // 0.70 .. 0.98
    damp1 = x2 * SCALEDAMP;                        // 0 .. 0.40
    damp2 = 1.0f - damp1;
  }
  wet = WETMIX[wetSel];

  static bool prevG2 = false;
  bool g2 = (digitalRead(0) != gateIdle);
  if (g2 && !prevG2) kick = 700;
  prevG2 = g2;

  static bool wasBtn = false;
  bool btn = (digitalRead(6) != btnRel);
  if (!btn && wasBtn) {
    wetSel = (wetSel + 1) & 3;
    Serial.printf(">> WET %d%%\n", (int)(WETMIX[wetSel] * 100.0f));
  }
  wasBtn = btn;

  float l = tailMon * 2.5f; if (l > 1.0f) l = 1.0f;
  analogWrite(5, (int)(l * l * 255.0f));

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    Serial.printf("room=%4.2f damp=%4.2f wet=%3d%% tail=%4.2f %s\n",
                  (double)(x1 * SCALEROOM + OFFSETROOM), (double)(x2 * SCALEDAMP),
                  (int)(wet * 100.0f), (double)tailMon, frozen ? "FROZEN" : "");
  }
  delay(2);
}
