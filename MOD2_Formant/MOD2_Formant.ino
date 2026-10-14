/* HAGIWO MOD2 / Formant  (Seeed XIAO RP2350)
 *
 * A parallel-formant voice. Three bandpass formants sit on a glottal pulse,
 * which is the late-70s way of making something that reads as a vowel. This
 * module has two knobs, so the live program spends them on pitch and which
 * vowel, and the phrase programs hand the second knob to speed.
 *
 *   POT1   : pitch, 50Hz to 400Hz, logarithmic
 *   POT2   : live, vowel O-U-A-E-I. On a phrase, phoneme length 45ms to 240ms
 *   POT3   : input DC bias - keep centered, not a parameter. The input is not
 *            in the signal path; BIAS is here so the constant stays in the
 *            same place as the other apps.
 *   IN1    : gate. Live, the vowel drones until the first gate, then it sounds
 *            only while IN1 is held. A rising edge restarts the glottal cycle.
 *            On a phrase, a rising edge plays it once; holding IN1 loops it.
 *   IN2    : formant scale, about +25% while held. Smaller vocal tract.
 *   BUTTON : tap cycles live vowel, HELLO, SPEAK, AIUEO, DID.
 *            Hold past 1.2s for voiced, breathy, whisper.
 *   LED    : output level
 *
 * The DSP, the phoneme rows and the MIT notice for those parts are in
 * klattsch_core.h. Tony Gies's klattsch is the source; the Playdate C port in
 * kurogedelic/klattsch-pd-test is what this was checked against.
 *
 * Requirements: same as the other apps here. The Eurorack +/-12V rail is
 * mandatory, the rear jumper goes to the MCU side. BIAS is unused: the input
 * is not mixed in, and the constant is left so a measured value has a home.
 *
 * MIT License - Copyright (c) 2026 Leo Kuroshita
 * Synthesizer and phoneme data: MIT License - Copyright (c) 2026 Tony Gies,
 * Copyright (c) 2026 Klattsch for Playdate contributors.
 * See klattsch_core.h for the full permission text.
 */

#include <math.h>
#include <hardware/adc.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/gpio.h>
#include "klattsch_core.h"

#define SL_AUD 0          // GPIO1 = slice0 channel B
#define SL_TIK 1          // timebase only, not routed to a pin
#define SL_LED 2          // GPIO5 = slice2 channel B
#define BIAS   1937.0f    // measured no-signal ADC center, calibrate per unit
#define FS     48828.1f   // sample rate (150MHz / 3072)
#define OUTLVL 0.70f
#define F0_LO  50.0f      // POT1 fully down
#define F0_SPAN 8.0f      // 50 * 8 = 400Hz at the top
#define SLOT_LO 0.045f    // seconds per phoneme, POT2 on a phrase
#define SLOT_HI 0.240f
#define SCALE_HI 1.25f    // formant scale while IN2 is held
#define NPROG  5          // live vowel plus KLATTSCH_PHRASE_COUNT

/* Glottal colour. Voiced is the plain pulse from klattsch (no extra breath). Breathy
 * keeps the pulse and adds noise. Whisper drops the pulse. */
typedef struct { float asp, effort, tilt, vmul; const char *name; } VoiceColor;
static const VoiceColor COLORS[3] = {
  { 0.00f, 0.55f,  0.00f, 1.0f, "VOICE" },
  { 0.50f, 0.25f, -0.20f, 1.0f, "BREATH" },
  { 0.85f, 0.40f,  0.35f, 0.0f, "WHISPER" },
};
static const char *const VOWEL_NAME[5] = { "O", "U", "A", "E", "I" };

static KlattschSynth synth;
static KlattschPhrase phrase;

static float dcx, dcy, e1, e2;
static float ampEnv = 0.0f;
static float atkC = 0.01f, relC = 0.001f;
static int settling = 0;

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float pitchHz = 120.0f, vowelPos = 0.5f;
static volatile float scaleCmd = 1.0f;
static volatile float aspCmd = 0.0f, effortCmd = 0.55f, tiltCmd = 0.0f, vmulCmd = 1.0f;
static volatile int slotSamples = 5371;          // ~110ms, until the knob is read
static volatile int program = 0;
static volatile int gateHeld = 0, heardGate = 0, doTrig = 0;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile float envMon = 0.0f;
static volatile int playMon = 0;

static inline uint16_t adc_oneshot(uint ch) {
  adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) | (ch << ADC_CS_AINSEL_LSB);
  adc_hw->cs |= ADC_CS_START_ONCE_BITS;
  while (!(adc_hw->cs & ADC_CS_READY_BITS)) tight_loop_contents();
  return (uint16_t)adc_hw->result;
}

/* The LED runs on the same 146.5kHz carrier as the audio output, and for the
 * same reason. At exactly three times the sample rate its switching folds to
 * DC when the ADC picks it up off the shared 3.3V rail. This does not make the
 * LED silent: its current still moves the rail, and a plain on/off blink is
 * audible on this board. */
static inline void ledSet(int v) {                 // 0..255
  if (v < 0) v = 0; else if (v > 255) v = 255;
  pwm_set_gpio_level(5, (uint16_t)(v << 2));
}

static inline float slewTo(float cur, float dest, float c) {
  float d = dest - cur;
  if (d < 0.00002f && d > -0.00002f) return dest;
  return cur + d * c;
}

static void __not_in_flash_func(isr)(void) {
  pwm_hw->intr = 1u << SL_TIK;
  int raw = adc_hw->result;                        // audio conversion from last time
  (void)raw;                                       // the jack is not in this patch

  static uint32_t tick = 0;
  if ((++tick & 0xFF) == 0) {                      // steal an ADC slot every 256 samples
    adc_oneshot(0); pot1raw = adc_oneshot(0);      // discard one, then keep (ch0 = POT1)
    adc_oneshot(1); pot2raw = adc_oneshot(1);      // the throwaway drains charge left by
    adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) // the previous channel
               | (2u << ADC_CS_AINSEL_LSB);
  }
  hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS); // queue the next conversion

  static float pitch = 120.0f, vowel = 0.5f, scale = 1.0f;
  static float asp = 0.0f, effort = 0.55f, tilt = 0.0f, vmul = 1.0f;
  static int latched = -1;

  pitch  = slewTo(pitch,  pitchHz,  0.005f);
  vowel  = slewTo(vowel,  vowelPos, 0.004f);
  scale  = slewTo(scale,  scaleCmd, 0.003f);
  asp    = slewTo(asp,    aspCmd,    0.002f);
  effort = slewTo(effort, effortCmd, 0.002f);
  tilt   = slewTo(tilt,   tiltCmd,   0.002f);
  vmul   = slewTo(vmul,   vmulCmd,  0.002f);

  int prog = program;
  int kick = doTrig;
  if (kick) doTrig = 0;

  if (prog != latched) {
    int prev = latched;
    latched = prog;
    if (prog == 0) {
      phrase.run = 0;
      if (prev >= 0) {
        KlattschFormants live;
        klattsch_vowel(vowel, &live);
        klattsch_set_formants(&synth, &live, klattsch_ms(FS, 30.0f));
        settling = 1;
      }
    } else {
      klattsch_phrase_start(&phrase, prog - 1, 1, &synth);
      settling = 0;
    }
  }
  if (kick && latched == 0) synth.glottal_phase = 0.0f;
  if (kick && latched > 0) klattsch_phrase_start(&phrase, latched - 1, 1, &synth);

  if (phrase.run) {
    klattsch_phrase_service(&phrase, &synth, slotSamples, gateHeld);
    settling = 0;
  } else if (latched == 0) {
    if (settling && synth.transition_remaining > 0) {
      klattsch_advance(&synth);
      if (synth.transition_remaining == 0) settling = 0;
    } else {
      KlattschFormants live;
      klattsch_vowel(vowel, &live);
      klattsch_write_formants(&synth, &live);
      settling = 0;
    }
  }

  synth.f0 = pitch;
  synth.aspiration = asp;
  synth.effort = effort;
  synth.tilt = tilt;
  synth.gain = 3.5f;
  float y = klattsch_render(&synth, scale, vmul);

  // Live program follows the gate once one has been seen. Until then it drones,
  // so a module with nothing patched still makes a sound. Phrases use their own
  // amplitude ramps and go quiet when the line finishes.
  float dest = 1.0f;
  if (latched == 0) dest = (heardGate && !gateHeld) ? 0.0f : 1.0f;
  else dest = phrase.run ? 1.0f : 0.0f;
  ampEnv += (dest - ampEnv) * (dest > ampEnv ? atkC : relC);
  if (ampEnv < 0.0f) ampEnv = 0.0f;
  if (ampEnv > 1.0f) ampEnv = 1.0f;
  y *= ampEnv;

  dcy = y - dcx + 0.9995f * dcy; dcx = y;          // ~4Hz, keeps the carrier centred
  float mag = dcy < 0.0f ? -dcy : dcy;
  if (mag > envMon) envMon = mag; else envMon *= 0.9993f;
  playMon = phrase.run;

  float t = 512.0f + dcy * 512.0f * OUTLVL + 1.5f * e1 - 0.5f * e2;
  int q = (int)(t + 0.5f);
  if (q < 0) q = 0; else if (q > 1023) q = 1023;
  e2 = e1; e1 = t - (float)q;
  pwm_hw->slice[SL_AUD].cc = (uint32_t)q << 16;
}

static void enableFlushToZero(void) {
  /* A dying formant can underflow to a denormal, and those are slow enough on
   * this FPU to crowd the sample period. Flush them. This firmware is the only
   * thing running. */
  uint32_t fpscr;
  __asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
  fpscr |= 1u << 24;
  __asm volatile("vmsr fpscr, %0" :: "r"(fpscr));
}

void setup() {
  set_sys_clock_khz(150000, true);
  Serial.begin(115200);
  klattsch_tables_init();
  klattsch_synth_init(&synth, FS);
  atkC = 1.0f - expf(-1.0f / (0.012f * FS));       // 12ms attack
  relC = 1.0f - expf(-1.0f / (0.050f * FS));       // 50ms release

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
  enableFlushToZero();                             // before the ISR runs any float
  irq_set_exclusive_handler(PWM_IRQ_WRAP_0, isr);
  irq_set_enabled(PWM_IRQ_WRAP_0, true);
  hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);
}

static int btnRel = 1, gateIdle = 0;
static int color = 0;

static void blink(int times) {
  for (int i = 0; i < times; i++) {
    ledSet(255); delay(120);
    ledSet(0);   delay(120);
  }
}

static const char *progName(int p) {
  if (p <= 0) return "VOWEL";
  return KLATTSCH_PHRASE_NAME[p - 1];
}

void loop() {
  static int n = 0;
  static bool init = false;
  if (!init) { init = true; btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);
  pitchHz = F0_LO * powf(F0_SPAN, x1);
  if (program == 0) vowelPos = x2;
  else {
    float sec = SLOT_LO * powf(SLOT_HI / SLOT_LO, x2);
    int slot = (int)(sec * FS);
    if (slot < 1) slot = 1;
    slotSamples = slot;
  }

  bool g1 = (digitalRead(7) != gateIdle);
  bool g2 = (digitalRead(0) != gateIdle);
  if (g1) heardGate = 1;
  gateHeld = g1 ? 1 : 0;
  scaleCmd = g2 ? SCALE_HI : 1.0f;

  static bool prevG1 = false;
  if (g1 && !prevG1) doTrig = 1;
  prevG1 = g1;

  aspCmd = COLORS[color].asp;
  effortCmd = COLORS[color].effort;
  tiltCmd = COLORS[color].tilt;
  vmulCmd = COLORS[color].vmul;

  // --- button: tap advances the program, hold past 1.2s changes the colour ---
  static bool wasDown = false;
  static int heldMs = 0;
  static bool consumed = false;
  bool down = (digitalRead(6) != btnRel);
  if (down) {
    if (!wasDown) { heldMs = 0; consumed = false; }
    heldMs += 2;
    if (heldMs > 1200 && !consumed) {
      consumed = true;
      color = (color + 1) % 3;
      Serial.printf(">> %s\n", COLORS[color].name);
      blink(color + 1);
    }
  } else if (wasDown && !consumed) {
    program = (program + 1) % NPROG;
    Serial.printf(">> %s\n", progName(program));
    blink(program + 1);
  }
  wasDown = down;

  if (!(down && consumed)) {
    float l = envMon;
    if (l < 0.0f) l = 0.0f;
    if (l > 1.0f) l = 1.0f;
    ledSet((int)(l * l * 255.0f));
  }

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    int vi = (int)(vowelPos * 4.0f + 0.5f);
    if (vi < 0) vi = 0;
    if (vi > 4) vi = 4;
    int slotMs = (int)((float)slotSamples * (1000.0f / FS) + 0.5f);
    if (program == 0) {
      Serial.printf("[VOWEL/%s] f0=%3.0fHz vow=%s env=%4.2f %s%s\n",
                    COLORS[color].name, (double)pitchHz, VOWEL_NAME[vi], (double)envMon,
                    heardGate ? (gateHeld ? "GATE" : "off") : "drone",
                    scaleCmd > 1.0f ? " SCALE" : "");
    } else {
      Serial.printf("[%s/%s] f0=%3.0fHz slot=%3dms %s%s\n",
                    progName(program), COLORS[color].name, (double)pitchHz, slotMs,
                    playMon ? "play" : "idle",
                    scaleCmd > 1.0f ? " SCALE" : "");
    }
  }
  delay(2);
}
