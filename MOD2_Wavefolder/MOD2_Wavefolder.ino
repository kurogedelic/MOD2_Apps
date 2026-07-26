/* HAGIWO MOD2 / Wavefolder  (Seeed XIAO RP2350)
 *
 * A wavefolder. Where a distortion flattens a waveform against a ceiling, a folder turns
 * it back on itself, so peaks come back down and the harmonics it adds move around rather
 * than just piling up. It is the west coast way of getting from a plain tone to a complex
 * one without a filter, and it wants to be fed something simple: a sine or a triangle
 * folds beautifully, a full mix turns to mud.
 *
 * The symmetry control is the reason this app suits this module in particular. Offsetting
 * the signal before folding decides whether it folds evenly on both halves, and that needs
 * the input to carry DC. This one does.
 *
 *   POT1   : fold. How hard the signal is driven into the folder.
 *   POT2   : symmetry. Offsets the signal so the two halves fold differently.
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : trigger an envelope that adds to the fold, the way a west coast patch runs an
 *            envelope into timbre rather than into a filter
 *   IN2    : doubles the fold while held
 *   BUTTON : cycles the folding curve, sine -> triangle -> clip
 *   LED    : how much folding is happening
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

#define STAB   1025       // one cycle of sine, plus a wrap entry
#define ENVMS  400.0f     // decay of the trigger envelope

/* Folding curves.
 *
 * The sine folder is the Buchla one: drive the signal into sin() and every time it passes
 * a quarter cycle the output turns around. It is smooth, so the harmonics it makes stay
 * musical even at high drive.
 *
 * The triangle folder reflects instead of curving, which is what a diode ladder folder
 * does in hardware. Sharper corners, brighter and more aggressive.
 *
 * Clipping is in here to make the point: same drive control, but it flattens instead of
 * folding, and the difference is immediate.
 */
#define F_SINE 0
#define F_TRI  1
#define F_CLIP 2

static float sinTab[STAB];

// --- DSP state ---
static float dcx, dcy, e1, e2;
static float env = 0.0f;

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float fold = 1.0f, sym = 0.0f, envDec = 0.9999f;
static volatile int pot1raw = 0, pot2raw = 0, curve = F_SINE;
static volatile bool boost = false, trig = false;
static volatile float foldMon = 0.0f;

static inline uint16_t adc_oneshot(uint ch) {
  adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) | (ch << ADC_CS_AINSEL_LSB);
  adc_hw->cs |= ADC_CS_START_ONCE_BITS;
  while (!(adc_hw->cs & ADC_CS_READY_BITS)) tight_loop_contents();
  return (uint16_t)adc_hw->result;
}

/* The LED runs on the same 146.5kHz carrier as the audio output, and for the same reason.
 * At exactly three times the sample rate its switching folds to DC when the ADC picks it
 * up off the shared 3.3V rail. This does not make the LED silent: its current still moves
 * the rail, and a plain on/off blink is audible on this board.
 */
static inline void ledSet(int v) {                 // 0..255
  if (v < 0) v = 0; else if (v > 255) v = 255;
  pwm_set_gpio_level(5, (uint16_t)(v << 2));
}

// sin(pi * x) from the table, wrapping. No sinf() in the interrupt.
static inline float sinLut(float x) {
  float p = x * 0.5f;                              // one table pass per 2.0 of input
  p -= (float)(int)p;                              // wrap to -1..1
  if (p < 0.0f) p += 1.0f;
  float fi = p * (float)(STAB - 1);
  int   i  = (int)fi;
  return sinTab[i] + (sinTab[i + 1] - sinTab[i]) * (fi - (float)i);
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

  if (trig) { trig = false; env = 1.0f; }
  env *= envDec;

  float drive = fold * (1.0f + env * 3.0f);
  if (boost) drive *= 2.0f;
  float x = in * drive + sym;
  foldMon = drive * (1.0f / 12.0f);

  float y;
  switch (curve) {
    default:
    case F_SINE:
      y = sinLut(x);
      break;
    case F_TRI: {
      // Reflect back into range as many times as it takes. Bounded because the drive is,
      // so this cannot spin.
      float t = x;
      for (int k = 0; k < 8; k++) {
        if      (t >  1.0f) t =  2.0f - t;
        else if (t < -1.0f) t = -2.0f - t;
        else break;
      }
      y = t;
      break;
    }
    case F_CLIP:
      y = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
      break;
  }

  // Folding leaves a DC component whenever symmetry is off centre, and that would push the
  // output off its midpoint rather than making a sound.
  static float oX = 0.0f, oY = 0.0f;
  oY = y - oX + 0.9995f * oY; oX = y;
  float out = oY * 0.8f;

  float t2 = 512.0f + out * 512.0f * OUTLVL + 1.5f * e1 - 0.5f * e2;  // 2nd-order error FB
  int q = (int)(t2 + 0.5f);
  if (q < 0) q = 0; else if (q > 1023) q = 1023;
  e2 = e1; e1 = t2 - (float)q;
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
static const char *CURVENAME[3] = { "SINE", "TRI", "CLIP" };

void loop() {
  static int n = 0;
  static bool init = false;
  if (!init) { init = true; delay(10); btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  fold   = 0.7f + 11.3f * x1 * x1;                 // squared, so the low end stays usable
  sym    = (x2 - 0.5f) * 2.0f;                     // -1 .. 1 of offset before the fold
  envDec = expf(-1.0f / (0.001f * ENVMS * FS));

  static bool prevG1 = false;
  bool g1 = (digitalRead(7) != gateIdle);
  if (g1 && !prevG1) trig = true;
  prevG1 = g1;
  boost = (digitalRead(0) != gateIdle);

  static bool wasBtn = false;
  bool btn = (digitalRead(6) != btnRel);
  if (btn && !wasBtn) {
    curve = (curve + 1) % 3;
    Serial.printf(">> %s\n", CURVENAME[curve]);
  }
  wasBtn = btn;

  float l = foldMon; if (l > 1.0f) l = 1.0f;
  ledSet((int)(l * l * 255.0f));

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    Serial.printf("[%-4s] fold=%5.2f sym=%+5.2f env=%4.2f %s\n",
                  CURVENAME[curve], (double)fold, (double)sym, (double)env,
                  boost ? "BOOST" : "");
  }
  delay(2);
}
