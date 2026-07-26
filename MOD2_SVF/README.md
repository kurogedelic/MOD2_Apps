# State Variable Filter

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

---

See the [top-level README](../README.md) for hardware notes and build instructions.
