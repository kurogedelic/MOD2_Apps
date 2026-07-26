/* HAGIWO MOD2 / Calibrate  (Seeed XIAO RP2350)
 *
 * Every other app in this collection has a `BIAS` constant near the top, and every one of
 * them is wrong until you measure it. This is the sketch that measures it.
 *
 * BIAS is the ADC reading with nothing going in. It should sit near the middle of the
 * range, but the analog front end is an op-amp summing circuit fed from the -12V rail
 * through ordinary resistors, so component tolerance moves it by a few hundred counts from
 * one module to the next. Theory says 2332 on this hardware; the unit these apps were
 * written on reads 1937.
 *
 * Getting it wrong does not stop audio passing, since a DC blocker follows, but it eats
 * headroom asymmetrically: the further the resting point is from centre, the sooner one
 * half of the waveform clips.
 *
 *   1. Patch nothing into the input and centre POT3.
 *   2. Open the serial monitor at 115200.
 *   3. Read the `center` figure once it settles, and copy it into the `BIAS` define of
 *      whichever app you are building.
 *
 * It also passes audio straight through, so it doubles as a way to check the module is
 * alive at all. `pp` is the peak-to-peak of the raw input: a few tens with nothing patched
 * is the noise floor, and a signal should push it into the thousands.
 *
 * If `center` sits far from 1900-2400 and POT3 does not move it, the analog front end has
 * no power. That means the Eurorack rail is missing, which is the usual cause: USB alone
 * runs the microcontroller but not the op-amps in front of it.
 *
 * MIT License - Copyright (c) 2026 Leo Kuroshita
 */

#include <hardware/adc.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/gpio.h>

#define SL_AUD 0          // GPIO1 = slice0 channel B
#define SL_TIK 1          // timebase only, not routed to a pin
#define SL_LED 2          // GPIO5 = slice2 channel B
#define FS     48828.1f   // sample rate (150MHz / 3072)
#define GAIN   0.25f      // pass-through level

static float dcx, dcy, e1, e2;
static volatile int pkMin = 4095, pkMax = 0;
static volatile int lastRaw = 0;

static inline void ledSet(int v) {                 // 0..255, see the README on LED noise
  if (v < 0) v = 0; else if (v > 255) v = 255;
  pwm_set_gpio_level(5, (uint16_t)(v << 2));
}

static void __not_in_flash_func(isr)(void) {
  pwm_hw->intr = 1u << SL_TIK;
  int raw = adc_hw->result;
  hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);   // queue the next sample

  lastRaw = raw;
  if (raw < pkMin) pkMin = raw;
  if (raw > pkMax) pkMax = raw;

  // Deliberately no BIAS here: the DC blocker finds the resting point on its own, which is
  // the whole reason this sketch can measure it without knowing it first.
  float d = -(float)raw;                             // the analog front end inverts
  dcy = d - dcx + 0.9995f * dcy; dcx = d;

  float t = 512.0f + GAIN * dcy + 1.5f * e1 - 0.5f * e2;
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
  adc_init(); adc_gpio_init(28); adc_select_input(2);

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

void loop() {
  delay(500);
  noInterrupts(); int lo = pkMin, hi = pkMax; pkMin = 4095; pkMax = 0; interrupts();
  int pp = hi - lo, ctr = (lo + hi) / 2;

  ledSet(pp > 200 ? 255 : 0);                      // lit once something is actually coming in

  const char *note = "";
  if (pp > 200)                    note = "  <- signal present, unpatch to read BIAS";
  else if (ctr < 1200 || ctr > 3000) note = "  <- check POT3, and that the rack rail is connected";

  Serial.printf("min=%4d max=%4d pp=%4d   BIAS = %4d%s\n", lo, hi, pp, ctr, note);
}
