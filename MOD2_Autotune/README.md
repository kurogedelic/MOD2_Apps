# Autotune

> Not yet tested on hardware. Compiles and runs, but nobody has listened to it.

Pitch correction. Work out what note is coming in, decide what note it should have been,
and shift the difference. Wind the speed to zero and it snaps, which is the sound everyone
actually wants from one of these.

| Control | |
|---|---|
| POT1 | Retune speed. Fully down is instant and robotic, up is a slow glide |
| POT2 | Key |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Hold the current target, so the correction stops chasing |
| IN2 | Bypass while held |
| Button | Cycles the scale, chromatic / major / minor / minor pentatonic |
| LED | Lit when a pitch is being tracked, dark when the input is unvoiced |

Feed it something monophonic. Pitch detection on a chord has nothing to find. Output is wet
only, because mixing the dry signal back in would beat against the corrected one.

## Three parts

**Detection** is a YIN-style difference function on a decimated copy of the input. For
every candidate period, sum the squared difference between the signal and a copy of itself
shifted by that much; a real period gives a near zero. Dividing by the running mean is what
stops it picking the octave below, which a plain autocorrelation does constantly. Pitch
lives in the low end, so analysing at a quarter of the sample rate loses nothing and costs
a sixteenth as much.

**Quantising** snaps the detected note to the nearest one the scale allows.

**Shifting** is two read taps crossfading at the splice, the same machinery as
[Harmonizer](../MOD2_Harmonizer). Corrections are small, so the taps barely move and the
splices are rare enough to stay clean.

## A reservation

Autotune may not have much of a job in a rack. The sources around it are oscillators, which
are already in tune, and the input stage here is not especially friendly to a voice. The
same pitch tracking would arguably be better spent on a diatonic harmonizer, where knowing
the input's note is what lets a harmony line hold an interval in key rather than shifting in
parallel and drifting out of it.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
