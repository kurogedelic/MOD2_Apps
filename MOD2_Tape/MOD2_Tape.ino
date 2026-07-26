/* HAGIWO MOD2 / Tape v1.0  (Seeed XIAO RP2350)
 *
 * An endless loop of tape running past three heads. Sound is laid down, comes back around,
 * and is laid down again on top of itself. How much survives each lap is up to the erase
 * head: wide open it is a plain delay, closed it is a loop that never forgets.
 *
 *   POT1   : tape speed. Like a real machine, this moves pitch and loop length together,
 *            so slowing the transport drags everything already on the tape down with it.
 *   POT2   : erase head. 0 leaves the tape untouched and sound piles up until it
 *            saturates; full wipes each lap clean, leaving a straight echo.
 *   POT3   : input DC bias - keep centered, not a parameter
 *   IN1    : tape stop. The motor has inertia, so pitch sags on the way down and spins
 *            back up on release.
 *   IN2    : reverse.
 *   BUTTON : tap lifts the record head, so the loop plays on untouched. Hold past 1.2s
 *            to erase the tape.
 *   LED    : lit while recording, dim while held
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
#define TAPELEN 180000    // 360KB of SRAM, 3.7s of tape at nominal speed
#define OUTLVL 0.5f       // output level
#define DRY    0.4f       // how much of the live input passes straight through

/* Tape speed is the length of the loop and its pitch at once, because it is one
 * transport. 0.25 stretches the 3.7s loop out to nearly 15 seconds two octaves down.
 */
#define SPD_MIN 0.25f
#define SPD_MAX 2.0f
#define MOTOR_MS 120.0f   // how long the capstan takes to reach speed, for tape stop
#define WOW_HZ  0.7f      // slow drift
#define WOW_AMT 0.004f
#define FLUT_HZ 11.0f     // faster shimmer, shallower
#define FLUT_AMT 0.0015f

// --- the tape itself ---
static int16_t tape[TAPELEN];
static float pos = 0.0f;            // fractional head position, in samples along the tape
static float speed = 1.0f;          // current transport speed, smoothed toward the target

// --- DSP state ---
static float dcx, dcy, e1, e2;      // input DC blocker + output noise shaper
static float fbLp = 0.0f;           // one pole in the feedback path, the tape losing highs
static float fbDc = 0.0f;           // and a slow leak so DC cannot pile up over laps
// --- written by loop(), read by the ISR (32-bit float access is atomic on M33)
static volatile float tgtSpeed = 1.0f, erase = 0.5f, motorC = 0.01f;
static volatile int pot1raw = 0, pot2raw = 0;
static volatile bool recOn = true, wiping = false;
static volatile uint32_t wipePtr = 0;
static volatile float levelMon = 0.0f;

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

// Cubic soft clip. Tape does not have a hard ceiling, it leans into one, and with the
// erase head shut that lean is the only thing keeping an infinite loop bounded.
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

  // --- transport ---
  // The capstan has mass. Speed eases toward its target instead of jumping, which is what
  // makes a tape stop sag in pitch rather than simply mute.
  speed += (tgtSpeed - speed) * motorC;

  // --- play head ---
  int   i0 = (int)pos;
  int   i1 = i0 + 1; if (i1 >= TAPELEN) i1 = 0;
  float fr = pos - (float)i0;
  float v  = ((float)tape[i0] + ((float)tape[i1] - (float)tape[i0]) * fr) * (1.0f / 32767.0f);

  // --- erase and record heads ---
  // Everything the play head just found is attenuated by the erase head, the live input is
  // added, and the sum goes straight back down onto the same spot of tape.
  float w = v * (1.0f - erase);
  if (recOn) w += in;
  fbLp += (w - fbLp) * 0.75f;                      // the tape loses a little top each lap
  w = fbLp;
  fbDc += (w - fbDc) * 0.0004f;                    // and cannot be allowed to drift to DC
  w = softclip(w - fbDc);

  // How far the tape moved this tick decides how much of it gets written. Slow transport
  // means many input samples average onto one spot, which is exactly why a slow tape
  // sounds duller. Fast transport smears one sample across several spots instead.
  float sp = speed;
  float adv = sp < 0.0f ? -sp : sp;
  float a = adv < 1.0f ? adv : 1.0f;
  int   q16 = (int)(w * 32767.0f);
  if (q16 >  32767) q16 =  32767;
  if (q16 < -32768) q16 = -32768;
  tape[i0] = (int16_t)((float)tape[i0] * (1.0f - a) + (float)q16 * a);
  if (adv > 1.0f) {                                // fill the spot we would have skipped
    int i2 = i0 + (sp > 0.0f ? 1 : -1);
    if (i2 >= TAPELEN) i2 = 0; else if (i2 < 0) i2 = TAPELEN - 1;
    tape[i2] = (int16_t)q16;
  }

  pos += sp;
  if (pos >= (float)TAPELEN) pos -= (float)TAPELEN;
  else if (pos < 0.0f)       pos += (float)TAPELEN;

  // Erase-all runs a second head along the whole loop rather than waiting for the
  // transport to carry every spot past the record head, which at a slow speed would take
  // the better part of a minute. At this rate the sweep finishes in about a quarter
  // second, so the loop audibly hollows out instead of cutting to silence.
  if (wiping) {
    uint32_t p = wipePtr;
    for (int k = 0; k < 16 && p < TAPELEN; k++) tape[p++] = 0;
    wipePtr = p;
    if (p >= TAPELEN) wiping = false;
  }

  // Below a crawl there is not enough tape moving past the head to make a sound, so the
  // output falls away with the transport rather than freezing on a single sample.
  float fade = adv * 4.0f; if (fade > 1.0f) fade = 1.0f;
  float out = v * fade + in * DRY;
  levelMon = out < 0.0f ? -out : out;

  float t = 512.0f + out * 512.0f * OUTLVL + 1.5f * e1 - 0.5f * e2;  // 2nd-order error FB
  int qq = (int)(t + 0.5f);
  if (qq < 0) qq = 0; else if (qq > 1023) qq = 1023;
  e2 = e1; e1 = t - (float)qq;
  pwm_hw->slice[SL_AUD].cc = (uint32_t)qq << 16;
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

  for (int i = 0; i < TAPELEN; i++) tape[i] = 0;   // blank tape

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
  irq_set_exclusive_handler(PWM_IRQ_WRAP_0, isr);
  irq_set_enabled(PWM_IRQ_WRAP_0, true);
  hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);
}

static int btnRel = 1, gateIdle = 0;

void loop() {
  static int n = 0;
  static float wowPh = 0.0f, flutPh = 0.0f;
  static bool init = false;
  if (!init) {                                     // sample the idle levels once, late
    init = true; btnRel = digitalRead(6); gateIdle = digitalRead(7);
  }

  float x1 = pot1raw * (1.0f / 4095.0f);
  float x2 = pot2raw * (1.0f / 4095.0f);

  // Wow and flutter. Nothing mechanical holds a constant speed, and the wobble is most of
  // what separates tape from a digital buffer.
  wowPh  += 2.0f * (float)M_PI * WOW_HZ  * 0.002f; if (wowPh  > 6.2832f) wowPh  -= 6.2832f;
  flutPh += 2.0f * (float)M_PI * FLUT_HZ * 0.002f; if (flutPh > 6.2832f) flutPh -= 6.2832f;
  float wobble = 1.0f + WOW_AMT * sinf(wowPh) + FLUT_AMT * sinf(flutPh);

  float base = SPD_MIN * powf(SPD_MAX / SPD_MIN, x1);   // 0.25 .. 2.0, log
  bool stopped = (digitalRead(7) != gateIdle);          // IN1 halts the transport
  bool rev     = (digitalRead(0) != gateIdle);          // IN2 flips direction
  tgtSpeed = stopped ? 0.0f : (base * wobble * (rev ? -1.0f : 1.0f));
  motorC   = 1.0f - expf(-1.0f / (0.001f * MOTOR_MS * FS));

  erase = x2;                                      // 0 = keep everything, 1 = wipe each lap

  // --- button: tap lifts the record head, long hold erases the tape ---
  static bool wasDown = false;
  static int  heldMs = 0;
  static bool consumed = false;
  bool down = (digitalRead(6) != btnRel);
  if (down) {
    if (!wasDown) { heldMs = 0; consumed = false; }
    heldMs += 2;
    if (heldMs > 1200 && !consumed) {
      consumed = true;
      wipePtr = 0; wiping = true;                  // the ISR sweeps the erase head across
      for (int i = 0; i < 3; i++) { ledSet(255); delay(80);
                                    ledSet(0);  delay(80); }
      Serial.println(">> TAPE ERASED");
    }
  } else {
    if (wasDown && !consumed) {                    // a tap, not a hold
      recOn = !recOn;
      Serial.printf(">> REC %s\n", recOn ? "ON" : "HOLD");
    }
  }
  wasDown = down;

  if (!wiping) {
    float l = levelMon * 2.0f; if (l > 1.0f) l = 1.0f;
    ledSet((int)(l * (recOn ? 255.0f : 60.0f)));   // dim while the loop is held
  }

  if (++n >= 250) {                                // log roughly twice a second
    n = 0;
    float sp = base * (rev ? -1.0f : 1.0f);
    Serial.printf("[%s] speed=%+5.2f loop=%5.2fs erase=%4.2f %s%s\n",
                  recOn ? "REC " : "HOLD", (double)sp,
                  (double)((float)TAPELEN / (base * FS)), (double)erase,
                  stopped ? "STOP " : "", rev ? "REV" : "");
  }
  delay(2);
}
