/* HAGIWO MOD2 / Digital Low Pass Gate v1.2  (Seeed XIAO RP2350)
 *
 * A Buchla-style low pass gate. One envelope opens amplitude and brightness together
 * through a simulated vactrol, so notes bloom in and decay with the highs falling away
 * first, the way a struck object does.
 *
 * Signal path: CV in (A2/GPIO28) -> ADC @48.83kHz -> DC block -> SVF + VCA -> PWM (GPIO1)
 *
 *   IN1 / IN2 : trigger or gate, OR'd with the button
 *   BUTTON    : manual trigger. Tap to ping, hold to sustain the gate.
 *               Hold past 1.2s to cycle LPG -> VCA -> LPF instead (LED blinks the mode).
 *                 LPG  envelope drives cutoff and amplitude
 *                 VCA  amplitude only, filter stays open
 *                 LPF  cutoff only, no amplitude control
 *   POT1      : decay   20ms - 4s (log)
 *   POT2      : timbre. How far open the gate travels at full envelope, from a dark thud
 *               around 150Hz to a bright snap at 6kHz.
 *   POT3      : input DC bias - keep centered, not a parameter
 *   LED       : follows the envelope, like the lamp inside the vactrol
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
#define GAIN   0.125f     // output gain
#define FS     48828.1f   // sample rate (150MHz / 3072)
#define GTAB   257        // cutoff coefficient table, one extra entry for interpolation
#define TRIGMS 4          // minimum gate width, so a short trigger still opens fully

/* Vactrol model.
 *
 * An LED shining on a photoresistor does not follow its drive current. It rises in a few
 * milliseconds, but falls far more slowly, and the fall is not a single exponential: the
 * cell has a fast component and a much slower one that keeps the gate cracked open long
 * after the note should have ended. That lingering tail is most of what makes a low pass
 * gate sound like a low pass gate rather than a VCA, so it gets modelled explicitly as
 * two decays summed together.
 */
#define ATK_MS   1.5f     // rise time, quick but never instant
#define SLOW_MUL 4.0f     // slow component decays this many times slower than the fast one
#define SLOW_MIX 0.28f    // how much of the tail comes from the slow component
#define EXP_K    4.0f     // amplitude taper curvature; larger drops away faster
#define FILT_Q   0.707f   // Butterworth, the flattest a two-pole gets. A real 292 has no
                          // resonance, and any peak here reads as a ring on every hit.

// --- DSP state ---
static float dcx, dcy, e1, e2;      // DC blocker + noise shaper
static float ic1, ic2;              // SVF integrator states
static float vFast = 0.0f, vSlow = 0.0f;   // the two vactrol components
static uint32_t trigHold = 0;       // samples left on the minimum gate width
// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float atkC = 0.05f, decFastC = 0.001f, decSlowC = 0.0003f;
static volatile float kOut = 1.0f, gOut = 0.0f, timbre = 1.0f;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool btnGate = false;
static volatile float envMon = 0.0f; // envelope copy for the LED and the log
// --- filter mode
static volatile int fmode = 0;
static const char *MODE_NAME[3] = {"LPG", "VCA", "LPF"};
static int btnRel = 1, gateIdle = 0;

/* The SVF needs g = tan(pi*fc/Fs) every sample, but the cutoff now moves with the
 * envelope and tanf() must never run inside the interrupt. Precompute g across the cutoff
 * range once and interpolate. Only g is tabulated: it depends solely on the envelope,
 * while resonance is a separate term.
 */
static float gTab[GTAB];

/* Amplitude taper. A linear fade sounds like a volume knob being turned down, because
 * hearing is logarithmic: the top of the fall is inaudible and the bottom drags. Feeding
 * the envelope through an exponential curve puts the loudness change where the ear
 * expects it, which is what gives the decay its snap.
 */
static float ampTab[GTAB];

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

  // --- gate ---
  // Either jack or the button fires. A rising edge latches the gate open for a few
  // milliseconds so that a 1ms trigger pulse still drives the vactrol to full brightness.
  static bool gPrev = false;
  bool gate = (gpio_get(7) != (bool)gateIdle) || (gpio_get(0) != (bool)gateIdle) || btnGate;
  if (gate && !gPrev) trigHold = (uint32_t)(FS * 0.001f * TRIGMS);
  gPrev = gate;
  if (trigHold) { trigHold--; gate = true; }

  // --- vactrol ---
  // Rising, both components track the drive quickly. Falling, they separate: the fast one
  // gives the initial drop, the slow one holds the tail up behind it.
  float drive = gate ? 1.0f : 0.0f;
  vFast += (drive - vFast) * ((drive > vFast) ? atkC : decFastC);
  vSlow += (drive - vSlow) * ((drive > vSlow) ? atkC : decSlowC);
  float v = (1.0f - SLOW_MIX) * vFast + SLOW_MIX * vSlow;
  envMon = v;

  float d = BIAS - (float)raw;                     // the analog front end inverts
  dcy = d - dcx + 0.9995f * dcy; dcx = d;          // DC block, ~4Hz corner
  float in = dcy;

  // --- envelope drives cutoff, except in VCA mode where the filter stays open ---
  // Timbre scales how far up the table the envelope reaches, which sets the cutoff the
  // gate opens into without needing the table rebuilt.
  float fenv = (fmode == 1) ? 1.0f : v * timbre;
  float fi = fenv * (float)(GTAB - 2);
  int   ii = (int)fi;
  float fr = fi - (float)ii;
  float g  = gTab[ii] + (gTab[ii + 1] - gTab[ii]) * fr;   // interpolate, no zipper noise

  float kq = kOut;
  float n1 = 1.0f / (1.0f + g * (g + kq));          // a divide is fine here, the FPU has
  float a2 = g * n1;                                // thousands of spare cycles per sample
  float a3 = g * a2;

  float v3 = in - ic2;
  float v1 = n1 * ic1 + a2 * v3;
  float v2 = ic2 + a2 * ic1 + a3 * v3;
  ic1 = 2.0f * v1 - ic1;
  ic2 = 2.0f * v2 - ic2;

  // --- amplitude follows the envelope, except in LPF mode ---
  float ai = v * (float)(GTAB - 2);
  int   aj = (int)ai;
  float vca = (fmode == 2) ? 1.0f
            : ampTab[aj] + (ampTab[aj + 1] - ampTab[aj]) * (ai - (float)aj);
  float out = v2 * vca;

  float t = 512.0f + gOut * out + 1.5f * e1 - 0.5f * e2;  // 2nd-order error feedback
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

  // Travel of the gate: nearly shut at 20Hz, wide open at 6kHz. Above that the input
  // stage rolls off anyway, so there is nothing left to open into.
  float eNorm = 1.0f / (expf(EXP_K) - 1.0f);
  for (int i = 0; i < GTAB; i++) {
    float x  = (float)i / (float)(GTAB - 2);
    float fc = 20.0f * powf(300.0f, x);
    gTab[i]  = tanf((float)M_PI * fc / FS);
    ampTab[i] = (expf(EXP_K * x) - 1.0f) * eNorm;   // 0 at rest, 1 wide open
  }

  pinMode(5, OUTPUT);                              // LED
  // The LED shares the 3.3V rail with the analog front end, and analogWrite defaults to
  // 1kHz in this core, which lands right in the middle of the audio band. Push the
  // switching well above hearing so the brightness metering costs nothing.
  analogWriteFreq(100000);
  pinMode(6, INPUT_PULLUP);                        // button is active low
  pinMode(7, INPUT); pinMode(0, INPUT);            // gate inputs, externally pulled
  delay(10);
  btnRel   = digitalRead(6);                       // released level, do not hold at boot
  gateIdle = digitalRead(7);                       // idle level, leave gates unpatched at boot

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

void loop() {
  static int n = 0;
  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  // Decay sets the fast component; the slow one trails it by a fixed ratio so the tail
  // stretches with the knob instead of staying put.
  float decay = 0.02f * powf(200.0f, x1);         // 20ms .. 4s, log
  atkC     = 1.0f - expf(-1.0f / (0.001f * ATK_MS * FS));
  decFastC = 1.0f - expf(-1.0f / (decay * FS));
  decSlowC = 1.0f - expf(-1.0f / (decay * SLOW_MUL * FS));

  timbre = 0.35f + 0.65f * x2;                    // reaches 150Hz .. 6kHz at full envelope
  kOut = 1.0f / FILT_Q;
  gOut = GAIN;

  // --- button: tap to trigger, hold past 1.2s to change mode ---
  static bool wasDown = false;
  static int  heldMs = 0;
  static bool consumed = false;
  bool down = (digitalRead(6) != btnRel);
  if (down) {
    if (!wasDown) { heldMs = 0; consumed = false; }
    heldMs += 2;
    if (heldMs > 1200 && !consumed) {              // long hold changes mode instead
      consumed = true;
      btnGate  = false;                            // drop the gate we were holding
      fmode    = (fmode + 1) % 3;
      for (int i = 0; i <= fmode; i++) {           // blink mode index plus one
        digitalWrite(5, HIGH); delay(120);
        digitalWrite(5, LOW);  delay(120);
      }
      Serial.printf(">> MODE = %s\n", MODE_NAME[fmode]);
    }
    btnGate = !consumed;
  } else {
    btnGate = false;
  }
  wasDown = down;

  // LED tracks the envelope, like the lamp inside a vactrol
  if (!consumed || !down) {
    float e = envMon;
    analogWrite(5, (int)(e * e * 255.0f));         // squared, closer to how the eye reads it
  }

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    Serial.printf("[%s] decay=%5dms timbre=%4.2f env=%4.2f gate=%d%d btn=%d\n",
                  MODE_NAME[fmode], (int)(decay * 1000.0f), (double)timbre,
                  (double)envMon, digitalRead(7), digitalRead(0), (int)btnGate);
  }
  delay(2);
}
