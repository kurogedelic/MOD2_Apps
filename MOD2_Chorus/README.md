# Chorus

> Not yet tested on hardware. Compiles and runs, but nobody has listened to it.

A modulated delay line. Vary the length of a short delay and the copy it produces drifts in
pitch against the original; sum the two and the interference between them is the effect.
Which effect depends almost entirely on how short the delay is, so the same structure
covers three of them.

| Control | |
|---|---|
| POT1 | Rate |
| POT2 | Depth |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Reset the sweep, so it can be lined up with a clock |
| IN2 | Inverts the flanger feedback, moving the notches half a comb over |
| Button | Cycles chorus / flanger / vibrato |
| LED | Follows the sweep |

- **Chorus** 20 to 30ms, two taps a quarter cycle apart, no feedback. A single moving tap
  shifts pitch; a second one moving the other way at any given moment is what makes it
  sound like more than one player rather than one detuned one.
- **Flanger** under 10ms with feedback, so the comb notches are close enough together to
  hear as a sweep rather than a blur.
- **Vibrato** one tap, no dry signal at all. Just the pitch moving.

IN1 resetting the sweep is what makes it usable with the rest of a rack: send it a clock
and the sweep starts from the same place every bar instead of wandering against everything
else.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
