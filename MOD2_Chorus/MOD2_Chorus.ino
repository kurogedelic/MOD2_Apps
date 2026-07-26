/* HAGIWO MOD2 / Chorus v1.0  (Seeed XIAO RP2350)
 *
 * A modulated delay line. Vary the length of a short delay and the copy it produces drifts
 * in pitch against the original; sum the two and the interference between them is the
 * effect. Which effect depends almost entirely on how short the delay is, so the same
 * structure covers three of them.
 *
 *   CHORUS   20 to 30ms, two taps a quarter cycle apart, no feedback. Thickening.
 *   FLANGER  under 10ms with feedback, so the comb notches are close enough to hear as a
 *            sweep rather than a blur.
 *   VIBRATO  one tap, no dry signal at all. Just the pitch moving.
 *
 *   POT1   : rate
 *   POT2   : depth
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : reset the sweep, so it can be lined up with a clock
 *   IN2    : inverts the flanger feedback, which moves the notches half a comb over
 *   BUTTON : cycles chorus -> flanger -> vibrato
 *   LED    : follows the sweep
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

#define BUFLEN 4096       // 84ms, more than any of the three modes needs
#define STAB   1025       // sine table for the sweep

#define M_CHOR 0
#define M_FLAN 1
#define M_VIB  2

static float buf[BUFLEN];
static uint32_t wr = 0;
static float lfo = 0.0f;                  // sweep phase, 0..1
static float sinTab[STAB];

// --- DSP state ---
static float dcx, dcy, e1, e2, fbz = 0.0f;

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float lfoInc = 0.0002f, depth = 200.0f, base = 1200.0f;
static volatile float fbAmt = 0.0f, dryAmt = 1.0f, wetAmt = 1.0f;
static volatile int pot1raw = 0, pot2raw = 0, mode = M_CHOR;
static volatile bool twoTap = true, resetLfo = false;
static volatile float lfoMon = 0.0f;

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

static inline float sinAt(float p) {               // p in 0..1
  p -= (float)(int)p; if (p < 0.0f) p += 1.0f;
  float fi = p * (float)(STAB - 1);
  int   i  = (int)fi;
  return sinTab[i] + (sinTab[i + 1] - sinTab[i]) * (fi - (float)i);
}

static inline float tapAt(float back) {
  float rd = (float)wr - back;
  while (rd < 0.0f) rd += (float)BUFLEN;
  int   i0 = (int)rd;
  int   i1 = i0 + 1; if (i1 >= BUFLEN) i1 = 0;
  return buf[i0] + (buf[i1] - buf[i0]) * (rd - (float)i0);
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

  if (resetLfo) { resetLfo = false; lfo = 0.0f; }
  lfo += lfoInc; if (lfo >= 1.0f) lfo -= 1.0f;
  float s = sinAt(lfo);
  lfoMon = 0.5f + 0.5f * s;

  float dA = base + depth * s;
  if (dA < 2.0f) dA = 2.0f;
  float a = tapAt(dA);

  // A single moving tap shifts pitch, which is vibrato. A second tap a quarter cycle
  // behind is moving the other way at any moment, and summing the two is what makes a
  // chorus sound like more than one player rather than one detuned one.
  float wet = a;
  if (twoTap) {
    float dB = base + depth * sinAt(lfo + 0.25f);
    if (dB < 2.0f) dB = 2.0f;
    wet = 0.5f * (a + tapAt(dB));
  }

  buf[wr] = in + fbz * fbAmt;
  wr = (wr + 1 >= BUFLEN) ? 0 : wr + 1;
  fbz = a;

  float out = in * dryAmt + wet * wetAmt;

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

  for (int i = 0; i < BUFLEN; i++) buf[i] = 0.0f;
  for (int i = 0; i < STAB; i++)
    sinTab[i] = sinf(2.0f * (float)M_PI * (float)i / (float)(STAB - 1));

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
static const char *MODENAME[3] = { "CHORUS", "FLANGER", "VIBRATO" };

void loop() {
  static int n = 0;
  static bool init = false;
  if (!init) { init = true; delay(10); btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  bool inv = (digitalRead(0) != gateIdle);         // IN2 flips the feedback polarity

  switch (mode) {
    default:
    case M_CHOR:                                   // slow and wide, dry and wet together
      lfoInc = (0.08f * powf(60.0f, x1)) / FS;     // 0.08 .. 5Hz
      base   = 0.020f * FS;
      depth  = x2 * 0.006f * FS;
      fbAmt  = 0.0f;  dryAmt = 1.0f; wetAmt = 0.9f; twoTap = true;
      break;
    case M_FLAN:                                   // short, fed back, so the comb is audible
      lfoInc = (0.05f * powf(40.0f, x1)) / FS;     // 0.05 .. 2Hz
      base   = 0.0012f * FS;
      depth  = x2 * 0.0035f * FS;
      fbAmt  = inv ? -0.72f : 0.72f;
      dryAmt = 1.0f; wetAmt = 0.9f; twoTap = false;
      break;
    case M_VIB:                                    // no dry path, so only the pitch moves
      lfoInc = (0.3f * powf(30.0f, x1)) / FS;      // 0.3 .. 9Hz
      base   = 0.006f * FS;
      depth  = x2 * 0.0035f * FS;
      fbAmt  = 0.0f;  dryAmt = 0.0f; wetAmt = 1.6f; twoTap = false;
      break;
  }

  static bool prevG1 = false;
  bool g1 = (digitalRead(7) != gateIdle);
  if (g1 && !prevG1) resetLfo = true;              // IN1 lines the sweep up with a clock
  prevG1 = g1;

  static bool wasBtn = false;
  bool btn = (digitalRead(6) != btnRel);
  if (btn && !wasBtn) {
    mode = (mode + 1) % 3;
    Serial.printf(">> %s\n", MODENAME[mode]);
  }
  wasBtn = btn;

  ledSet((int)(lfoMon * lfoMon * 255.0f));

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    Serial.printf("[%-7s] rate=%5.2fHz depth=%4.1fms base=%4.1fms %s\n",
                  MODENAME[mode], (double)(lfoInc * FS),
                  (double)(depth * 1000.0f / FS), (double)(base * 1000.0f / FS),
                  (mode == M_FLAN && inv) ? "INV" : "");
  }
  delay(2);
}
