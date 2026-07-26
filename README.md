# MOD2_Apps

Audio apps for [HAGIWO MOD2](https://note.com/solder_state/n/nce8f7defcf98).

MOD2 is a 4HP Eurorack drum module built around a Seeed XIAO RP2350. These sketches
repurpose it as an audio effect processor: the CV input becomes an audio input, and the
PWM output becomes the processed signal.

- [x] State Variable Filter
- [x] Digital Low Pass Gate
- [x] Tape
- [x] Sampler

## Hardware notes

Common to every app here.

| Function | Pin | |
|---|---|---|
| POT1 | A0 / GPIO26 | |
| POT2 | A1 / GPIO27 | |
| POT3 | A2 / GPIO28 | shared with CV in, acts as input DC bias |
| Audio in | J2 -> A2 | |
| IN1 / IN2 | GPIO7 / GPIO0 | gate and trigger only, not CV |
| Out | GPIO1 | PWM, 10Vpp AC |
| Button | GPIO6 | active low, use `INPUT_PULLUP` |
| LED | GPIO5 | |

- **The Eurorack +/-12V rail is mandatory.** The analog input stage is an op-amp circuit
  that does nothing on USB power alone, which pins the ADC to a constant and makes the
  input appear dead. Power the module from the rack when testing audio.
- Set the **rear jumper to the MCU side** (higher cutoff) for audio use.
- Input stage rolls off at about 7.2kHz, one pole. That doubles as a mild anti-alias
  filter, so do not lower the sample rate below ~48kHz.
- Useful input range is roughly **+/-2.1V**, asymmetric, clipping the negative side first.
  Line level or an attenuated Eurorack signal works well; a raw 10Vpp modular signal clips.
- **Calibrate `BIAS` per unit.** It is the no-signal ADC center, and component tolerance
  moves it. Open the serial monitor at 115200 with nothing patched and POT3 centered, then
  copy the reported center value into the `BIAS` define.
- **The LED puts noise in the audio.** It shares the 3.3V rail with the analog front end,
  and the apps that dim it do so with `analogWrite`, whose default frequency in this core
  is 1kHz, right in the middle of the audio band. Expect it on every app here. Raising the
  LED PWM well above hearing with `analogWriteFreq(100000)` in `setup()` takes care of the
  worst of it; dropping back to a plain on/off `digitalWrite` removes the switching
  entirely, at the cost of the brightness metering.

## Build

Arduino IDE or `arduino-cli` with the [Earle Philhower core](https://github.com/earlephilhower/arduino-pico).
Board `Seeed XIAO RP2350`, CPU speed **150MHz**.

```
arduino-cli compile -b "rp2040:rp2040:seeed_xiao_rp2350:freq=150,arch=arm" --upload -p /dev/ttyACM0 MOD2_SVF
```

Disconnect the power ribbon cable before plugging in USB.

## State Variable Filter

A TPT state-variable filter (Zavalishin / Cytomic topology) running at 48.83kHz.

| Control | |
|---|---|
| POT1 | Cutoff, 40Hz - 6kHz, log |
| POT2 | Resonance, Q 0.5 - 30, log |
| POT3 | Input DC bias. Leave centered, it is not a filter parameter |
| Button | Cycles LP -> BP -> HP -> Notch. The LED blinks the mode index plus one |
| LED | Signal present indicator |

Resonance goes deliberately far, up to Q=30. Gain compensation keeps a floor at 0.3 so
high Q settings stay audible instead of collapsing in level.

Implementation notes: pots are sampled by stealing one ADC slot every 256 samples, so the
audio stream never drops a sample. Coefficients are computed in `loop()` to keep `tanf`
and `powf` out of the interrupt. Output is 10-bit PWM at a 146.5kHz carrier with
second-order error feedback to recover the resolution lost against a slower carrier.

## Digital Low Pass Gate

A Buchla-style low pass gate. A single envelope opens amplitude and brightness together,
so notes bloom in and decay with the highs falling away first, the way a struck object
does. It is not a VCA with a filter bolted on: the two move as one, and the character
comes from the sluggish vactrol between them.

| Control | |
|---|---|
| IN1 / IN2 | Trigger or gate, OR'd with the button |
| Button | Manual trigger. Tap to ping, hold to sustain. Hold past 1.2s to change mode |
| POT1 | Decay, 20ms - 4s, log |
| POT2 | Timbre. How far open the gate travels, from a dark thud near 150Hz to a bright snap at 6kHz |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| LED | Follows the envelope, like the lamp inside the vactrol |

Three modes, the classic Buchla 292 arrangement:

- **LPG** envelope drives cutoff and amplitude
- **VCA** amplitude only, filter stays open
- **LPF** cutoff only, no amplitude control

Switching between LPG and VCA on the same decay setting is the quickest way to hear what
a low pass gate actually does.

### The vactrol

An LED shining on a photoresistor does not follow its drive current. It rises in a couple
of milliseconds but falls far more slowly, and the fall is not a single exponential: the
cell has a fast component and a much slower one that keeps the gate cracked open long
after the note should have ended. That lingering tail is most of what separates a low pass
gate from a VCA, so it is modelled as two decays summed rather than one.

Amplitude then runs through an exponential taper. A linear fade sounds like a volume knob
being turned down, because hearing is logarithmic: the top of the fall goes unnoticed and
the bottom drags. `ATK_MS`, `SLOW_MUL`, `SLOW_MIX` and `EXP_K` at the top of the sketch
control the feel and are worth experimenting with.

Filter Q is fixed at 0.707. A real 292 has no resonance, and any peak reads as a ring on
every hit.

Implementation note: the cutoff moves every sample, so `tanf` cannot run in the interrupt.
Filter coefficients and the amplitude curve are both precomputed into tables at boot and
interpolated, which keeps the ISR to plain arithmetic.

## Tape

An endless loop of tape running past three heads. Sound is laid down, comes back around,
and is laid down again on top of itself. How much survives each lap is up to the erase
head, and that one control spans everything from a plain echo to a loop that never
forgets.

| Control | |
|---|---|
| POT1 | Tape speed |
| POT2 | Erase head |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Tape stop |
| IN2 | Reverse |
| Button | Tap lifts the record head so the loop plays on untouched. Hold past 1.2s to erase the tape |
| LED | Lit while recording, dim while held |

360KB of SRAM holds the tape: 3.7 seconds at nominal speed, close to 15 at the slowest.

**Tape speed** moves pitch and loop length together, because on a real machine they are
the same thing. Slowing the transport drags everything already recorded down with it, and
a slow tape genuinely loses detail here rather than pretending to: at low speed many input
samples average onto one spot of tape, which is the same reason a real machine sounds
duller slowed down.

**The erase head** is the interesting one. At full it wipes each lap clean and the module
is an echo. Backed off, old material survives a little longer each time around. At zero
nothing is erased at all and sound piles up indefinitely, held in check only by tape
saturation, which is how a Frippertronics rig behaves once the loop closes.

Wow and flutter are always running, the feedback path loses a little top end every lap,
and the whole thing soft clips rather than hitting a ceiling. `WOW_AMT`, `FLUT_AMT`,
`MOTOR_MS`, `SPD_MIN`, `SPD_MAX` and `DRY` at the top of the sketch set the feel.

Implementation note: erase-all sweeps a second head across the entire buffer instead of
waiting for the transport to carry every spot past the record head, which at low speed
would take most of a minute.

## Sampler

Record a sound, then fire it back with a gate. 3.7 seconds of memory, played at any speed
from either end.

| Control | |
|---|---|
| POT1 | Pitch, quarter speed to four times, which moves length with it |
| POT2 | Start point, anywhere in the first 90% of the sample |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Trigger, plays from the start point |
| IN2 | Same, but backwards |
| Button | Click to play. Hold past 1.2s to arm, click again to end the take |
| LED | Blinks while armed, follows the input while recording, lit during playback |

Recording arms rather than starts. Hold the button and the LED blinks, waiting; the take
begins the moment something crosses the threshold, so the attack lands at the head of the
sample instead of behind a stretch of silence. Clicking again ends it, or backs out if
nothing ever arrived. There is no erase, because arming again is all it takes to replace
what is there.

Playback fades in and out over 96 samples. Starting from the middle of a waveform is a
step edge, and a click on every hit is the fastest way to make a sampler sound cheap.

Note that POT2 is live: a sample played with the knob at the halfway mark starts halfway
in, which reads as a missing first second until you know to expect it. `THRESH` at the top
of the sketch sets how loud the input has to get to begin a take.

## License

MIT
