/* HAGIWO MOD2 / State Variable Filter  (Seeed XIAO RP2350)
 *
 * A state-variable filter in the TPT form, which stays stable and in tune however hard the
 * cutoff and resonance are pushed around underneath it.
 *
 *   POT1   : cutoff, 40Hz - 6kHz (log)
 *   POT2   : resonance, Q 0.5 - 30 (log)
 *   POT3   : input DC bias - keep centered, not a parameter
 *   BUTTON : cycles LP -> BP -> HP -> Notch, blinking the mode index plus one
 *   LED    : signal present
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

#define SL_AUD 0          // GPIO1 = slice0 channel B
#define SL_TIK 1          // timebase only, not routed to a pin
#define BIAS   1937.0f    // measured no-signal ADC center (theory says 2332; trust the meter)
#define GAIN   0.125f     // output gain; roughly unity at Q=1
#define FS     48828.1f   // sample rate (150MHz / 3072)

// --- DSP state ---
static float dcx, dcy, e1, e2;      // DC blocker + noise shaper
static float ic1, ic2;              // SVF integrator states
// --- coefficients: computed in loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, gOut = 0.0f, kOut = 1.0f;
// --- pot readings: written by the ISR, read by loop()
static volatile int pot1raw = 0, pot2raw = 0;
static volatile int pkMin = 4095, pkMax = 0;
// --- filter mode: 0=LP 1=BP 2=HP 3=Notch, cycled by the button
static volatile int fmode = 0;
static const char *MODE_NAME[4] = {"LP", "BP", "HP", "NOTCH"};
static int btnRel = 0;              // button level when released, sampled at boot

// Blocking one-shot conversion. Registers only, so the ISR never calls into flash.
static inline uint16_t adc_oneshot(uint ch) {
  adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) | (ch << ADC_CS_AINSEL_LSB);
  adc_hw->cs |= ADC_CS_START_ONCE_BITS;
  while (!(adc_hw->cs & ADC_CS_READY_BITS)) tight_loop_contents();
  return (uint16_t)adc_hw->result;
}

static void __not_in_flash_func(isr)(void) {
  pwm_hw->intr = 1u << SL_TIK;
  int raw = adc_hw->result;                        // audio sample requested by the previous ISR

  static uint32_t tick = 0;
  if ((++tick & 0xFF) == 0) {                      // steal a slot every 256 samples to read pots
    adc_oneshot(0); pot1raw = adc_oneshot(0);      // discard one, then keep (ch0 = POT1)
    adc_oneshot(1); pot2raw = adc_oneshot(1);      // 100k pots need the throwaway conversion:
    adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) // charge from the previous channel lingers
               | (2u << ADC_CS_AINSEL_LSB);        // back to A2
  }
  hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS); // queue the next audio sample, never block

  if (raw < pkMin) pkMin = raw;
  if (raw > pkMax) pkMax = raw;

  float d = BIAS - (float)raw;                     // the analog front end inverts
  dcy = d - dcx + 0.9995f * dcy; dcx = d;          // DC block, ~4Hz corner
  float in = dcy;

  // --- TPT state-variable filter (Zavalishin / Cytomic topology) ---
  float v3 = in - ic2;
  float v1 = a1 * ic1 + a2 * v3;
  float v2 = ic2 + a2 * ic1 + a3 * v3;
  ic1 = 2.0f * v1 - ic1;
  ic2 = 2.0f * v2 - ic2;
  float out;
  switch (fmode) {
    default:
    case 0: out = v2;                    break;    // low pass
    case 1: out = v1;                    break;    // band pass
    case 2: out = in - kOut * v1 - v2;   break;    // high pass
    case 3: out = in - kOut * v1;        break;    // notch (LP + HP)
  }

  // 10-bit PWM puts the carrier at 146.5kHz; 2nd-order error feedback buys back
  // the two bits lost versus HAGIWO's stock 12-bit/36.6kHz setup.
  float t = 512.0f + gOut * out + 1.5f * e1 - 0.5f * e2;
  int q = (int)(t + 0.5f);
  if (q < 0) q = 0; else if (q > 1023) q = 1023;
  e2 = e1; e1 = t - (float)q;
  pwm_hw->slice[SL_AUD].cc = (uint32_t)q << 16;    // channel B
}

void setup() {
  set_sys_clock_khz(150000, true);
  Serial.begin(115200);
  // Run the ADC off PLL_SYS, not the default PLL_USB: separate clock sources drift
  // against the PWM timebase and drop or duplicate samples. 150/48 = 3.125 divides cleanly.
  clock_configure(clk_adc, 0, CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                  150000000, 48000000);
  adc_init();
  adc_gpio_init(26); adc_gpio_init(27); adc_gpio_init(28); // A0 / A1 / A2
  adc_select_input(2);                             // start on A2 (audio in)
  pinMode(5, OUTPUT);                              // LED
  pinMode(6, INPUT_PULLUP);                        // button is active low
  delay(10); btnRel = digitalRead(6);             // record the released level (do not hold at boot)
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
  // Pots to filter coefficients. Transcendentals live here, never in the ISR.
  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);
  float fc = 40.0f * powf(150.0f, x1);            // 40Hz .. 6kHz, log taper
  float Q  = 0.5f  * powf(60.0f,  x2);            // 0.5 .. 30, log taper
  float g  = tanf((float)M_PI * fc / FS);
  float kq = 1.0f / Q;
  float n1 = 1.0f / (1.0f + g * (g + kq));
  a1 = n1;
  a2 = g * n1;
  a3 = g * a2;
  kOut = kq;                                       // the ISR needs it for HP and notch
  // Gentle Q compensation. Full 1/Q makes high resonance collapse in level, so keep a
  // 0.3 floor: loud enough to stay usable, still quiet enough not to clip the output.
  gOut = GAIN * (0.3f + 0.7f * kq);

  // --- button: each press advances LP -> BP -> HP -> Notch ---
  static bool wasDown = false;
  bool down = (digitalRead(6) != btnRel);
  if (down && !wasDown) {
    fmode = (fmode + 1) & 3;
    for (int i = 0; i <= fmode; i++) {            // blink mode index + 1 times
      digitalWrite(5, HIGH); delay(120);
      digitalWrite(5, LOW);  delay(120);
    }
    Serial.printf(">> MODE = %s\n", MODE_NAME[fmode]);
  }
  wasDown = down;

  if (++n >= 250) {                               // log roughly twice a second
    n = 0;
    noInterrupts(); int lo = pkMin, hi = pkMax; pkMin = 4095; pkMax = 0; interrupts();
    digitalWrite(5, (hi - lo) > 200);
    Serial.printf("[%-5s] fc=%5dHz Q=%4.1f  min=%4d max=%4d pp=%4d\n",
                  MODE_NAME[fmode], (int)fc, (double)Q, lo, hi, hi - lo);
  }
  delay(2);                                       // ~500Hz coefficient update, smooth knob tracking
}
