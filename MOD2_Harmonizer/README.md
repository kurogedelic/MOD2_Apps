# Harmonizer

A characterful digital pitch shifter with overlapping read taps, splice crossfades and feedback.

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
shifted again, producing a descending cascade. Feedback goes just past unity, which is
where it stops decaying and starts building.

## How it shifts

Write into a delay line at one rate and read out at another, and the pitch moves. The
catch is that the read pointer then closes on the write pointer and has to jump.

This implementation uses two read taps half a window apart, crossfaded into each other at
the jump, like a tape splice. The crossfade pair sums to exactly one, so on paper the
level is constant. In practice the two taps are reading different parts of the waveform,
and wherever they disagree in phase they cancel and the level dips.

That artifact is intentional and is part of this effect's sound.

The splice rate is `|1 - ratio| * Fs / WINDOW`. At an octave with an 84ms window that is
about twelve times a second, which makes octave settings deliberately gritty.

## Feedback

The shifter is inside the loop rather than after it, so every repeat is shifted again
rather than repeating at a fixed interval. That is what turns feedback into an arpeggio
here.

## Drift

A slow random walk on the pitch ratio adds a small amount of clock-like wander so
sustained sounds do not remain perfectly static.

Pitch is quantised to semitones. Removing the `lroundf` in `loop()` gives continuous
pitch control instead, which is useful for detune-thickening rather than harmony.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
