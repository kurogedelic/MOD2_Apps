/* HAGIWO MOD2 / Granular v1.0  (Seeed XIAO RP2350)
 *
 * A granular texture processor. The last few seconds of input are always in memory, and a
 * cloud of short overlapping grains is drawn out of that buffer continuously. Move the
 * position knob and you travel backwards through what just happened; freeze it and those
 * few seconds become a fixed texture to pick over.
 *
 * This is not a port of Clouds. Clouds is stereo with six controls, and this module has
 * one input, two knobs and a pair of gates, so the shape had to be worked out from the
 * hardware rather than translated onto it. Density is the main casualty and the main
 * saving: rather than spend a knob on it, grains are scheduled to keep a constant overlap,
 * so the texture stays coherent at any size.
 *
 *   POT1   : position, from live at one end to a few seconds back at the other
 *   POT2   : grain size, 5ms to 500ms
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : throw a single grain, on top of whatever the cloud is already doing
 *   IN2    : grains play backwards
 *   BUTTON : freeze. Recording stops and the buffer becomes a fixed piece of material.
 *   LED    : grain activity
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

#define BUFLEN  180000    // 360KB of SRAM, 3.7 seconds of rolling memory
#define NVOICE  8         // grains that can be in flight at once
#define OVERLAP 4         // how many grains are meant to be sounding at any moment
#define ENVSZ   257       // Hann window table, one extra entry for interpolation

#define GMIN    244       // 5ms
#define GMAX    24414     // 500ms
#define DETUNE  0.01f     // per-grain pitch spread; a cloud of identical grains sounds dead
#define JITTER  0.35f     // start-point scatter, as a fraction of grain length

// --- the buffer ---
static int16_t buf[BUFLEN];
static volatile uint32_t wr = 0;

/* A grain that starts and stops abruptly is two clicks with some audio in between, so each
 * one is shaped by a Hann window: zero at both ends, no discontinuity anywhere. With
 * several overlapping, the windows sum to something close to flat.
 */
static float envTab[ENVSZ];

struct Grain {
  float    pos;           // read head into the buffer
  float    step;          // playback rate, slightly detuned per grain
  float    envPh;         // 0..1 through the window
  float    envInc;
  uint32_t left;          // samples remaining
  bool     active;
};
static struct Grain gr[NVOICE];
static uint32_t nextSpawn = 0;

// --- DSP state ---
static float dcx, dcy, e1, e2;
static uint32_t rng = 0x9e3779b9;

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float position = 0.0f;
static volatile uint32_t grainLen = 4800;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool frozen = false, revMode = false;
static volatile uint32_t manual = 0;
static volatile float outMon = 0.0f;

static inline uint16_t adc_oneshot(uint ch) {
  adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) | (ch << ADC_CS_AINSEL_LSB);
  adc_hw->cs |= ADC_CS_START_ONCE_BITS;
  while (!(adc_hw->cs & ADC_CS_READY_BITS)) tight_loop_contents();
  return (uint16_t)adc_hw->result;
}

static inline float frand(void) {                  // -1 .. 1
  rng = rng * 1664525u + 1013904223u;
  return (float)(int32_t)rng * (1.0f / 2147483648.0f);
}

static void __not_in_flash_func(spawn)(void) {
  int v = -1;
  for (int i = 0; i < NVOICE; i++) if (!gr[i].active) { v = i; break; }
  if (v < 0) return;                               // all busy, let this one go

  uint32_t len = grainLen;
  // Scatter the start point a little. Grains taken from exactly the same spot every time
  // sound like a loop rather than a cloud, and the scatter is what turns a repeat into a
  // texture.
  float off = position * (float)(BUFLEN - GMAX) + frand() * JITTER * (float)len;
  float st  = (float)wr - off - (float)len;
  while (st < 0.0f) st += (float)BUFLEN;

  gr[v].step   = 1.0f + frand() * DETUNE;
  if (revMode) { gr[v].pos = st + (float)len; gr[v].step = -gr[v].step; }
  else         { gr[v].pos = st; }
  gr[v].envPh  = 0.0f;
  gr[v].envInc = 1.0f / (float)len;
  gr[v].left   = len;
  gr[v].active = true;
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

  // --- record head, unless frozen ---
  if (!frozen) {
    int q = (int)(in * 32767.0f);
    if (q >  32767) q =  32767;
    if (q < -32768) q = -32768;
    buf[wr] = (int16_t)q;
    wr = (wr + 1 >= BUFLEN) ? 0 : wr + 1;
  }

  // --- scheduler ---
  // Spawn often enough that OVERLAP grains are sounding at once. Tying the rate to the
  // length rather than a knob keeps the texture even as the size changes: short grains
  // arrive quickly, long ones rarely, and the density looks after itself.
  if (nextSpawn == 0) {
    spawn();
    uint32_t iv = grainLen / OVERLAP;
    nextSpawn = iv < 32 ? 32 : iv;
  }
  nextSpawn--;
  if (manual) { manual = 0; spawn(); }             // IN1 throws one regardless

  // --- grains ---
  float sum = 0.0f;
  for (int i = 0; i < NVOICE; i++) {
    if (!gr[i].active) continue;

    float p = gr[i].pos;
    int   i0 = (int)p;
    int   i1 = i0 + 1; if (i1 >= BUFLEN) i1 = 0;
    float fr = p - (float)i0;
    float s  = ((float)buf[i0] + ((float)buf[i1] - (float)buf[i0]) * fr) * (1.0f / 32767.0f);

    float ei = gr[i].envPh * (float)(ENVSZ - 2);
    int   ej = (int)ei;
    float en = envTab[ej] + (envTab[ej + 1] - envTab[ej]) * (ei - (float)ej);
    sum += s * en;

    gr[i].pos += gr[i].step;
    if (gr[i].pos >= (float)BUFLEN) gr[i].pos -= (float)BUFLEN;
    else if (gr[i].pos < 0.0f)      gr[i].pos += (float)BUFLEN;
    gr[i].envPh += gr[i].envInc;
    if (gr[i].envPh > 1.0f) gr[i].envPh = 1.0f;
    if (--gr[i].left == 0) gr[i].active = false;
  }

  float out = sum * (2.0f / (float)OVERLAP);       // overlapping windows sum, so scale back
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

  for (int i = 0; i < BUFLEN; i++) buf[i] = 0;
  for (int i = 0; i < NVOICE; i++) gr[i].active = false;
  for (int i = 0; i < ENVSZ; i++) {
    float x = (float)i / (float)(ENVSZ - 2);
    envTab[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * x));   // Hann
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

void loop() {
  static int n = 0;
  static bool init = false;
  if (!init) { init = true; delay(10); btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  position = x1;                                   // live at one end, seconds back at the other
  grainLen = (uint32_t)(GMIN * powf((float)GMAX / (float)GMIN, x2));   // 5ms .. 500ms, log

  static bool prevG1 = false;
  bool g1 = (digitalRead(7) != gateIdle);
  if (g1 && !prevG1) manual = 1;                   // IN1 throws a grain
  prevG1 = g1;

  revMode = (digitalRead(0) != gateIdle);          // IN2 runs them backwards

  static bool wasBtn = false;
  bool btn = (digitalRead(6) != btnRel);
  if (btn && !wasBtn) {
    frozen = !frozen;
    Serial.printf(">> %s\n", frozen ? "FROZEN" : "LIVE");
  }
  wasBtn = btn;

  float l = outMon * 2.0f; if (l > 1.0f) l = 1.0f;
  analogWrite(5, (int)(l * l * 255.0f));

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    int act = 0;
    for (int i = 0; i < NVOICE; i++) if (gr[i].active) act++;
    Serial.printf("[%s] pos=%4.2f (%4.2fs) size=%4dms voices=%d %s\n",
                  frozen ? "FROZ" : "LIVE", (double)position,
                  (double)(position * (BUFLEN - GMAX) / FS),
                  (int)(grainLen * 1000.0f / FS), act, revMode ? "REV" : "");
  }
  delay(2);
}
