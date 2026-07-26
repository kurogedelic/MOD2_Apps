/* HAGIWO MOD2 / Autotune v1.0  (Seeed XIAO RP2350)
 *
 * Pitch correction. Work out what note is coming in, decide what note it should have been,
 * and shift the difference. Wind the speed to zero and it snaps, which is the sound
 * everyone actually wants from one of these.
 *
 * Three parts:
 *   Detection    a YIN-style difference function on a decimated copy of the input. Pitch
 *                lives in the low end, so analysing at a quarter of the sample rate loses
 *                nothing and costs a sixteenth as much.
 *   Quantising   the detected note is snapped to the nearest one allowed by the scale.
 *   Shifting     two read taps crossfading at the splice, same as Harmonizer in this repo.
 *
 *   POT1   : retune speed. Fully down is instant and robotic, up is a slow glide.
 *   POT2   : key
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : hold the current target, so the correction stops chasing
 *   IN2    : bypass while held
 *   BUTTON : cycles the scale, chromatic / major / minor / minor pentatonic
 *   LED    : lit when a pitch is being tracked, dark when the input is unvoiced
 *
 * Output is wet only. Mixing the dry signal back in would beat against the corrected one.
 *
 * Feed it something monophonic. Pitch detection on a chord has nothing to find.
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

#define BUFLEN 8192       // shifter delay line
#define WINDOW 3072.0f    // splice window. Near unity the taps barely move, so corrections
                          // of a few percent splice rarely and stay clean.
#define XFTAB  257

// --- pitch detection ---
#define DEC    4                      // decimation factor
#define DFS    (FS / (float)DEC)      // 12207 Hz
#define DECLEN 512                    // power of two, for cheap wrapping
#define WIN    256                    // samples compared
#define LAGMIN 12                     // about 1000Hz
#define LAGMAX 186                    // about 65Hz
#define CONF   0.20f                  // below this the estimate is trusted

static float buf[BUFLEN];
static uint32_t wr = 0;
static float phase = 0.0f;
static float xfTab[XFTAB];

static volatile float decBuf[DECLEN];
static volatile uint32_t decWr = 0;

// --- DSP state ---
static float dcx, dcy, e1, e2;

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float ratio = 1.0f;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool bypass = false;
static volatile bool voiced = false;

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

static inline float tapRead(float back) {
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

  // Averaging four samples is both the decimation and the anti-alias filter it needs.
  static float acc = 0.0f; static int dc = 0;
  acc += in;
  if (++dc >= DEC) {
    dc = 0;
    decBuf[decWr] = acc * (1.0f / (float)DEC);
    decWr = (decWr + 1) & (DECLEN - 1);
    acc = 0.0f;
  }

  buf[wr] = in;
  wr = (wr + 1 >= BUFLEN) ? 0 : wr + 1;

  // --- shifter ---
  float pA = phase;
  float pB = phase + 0.5f; if (pB >= 1.0f) pB -= 1.0f;
  float sA = tapRead(pA * WINDOW);
  float sB = tapRead(pB * WINDOW);
  int   jA = (int)(pA * (float)(XFTAB - 2));
  int   jB = (int)(pB * (float)(XFTAB - 2));
  float out = sA * xfTab[jA] + sB * xfTab[jB];

  phase += (1.0f - ratio) * (1.0f / WINDOW);
  if (phase >= 1.0f) phase -= 1.0f;
  else if (phase < 0.0f) phase += 1.0f;

  if (bypass) out = in;

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
  for (int i = 0; i < DECLEN; i++) decBuf[i] = 0.0f;
  for (int i = 0; i < XFTAB; i++) {
    float x = (float)i / (float)(XFTAB - 2);
    xfTab[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * x));
  }

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

/* Which of the twelve semitones the scale allows, as a bitmask relative to the key. */
static const uint16_t SCALE[4] = {
  0x0FFF,   // chromatic, every note
  0x0AB5,   // major        0 2 4 5 7 9 11
  0x05AD,   // minor        0 2 3 5 7 8 10
  0x04A9,   // minor pentatonic  0 3 5 7 10
};
static const char *SCALENAME[4] = { "CHROM", "MAJ", "MIN", "PENT" };
static const char *NOTENAME[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

/* YIN, in short. For every candidate period, sum the squared difference between the signal
 * and a copy of itself shifted by that much; a real period gives a near zero. Dividing by
 * the running mean is what stops it picking the octave below, which a plain autocorrelation
 * does constantly.
 */
static float detectPitch(float *conf) {
  static float w[WIN + LAGMAX];
  uint32_t end = decWr;
  for (int i = 0; i < WIN + LAGMAX; i++)
    w[i] = decBuf[(end + DECLEN - (WIN + LAGMAX) + i) & (DECLEN - 1)];

  float energy = 0.0f;
  for (int i = 0; i < WIN; i++) energy += w[i] * w[i];
  if (energy < 0.002f) { *conf = 1.0f; return 0.0f; }   // nothing worth analysing

  static float dv[LAGMAX + 1];
  dv[0] = 1.0f;
  float run = 0.0f;
  int   best = -1;
  float bestV = 1e9f;

  for (int lag = 1; lag <= LAGMAX; lag++) {
    float s = 0.0f;
    for (int i = 0; i < WIN; i++) { float t = w[i] - w[i + lag]; s += t * t; }
    run += s;
    dv[lag] = s * (float)lag / (run > 1e-12f ? run : 1e-12f);
    if (lag >= LAGMIN) {
      if (dv[lag] < bestV) { bestV = dv[lag]; best = lag; }
      // The first dip below the threshold wins, not the deepest. Later dips are octaves
      // of the true period and always look at least as good.
      if (dv[lag] < CONF && dv[lag] < dv[lag - 1] && lag + 1 <= LAGMAX) {
        float nxt = 0.0f;
        for (int i = 0; i < WIN; i++) { float t = w[i] - w[i + lag + 1]; nxt += t * t; }
        if (s <= nxt) { best = lag; bestV = dv[lag]; break; }
      }
    }
  }
  if (best < LAGMIN) { *conf = 1.0f; return 0.0f; }

  // Fit a parabola through the winner and its neighbours; a period is rarely a whole
  // number of samples and rounding it costs tens of cents.
  float p = (float)best;
  if (best > LAGMIN && best < LAGMAX) {
    float a = dv[best - 1], b = dv[best], c = dv[best + 1];
    float den = a - 2.0f * b + c;
    if (den > 1e-9f || den < -1e-9f) p += 0.5f * (a - c) / den;
  }
  *conf = bestV;
  return DFS / p;
}

void loop() {
  static int n = 0, scaleSel = 0, hits = 0;
  static float target = 1.0f;
  static bool init = false;
  if (!init) { init = true; delay(10); btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);
  int key = (int)(x2 * 11.99f);

  bypass = (digitalRead(0) != gateIdle);
  bool hold = (digitalRead(7) != gateIdle);

  static bool wasBtn = false;
  bool btn = (digitalRead(6) != btnRel);
  if (btn && !wasBtn) {
    scaleSel = (scaleSel + 1) & 3;
    Serial.printf(">> %s\n", SCALENAME[scaleSel]);
  }
  wasBtn = btn;

  static float f0 = 0.0f, conf = 1.0f;
  static int noteOut = -1;
  if (++hits >= 8) {                               // analyse about every 16ms
    hits = 0;
    if (!hold) {
      f0 = detectPitch(&conf);
      if (f0 > 40.0f && conf < CONF * 2.0f) {
        voiced = true;
        float midi = 69.0f + 12.0f * log2f(f0 / 440.0f);
        int   near = (int)lroundf(midi);
        uint16_t mask = SCALE[scaleSel];
        int best = near; int bestD = 99;
        for (int c = near - 6; c <= near + 6; c++) {
          int deg = ((c - key) % 12 + 12) % 12;
          if (!(mask & (1u << deg))) continue;
          int dd = c > near ? c - near : near - c;
          if (dd < bestD) { bestD = dd; best = c; }
        }
        noteOut = best;
        float tgtHz = 440.0f * powf(2.0f, (float)(best - 69) / 12.0f);
        float r = tgtHz / f0;
        if (r > 2.0f) r = 2.0f; if (r < 0.5f) r = 0.5f;
        target = r;
      } else {
        voiced = false;
        target = 1.0f;                             // unvoiced, so leave it alone
      }
    }
  }

  // Retune speed. At zero the ratio jumps and the correction is audible as a snap, which
  // is the effect people are usually after; wound up it slides and sounds like a singer.
  float glide = x1 * x1;                           // 0 .. 1
  float k = glide < 0.001f ? 1.0f : (1.0f - expf(-1.0f / (glide * 0.25f * 500.0f)));
  ratio += (target - ratio) * k;

  ledSet(voiced ? 200 : 0);

  if (++n >= 100) {                                // log roughly twice a second
    n = 0;
    if (voiced && noteOut >= 0)
      Serial.printf("[%-5s] key=%-2s in=%6.1fHz -> %s%d  ratio=%5.3f conf=%4.2f %s%s\n",
                    SCALENAME[scaleSel], NOTENAME[key], (double)f0,
                    NOTENAME[((noteOut % 12) + 12) % 12], noteOut / 12 - 1,
                    (double)ratio, (double)conf,
                    hold ? "HOLD " : "", bypass ? "BYPASS" : "");
    else
      Serial.printf("[%-5s] key=%-2s no pitch  ratio=%5.3f %s%s\n",
                    SCALENAME[scaleSel], NOTENAME[key], (double)ratio,
                    hold ? "HOLD " : "", bypass ? "BYPASS" : "");
  }
  delay(2);
}
