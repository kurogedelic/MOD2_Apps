/* HAGIWO MOD2 / Spring v1.0  (Seeed XIAO RP2350)
 *
 * A spring reverb. Not a hall, not a plate: two coiled springs with transducers at each
 * end, which is a very particular and very recognisable way to make a sound last.
 *
 * The bandwidth of this module suits it. The input stage rolls off around 7.2kHz, which
 * makes a hall sound muffled, but a real spring tank has no top end either. What would be
 * a limitation elsewhere is close to the correct answer here.
 *
 *   POT1   : wet / dry
 *   POT2   : spring length, from a short bright tank to a long slack one
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : freeze. Holds the tank ringing and shuts the input out.
 *   IN2    : crash. Kicks the tank, the way knocking a real amp does.
 *   BUTTON : crash by hand
 *   LED    : follows what is ringing in the tank
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

/* A spring does not pass a transient along in one piece. High frequencies travel faster
 * than low ones, so a knock at one end arrives at the other smeared out into a descending
 * chirp: the sproing. A chain of first-order allpass sections does the same thing, since
 * each one delays by an amount that depends on frequency while leaving the magnitude
 * alone. One section is inaudible. Forty of them is a spring.
 */
#define NAP    40         // allpass sections per spring
#define NSPR   2          // two springs, as in a real two-spring tank
#define DECAY  0.86f      // fixed: how much survives each trip along the spring

/* Longest each spring can be, in samples. The two are coprime so their echoes never line
 * up and comb the way a single resonant tube would.
 */
static const int SPRMAX[NSPR] = { 5003, 6301 };
#define BUFLEN 6301

// --- the tank ---
static float apz[NSPR][NAP];              // allpass state, one sample of memory each
static float dly[NSPR][BUFLEN];           // the spring itself
static int   wr[NSPR];                    // write head; the read head trails it by the length
static float damp[NSPR];                  // a spring loses its top end on every pass

// --- DSP state ---
static float dcx, dcy, e1, e2;
static uint32_t rng = 0x1234567;          // for the crash

// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float wet = 0.5f, lenScale = 0.6f, tension = 0.55f;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool frozen = false;
static volatile uint32_t crash = 0;       // samples of noise burst left to inject
static volatile float tankMon = 0.0f;

/* The LED runs on the same 146.5kHz carrier as the audio output, and for the same reason.
 * At exactly three times the sample rate its switching folds to DC when the ADC picks it
 * up off the shared 3.3V rail. This does not make the LED silent: its current still moves
 * the rail, and a plain on/off blink is audible on this board. It only keeps the switching
 * itself from landing somewhere you can hear, which 100kHz did not, folding back to 2.3kHz.
 */
static inline void ledSet(int v) {                 // 0..255
  if (v < 0) v = 0; else if (v > 255) v = 255;
  pwm_set_gpio_level(5, (uint16_t)(v << 2));
}

static inline uint16_t adc_oneshot(uint ch) {
  adc_hw->cs = (adc_hw->cs & ~ADC_CS_AINSEL_BITS) | (ch << ADC_CS_AINSEL_LSB);
  adc_hw->cs |= ADC_CS_START_ONCE_BITS;
  while (!(adc_hw->cs & ADC_CS_READY_BITS)) tight_loop_contents();
  return (uint16_t)adc_hw->result;
}

static inline float softclip(float x) {
  if (x >  1.5f) return  1.0f;
  if (x < -1.5f) return -1.0f;
  return x * (1.0f - 0.1481481f * x * x);
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

  float drive = frozen ? 0.0f : in;                // freeze shuts the input out
  if (crash) {                                     // a kick to the tank: short noise burst
    crash--;
    rng = rng * 1664525u + 1013904223u;
    drive += ((float)(int32_t)rng * (1.0f / 2147483648.0f)) * 0.35f;
  }

  // The length knob is smoothed here rather than jumping, so that winding the tank out
  // sounds like the springs being stretched instead of the buffer being re-pointed.
  static float lenSm = 0.6f;
  lenSm += (lenScale - lenSm) * 0.0006f;
  float g = tension;
  float sum = 0.0f;

  for (int s = 0; s < NSPR; s++) {
    // Read the far end of the spring, then feed it back into the near end. The chirp comes
    // from what happens on the way.
    float fl = lenSm * (float)SPRMAX[s];
    float rd = (float)wr[s] - fl;
    if (rd < 0.0f) rd += (float)BUFLEN;
    int   r0 = (int)rd;
    int   r1 = r0 + 1; if (r1 >= BUFLEN) r1 = 0;
    float fr = rd - (float)r0;
    float out = dly[s][r0] + (dly[s][r1] - dly[s][r0]) * fr;

    damp[s] += (out - damp[s]) * 0.45f;            // steel is not a wideband medium
    float v = drive + damp[s] * DECAY;

    // The dispersive part. Each section is y = -g*x + z + g*y, a first-order allpass:
    // flat in magnitude, but the delay it adds depends on frequency, and stacking them
    // multiplies that skew until a transient smears into the characteristic sproing.
    for (int k = 0; k < NAP; k++) {
      float z = apz[s][k];
      float y = -g * v + z;
      apz[s][k] = v + g * y;
      v = y;
    }

    dly[s][wr[s]] = softclip(v);
    wr[s] = (wr[s] + 1 >= BUFLEN) ? 0 : wr[s] + 1;
    sum += out;
  }

  float tank = sum * 0.5f;
  tankMon = tank < 0.0f ? -tank : tank;

  float out = in * (1.0f - wet) + tank * wet * 1.6f;

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

  for (int s = 0; s < NSPR; s++) {
    for (int k = 0; k < NAP; k++) apz[s][k] = 0.0f;
    for (int j = 0; j < BUFLEN; j++) dly[s][j] = 0.0f;
    wr[s] = 0; damp[s] = 0.0f;
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

void loop() {
  static int n = 0;
  static bool init = false;
  if (!init) { init = true; delay(10); btnRel = digitalRead(6); gateIdle = digitalRead(7); }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  wet      = x1;                                   // fully dry through to fully wet
  lenScale = 0.15f + 0.85f * x2;                   // 15ms .. 130ms of spring
  // A longer spring disperses more, so the sproing stretches out with the length rather
  // than needing a knob of its own.
  tension  = 0.34f + 0.34f * x2;

  frozen = (digitalRead(7) != gateIdle);           // IN1 holds the tank

  static bool prevG2 = false, prevBtn = false;     // IN2 and the button both kick it
  bool g2  = (digitalRead(0) != gateIdle);
  bool btn = (digitalRead(6) != btnRel);
  if ((g2 && !prevG2) || (btn && !prevBtn)) crash = 900;
  prevG2 = g2; prevBtn = btn;

  float l = tankMon * 3.0f; if (l > 1.0f) l = 1.0f;
  ledSet((int)(l * l * 255.0f));

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    Serial.printf("wet=%3d%% len=%4.1fms tension=%4.2f tank=%4.2f %s\n",
                  (int)(wet * 100.0f), (double)(lenScale * SPRMAX[1] * 1000.0f / FS),
                  (double)tension, (double)tankMon, frozen ? "FROZEN" : "");
  }
  delay(2);
}
