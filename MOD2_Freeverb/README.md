# Freeverb

Jezar at Dreampoint's Freeverb, the Schroeder-Moorer reverb that ended up in half the free
software of the late nineties. Eight comb filters in parallel build the density, four
allpasses in series smear what is left, and a lowpass inside each comb's feedback loop
takes the top off a little more on every pass.

Where [Spring](../MOD2_Spring) is all dispersion and character, this is the opposite
approach: a room rather than a piece of hardware.

| Control | |
|---|---|
| POT1 | Room size |
| POT2 | Damping, how fast the highs die away relative to the lows |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Freeze |
| IN2 | Kick the tank with a burst, handy for hearing the tail on its own |
| Button | Steps the wet balance, 25 / 50 / 75 / 100% |
| LED | Follows the tail |

Damping is the control worth spending time on. At zero the tail stays bright and rings on;
wound up, the highs disappear first and what is left is soft and round. It is the
difference between a tiled room and one full of curtains.

Freeze is the mode from the original source, not a mute: comb feedback goes to unity and
the damping lowpass is taken out of the loop, so whatever is in the tank keeps circulating
undiminished while the input is cut.

## Sample rate

The published tunings assume 44.1kHz, and this runs at 48.83kHz, so they are scaled by
`48828/44100`. A reverb is defined in time rather than samples: 1116 samples at 44.1kHz is
25.3ms, and so is 1236 at 48.83kHz. Left unscaled the whole room would shrink.

The scaled values stay mutually prime, which is the reason the originals are such odd
numbers in the first place. Sharing a factor would let echoes coincide and comb.

Running at 44.1kHz to use the stock numbers would sound identical and cost something: at
150MHz the sample period would be 3401 cycles, which is not a multiple of the 1024-cycle
PWM carrier, so each sample would be held for three or four carrier periods alternately
and the output would pick up a beat. The current 3072 is exactly three carrier periods.

`FIXEDGAIN` trims the input, `SCALEROOM` and `OFFSETROOM` set the room size range, and
`SCALEDAMP` sets how far damping goes.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
