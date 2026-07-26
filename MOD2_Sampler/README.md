# Sampler

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

---

See the [top-level README](../README.md) for hardware notes and build instructions.
