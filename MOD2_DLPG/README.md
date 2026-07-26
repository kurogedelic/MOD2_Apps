# Digital Low Pass Gate

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

## The vactrol

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

---

See the [top-level README](../README.md) for hardware notes and build instructions.
