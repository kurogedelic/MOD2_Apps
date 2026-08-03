# MOD2_Apps

Audio apps for [HAGIWO MOD2](https://note.com/solder_state/n/nce8f7defcf98).

MOD2 is a 4HP Eurorack drum module built around a Seeed XIAO RP2350. These sketches
repurpose it as an audio effect processor: the CV input becomes an audio input, and the
PWM output becomes the processed signal.

These are written for this module rather than ported onto it. It has one input, two usable
knobs and two gate jacks, so most effects have to be rethought before they fit: the
granular app spends no control on density and schedules grains for constant overlap
instead, the compressor cannot have an audio sidechain but can have a gate one, and the
spring reverb works precisely because the input rolls off at 7.2kHz. Where a constraint
came up, the notes in each app say what was done about it.

Apps marked *untested* compile and run but have not been listened to on hardware yet.

### Filters and dynamics

| App | |
|---|---|
| [State Variable Filter](MOD2_SVF) | LP, BP, HP and notch, resonance up to Q=30 |
| [Digital Low Pass Gate](MOD2_DLPG) | Buchla-style gate with a simulated vactrol |
| [Compressor](MOD2_Compressor) | Feed-forward, with a gate sidechain — *untested* |

### Distortion

| App | |
|---|---|
| [Wavefolder](MOD2_Wavefolder) | West coast folding, with symmetry — *untested* |

### Delay and modulation

| App | |
|---|---|
| [Delay](MOD2_Delay) | Tap tempo and clock sync, with triplets and dotted ratios |
| [Tape](MOD2_Tape) | Endless tape loop, from plain echo to sound piling up forever |
| [Chorus](MOD2_Chorus) | Chorus, flanger and vibrato from one delay line — *untested* |

### Reverb

| App | |
|---|---|
| [Spring](MOD2_Spring) | Dispersion through forty allpasses per spring |
| [Freeverb](MOD2_Freeverb) | Schroeder-Moorer, room size and damping |

### Pitch

| App | |
|---|---|
| [Harmonizer](MOD2_Harmonizer) | After the Eventide H910, glitch included |
| [Autotune](MOD2_Autotune) | Pitch tracking and correction — *untested* |

### Sampling

| App | |
|---|---|
| [Sampler](MOD2_Sampler) | Auto-triggered recording, played at any speed from either end |
| [Granular](MOD2_Granular) | Rolling buffer of grains, with freeze |

### Utility

| App | |
|---|---|
| [Calibrate](MOD2_Calibrate) | Measures `BIAS` for your module, and checks it is alive |
| [MOD2_UAC](https://github.com/kurogedelic/MOD2_UAC) | USB Audio capture firmware, 48kHz mono input |

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
- **Calibrate `BIAS` per unit.** Every app has a `BIAS` constant, and every one of them is
  wrong until measured: it is the ADC reading with nothing going in, and component
  tolerance moves it by hundreds of counts between modules. Theory says 2332; the unit
  these were written on reads 1937. Build [Calibrate](MOD2_Calibrate), read the figure it
  prints, and copy it into whichever app you want to run.
- **The LED puts noise in the audio.** Its current shares the 3.3V rail with the analog
  front end. Expect it on every app here, and note that it is not only a switching
  artifact: a plain on/off blink is audible on this board too, so the coupling is the
  current itself and no firmware setting removes it.

  The sketches do lock the LED PWM to 146.5kHz, exactly three times the sample rate and
  the same carrier the audio output uses, so the switching folds to DC when the ADC picks
  it up rather than landing somewhere audible. That much is worth having. `analogWrite`
  is not used for it: the core defaults to 1kHz, which is plainly audible, and raising it
  to 100kHz only moves the problem, since anything above Nyquist folds back and 100kHz
  lands at 2.3kHz. Dropping the LED to a plain on/off `digitalWrite` costs the brightness
  metering and still does not make it silent.

## Build

Arduino IDE or `arduino-cli` with the [Earle Philhower core](https://github.com/earlephilhower/arduino-pico).
Board `Seeed XIAO RP2350`, CPU speed **150MHz**.

```
arduino-cli compile -b "rp2040:rp2040:seeed_xiao_rp2350:freq=150,arch=arm" --upload -p /dev/ttyACM0 MOD2_SVF
```

Disconnect the power ribbon cable before plugging in USB.

## See also

- [HAGIWO's build documentation](https://note.com/solder_state/n/nce8f7defcf98) for the
  module itself, and [the VCO firmware](https://note.com/solder_state/n/n2214749e92f2),
  which is the reference for anything involving V/oct.
- [modulove/MOD2](https://github.com/modulove/MOD2) collects the community firmwares,
  drums and otherwise, including ports of Braids and Tides. Modulove also host a
  [browser flasher](https://dl.modulove.io/).
- [wgd-modular](https://github.com/wgd-modular) make MELON, a revised MOD2, and keep
  [their own firmware collection](https://github.com/wgd-modular/melon-firmwares).

## License

MIT
