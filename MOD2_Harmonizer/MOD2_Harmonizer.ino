/* HAGIWO MOD2 / Harmonizer v1.0  (Seeed XIAO RP2350)
 *
 * After the Eventide H910, the 1975 box that was the first commercial digital audio effect
 * and the reason engineers say "glitch".
 *
 * It shifts pitch by reading a delay line at the wrong speed. Write at one rate, read at
 * another, and the pitch moves; the catch is that the read pointer then closes on the
 * write pointer and has to jump. Eventide's fix was to run two read taps and crossfade
 * between them at the jump, the way a tape splice is made. It helps, but the two taps sit
 * at different points of the waveform, so where they disagree in phase they cancel and the
 * level dips. That artifact is the sound of the machine. When Eventide later shipped the
 * de-glitched H949, some people missed it.
 *
 * The pitch shifter sits inside the feedback loop rather than after it, so each repeat is
 * shifted again. A semitone down with the feedback up gives the descending cascade the
 * H910 is known for.
 *
 *   POT1   : pitch, an octave either way in semitones
 *   POT2   : feedback
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : freeze. Feedback to unity with the input cut, so the cascade runs on its own.
 *   IN2    : force a splice. The glitch on demand, which is the point of having one.
 *   BUTTON : steps the delay, 0 / 28 / 56 / 112ms
 *   LED    : output level
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

#define BUFLEN 16384      // enough for the longest delay plus a splice window
#define WINDOW 4096.0f    // splice window, 84ms. At an octave up this splices about 12
                          // times a second, which is roughly where the original sat.
#define XFTAB  257        // crossfade table

#define DRY    0.5f
#define WET    0.7f

static float buf[BUFLEN];
static uint32_t wr = 0;
static float phase = 0.0f;                // where the read taps are inside the window

/* Two taps half a window apart, each faded by a raised cosine. The pair sums to exactly
 * one, so the crossfade is level on paper; in practice the taps are reading different
 * parts of the waveform and cancel wherever they disagree, which is the glitch.
 */
static float xfTab[XFTAB];

// --- DSP state ---
static float dcx, dcy, e1, e2, fbHold = 0.0f;
static uint32_t rng = 0x2545f491;

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float ratio = 1.0f, feedback = 0.3f, delayOfs = 0.0f;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool frozen = false, doSplice = false;
static volatile float outMon = 0.0f;
static volatile int semitone = 0;

static inline uint16_t adc_oneshot(uint ch) {
  adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) | (ch << ADC_CS_AINSEL_LSB);
  adc_hw->cs |= ADC_CS_START_ONCE_BITS;
  while (!(adc_hw->cs & ADC_CS_READY_BITS)) tight_loop_contents();
  return (uint16_t)adc_hw->result;
}

static inline float tapRead(float back) {
  float rd = (float)wr - back;
  while (rd < 0.0f) rd += (float)BUFLEN;
  int   i0 = (int)rd;
  int   i1 = i0 + 1; if (i1 >= BUFLEN) i1 = 0;
  float fr = rd - (float)i0;
  return buf[i0] + (buf[i1] - buf[i0]) * fr;
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

  if (doSplice) { doSplice = false; phase = 0.0f; } // IN2 jumps the taps by hand

  // --- the two taps ---
  float pA = phase;
  float pB = phase + 0.5f; if (pB >= 1.0f) pB -= 1.0f;

  float sA = tapRead(delayOfs + pA * WINDOW);
  float sB = tapRead(delayOfs + pB * WINDOW);

  int   jA = (int)(pA * (float)(XFTAB - 2));
  int   jB = (int)(pB * (float)(XFTAB - 2));
  float shifted = sA * xfTab[jA] + sB * xfTab[jB];

  // The read taps move at the pitch ratio while the write head moves at one, so the gap
  // between them changes steadily and the window has to wrap. Each wrap is a splice.
  phase += (1.0f - ratio) * (1.0f / WINDOW);
  if (phase >= 1.0f) phase -= 1.0f;
  else if (phase < 0.0f) phase += 1.0f;

  // --- write, with the shifter inside the loop ---
  float src = frozen ? 0.0f : in;
  float w = src + shifted * feedback;
  if (w >  1.6f) w =  1.6f;                        // the loop needs a ceiling to sit under
  if (w < -1.6f) w = -1.6f;
  buf[wr] = w;
  wr = (wr + 1 >= BUFLEN) ? 0 : wr + 1;

  float out = in * DRY + shifted * WET;
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

  for (int i = 0; i < BUFLEN; i++) buf[i] = 0.0f;
  for (int i = 0; i < XFTAB; i++) {
    float x = (float)i / (float)(XFTAB - 2);
    xfTab[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * x));
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
static const float DLYMS[4] = { 0.0f, 28.0f, 56.0f, 112.0f };

void loop() {
  static int n = 0, dlySel = 0;
  static bool init = false;
  static float drift = 0.0f;
  if (!init) { init = true; delay(10); btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  // Quantised, because the thing is called a Harmonizer. Middle of the knob is unison,
  // matching the original where middle C on the keyboard remote meant a ratio of 1.00.
  semitone = (int)lroundf(x1 * 24.0f) - 12;

  // The H910's master clock was a free-running LC oscillator rather than a crystal, so it
  // drifted slowly and never quite agreed with itself. That wander is why the front panel
  // display flickered, and it is part of why the box thickens a sound rather than just
  // transposing it.
  rng = rng * 1664525u + 1013904223u;
  drift += ((float)(int32_t)rng * (1.0f / 2147483648.0f)) * 0.00004f;
  drift *= 0.999f;
  ratio = powf(2.0f, (float)semitone / 12.0f) * (1.0f + drift);

  frozen = (digitalRead(7) != gateIdle);
  feedback = frozen ? 1.0f : x2 * 1.02f;           // just past unity is where it runs away

  static bool prevG2 = false;
  bool g2 = (digitalRead(0) != gateIdle);
  if (g2 && !prevG2) doSplice = true;
  prevG2 = g2;

  static bool wasBtn = false;
  bool btn = (digitalRead(6) != btnRel);
  if (btn && !wasBtn) {
    dlySel = (dlySel + 1) & 3;
    Serial.printf(">> DELAY %dms\n", (int)DLYMS[dlySel]);
  }
  wasBtn = btn;
  delayOfs = DLYMS[dlySel] * 0.001f * FS;

  float l = outMon * 2.0f; if (l > 1.0f) l = 1.0f;
  analogWrite(5, (int)(l * l * 255.0f));

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    Serial.printf("pitch=%+3d st (x%5.3f) fb=%4.2f delay=%3dms out=%4.2f %s\n",
                  semitone, (double)ratio, (double)feedback, (int)DLYMS[dlySel],
                  (double)outMon, frozen ? "FROZEN" : "");
  }
  delay(2);
}
