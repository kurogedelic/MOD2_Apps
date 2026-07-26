/* HAGIWO MOD2 / Compressor v1.0  (Seeed XIAO RP2350)
 *
 * A feed-forward compressor with a gate sidechain.
 *
 * There is only one audio input on this module, so a conventional sidechain is off the
 * table. A gate sidechain is not: send the same trigger that fires your kick into IN1 and
 * the compressor ducks on it, which is the thing people actually reach for a sidechain to
 * do. The constraint picks the useful half.
 *
 *   POT1   : threshold, -40dB to 0
 *   POT2   : ratio, 1:1 through to limiting
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : sidechain trigger. Ducks on the gate, with the same timing as the detector.
 *   IN2    : bypass while held, for comparing
 *   BUTTON : cycles the timing, fast / medium / slow
 *   LED    : gain reduction, brighter as it works harder
 *
 * Makeup gain is applied automatically from the threshold and ratio, so moving the
 * threshold changes how it sounds rather than how loud it is.
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
#define DB     6.0205999f // 20*log10(x) is this times log2(x)

// --- DSP state ---
static float dcx, dcy, e1, e2;
static float det = 0.0f;            // peak detector
static float gr  = 1.0f;            // current gain, smoothed
static float sc  = 0.0f;            // sidechain envelope

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float threshDb = -20.0f, slope = 0.5f, makeup = 1.0f;
static volatile float atkC = 0.02f, relC = 0.0005f, detRel = 0.0005f;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool bypass = false, scTrig = false;
static volatile float grMon = 1.0f;

static inline uint16_t adc_oneshot(uint ch) {
  adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) | (ch << ADC_CS_AINSEL_LSB);
  adc_hw->cs |= ADC_CS_START_ONCE_BITS;
  while (!(adc_hw->cs & ADC_CS_READY_BITS)) tight_loop_contents();
  return (uint16_t)adc_hw->result;
}

static inline void ledSet(int v) {                 // 0..255, see the README on LED noise
  if (v < 0) v = 0; else if (v > 255) v = 255;
  pwm_set_gpio_level(5, (uint16_t)(v << 2));
}

/* A compressor's gain law lives in decibels, and logf and expf cannot be called from the
 * interrupt. These read the exponent straight out of the float and interpolate the
 * mantissa linearly: a couple of percent off, which is a fraction of a dB, and nowhere
 * near enough to hear on a gain curve.
 */
static inline float fastLog2(float x) {
  union { float f; uint32_t i; } v; v.f = x;
  return (float)v.i * 1.1920929e-7f - 126.94269504f;
}
static inline float fastExp2(float x) {
  if (x < -126.0f) x = -126.0f;
  if (x >   126.0f) x =  126.0f;
  union { uint32_t i; float f; } v;
  v.i = (uint32_t)((x + 126.94269504f) * 8388608.0f);
  return v.f;
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

  // --- detector ---
  // Peak, catching instantly and letting go slowly. Attack is applied to the gain rather
  // than here, so a transient is measured properly before anything is done about it.
  float a = in < 0.0f ? -in : in;
  if (a > det) det = a; else det += (a - det) * detRel;

  if (scTrig) { scTrig = false; sc = 1.0f; }
  sc += (0.0f - sc) * detRel;                      // the gate sidechain decays the same way
  float key = det > sc ? det : sc;

  // --- gain computer ---
  // Above the threshold, every dB in becomes 1/ratio dB out, so the reduction is the
  // amount over the threshold times how much of it is being given away.
  float g = 1.0f;
  if (key > 1e-6f) {
    float lvlDb = DB * fastLog2(key);
    float over  = lvlDb - threshDb;
    if (over > 0.0f) g = fastExp2((-over * slope) * (1.0f / DB));
  }

  // --- attack and release ---
  // Clamping down is fast, letting go is slow. Doing it the other way round is what makes
  // a compressor breathe audibly on every note.
  gr += (g - gr) * (g < gr ? atkC : relC);
  grMon = gr;

  float out = bypass ? in : in * gr * makeup;

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

// attack ms, release ms, detector release ms
static const float TIMES[3][3] = {
  {  1.0f,  60.0f,  20.0f },   // fast, pumps
  {  8.0f, 160.0f,  60.0f },   // medium
  { 30.0f, 500.0f, 150.0f },   // slow, mostly levelling
};
static const char *TIMENAME[3] = { "FAST", "MED", "SLOW" };

void loop() {
  static int n = 0, tSel = 1;
  static bool init = false;
  if (!init) { init = true; delay(10); btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  threshDb = -40.0f + 40.0f * x1;                  // -40dB .. 0
  float ratio = 1.0f + 19.0f * x2 * x2;            // 1:1 .. 20:1, squared for feel
  slope = 1.0f - 1.0f / ratio;
  // Put back what the threshold takes away, so the knob changes character rather than
  // level and A/B against bypass stays honest.
  makeup = powf(10.0f, (-threshDb * slope) / 20.0f);
  if (makeup > 8.0f) makeup = 8.0f;

  atkC   = 1.0f - expf(-1.0f / (0.001f * TIMES[tSel][0] * FS));
  relC   = 1.0f - expf(-1.0f / (0.001f * TIMES[tSel][1] * FS));
  detRel = 1.0f - expf(-1.0f / (0.001f * TIMES[tSel][2] * FS));

  static bool prevG1 = false;
  bool g1 = (digitalRead(7) != gateIdle);
  if (g1 && !prevG1) scTrig = true;                // IN1 ducks it
  prevG1 = g1;
  bypass = (digitalRead(0) != gateIdle);

  static bool wasBtn = false;
  bool btn = (digitalRead(6) != btnRel);
  if (btn && !wasBtn) {
    tSel = (tSel + 1) % 3;
    Serial.printf(">> %s\n", TIMENAME[tSel]);
  }
  wasBtn = btn;

  float red = 1.0f - grMon;                        // brighter the harder it is working
  if (red < 0.0f) red = 0.0f; if (red > 1.0f) red = 1.0f;
  ledSet((int)(red * 255.0f));

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    Serial.printf("[%-4s] thresh=%+5.1fdB ratio=%4.1f:1 gr=%+5.1fdB makeup=%4.1f %s\n",
                  TIMENAME[tSel], (double)threshDb, (double)ratio,
                  (double)(20.0f * log10f(grMon < 1e-4f ? 1e-4f : grMon)),
                  (double)makeup, bypass ? "BYPASS" : "");
  }
  delay(2);
}
