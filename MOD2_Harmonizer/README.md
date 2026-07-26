# Harmonizer

After the Eventide H910, the 1975 box that was the first commercial digital audio effect
and the reason engineers say "glitch".

| Control | |
|---|---|
| POT1 | Pitch, an octave either way in semitones. Centre is unison |
| POT2 | Feedback |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Freeze. Feedback to unity with the input cut, so the cascade runs on its own |
| IN2 | Force a splice. The glitch on demand |
| Button | Steps the delay, 0 / 28 / 56 / 112ms |
| LED | Output level |

Set POT1 to a semitone down and POT2 to about three quarters. Each repeat comes back
shifted again, and the result is the descending cascade the H910 is known for. Feedback
goes just past unity, which is where it stops decaying and starts building.

## How it shifts

Write into a delay line at one rate and read out at another, and the pitch moves. The
catch is that the read pointer then closes on the write pointer and has to jump.

Eventide's answer was two read taps half a window apart, crossfaded into each other at the
jump, the way a tape splice is made. The crossfade pair sums to exactly one, so on paper
the level is constant. In practice the two taps are reading different parts of the
waveform, and wherever they disagree in phase they cancel and the level dips.

That artifact is the sound of the machine, not a defect in this implementation. Eventide
solved it properly years later with autocorrelation in the H949, and some people missed
it.

The splice rate is `|1 - ratio| * Fs / WINDOW`. At an octave with an 84ms window that is
about twelve times a second, which is roughly where the original sat and why octaves are
the grittiest setting on the knob.

## Feedback

The shifter is inside the loop rather than after it, so every repeat is shifted again
rather than repeating at a fixed interval. That is what turns feedback into an arpeggio
here.

## Drift

The H910's master clock was a free-running LC oscillator rather than a crystal, so it
wandered slowly and never quite agreed with itself. That is why the front panel display
flickered between readings, and part of why the box thickens a sound instead of merely
transposing it. A slow random walk on the ratio stands in for it.

Pitch is quantised to semitones, matching the keyboard remote where middle C meant a ratio
of 1.00. Removing the `lroundf` in `loop()` gives the continuous behaviour of the front
panel MANUAL knob instead, which is the setting for detune-thickening rather than harmony.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
