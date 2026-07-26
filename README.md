# MOD2_Apps

Audio apps for [HAGIWO MOD2](https://note.com/solder_state/n/nce8f7defcf98).

MOD2 is a 4HP Eurorack drum module built around a Seeed XIAO RP2350. These sketches
repurpose it as an audio effect processor: the CV input becomes an audio input, and the
PWM output becomes the processed signal.

| App | |
|---|---|
| [State Variable Filter](MOD2_SVF) | LP, BP, HP and notch, resonance up to Q=30 |
| [Digital Low Pass Gate](MOD2_DLPG) | Buchla-style gate with a simulated vactrol |
| [Tape](MOD2_Tape) | Endless tape loop, from plain echo to sound piling up forever |
| [Sampler](MOD2_Sampler) | Auto-triggered recording, played at any speed from either end |
| [Spring](MOD2_Spring) | Spring reverb, dispersion and all |

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
- **The LED puts noise in the audio.** Its current shares the 3.3V rail with the analog
  front end, and the apps that dim it do so with `analogWrite`. Expect it on every app
  here.

  The sketches raise the LED PWM to 100kHz, which helps less than it sounds like it
  should: the ADC runs at 48.83kHz, so anything above Nyquist folds back, and 100kHz lands
  at `|100000 - 2*48828|` = 2.3kHz. The switching noise moves rather than leaves. Locking
  the LED PWM to a multiple of the sample rate (48828 or 146484Hz, the same trick the audio
  carrier uses) would fold it to DC instead; a plain on/off `digitalWrite` avoids the
  question entirely at the cost of the brightness metering.

## Build

Arduino IDE or `arduino-cli` with the [Earle Philhower core](https://github.com/earlephilhower/arduino-pico).
Board `Seeed XIAO RP2350`, CPU speed **150MHz**.

```
arduino-cli compile -b "rp2040:rp2040:seeed_xiao_rp2350:freq=150,arch=arm" --upload -p /dev/ttyACM0 MOD2_SVF
```

Disconnect the power ribbon cable before plugging in USB.

## License

MIT
