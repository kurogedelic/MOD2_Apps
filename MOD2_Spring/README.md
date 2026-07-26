# Spring

A spring reverb. Not a hall, not a plate: two coiled springs with transducers at each end,
which is a very particular and very recognisable way to make a sound last.

The bandwidth of this module suits it. The input stage rolls off around 7.2kHz, which
makes a hall sound muffled, but a real spring tank has no top end either. What would be a
limitation elsewhere is close to the correct answer here.

| Control | |
|---|---|
| POT1 | Wet / dry |
| POT2 | Spring length, 15ms to 130ms |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Freeze. Holds the tank ringing and shuts the input out |
| IN2 | Crash. Kicks the tank, the way knocking a real amp does |
| Button | Crash by hand |
| LED | Follows what is ringing in the tank |

Hit the button with nothing patched and the tank sproings on its own. That sound is the
whole point of the algorithm, and it is the quickest way to hear what the length knob is
doing.

## Dispersion

A spring does not pass a transient along in one piece. High frequencies travel faster than
low ones, so a knock at one end arrives at the other smeared into a descending chirp. That
chirp is the sproing, and it is what separates a spring from every other kind of reverb.

A first-order allpass section does the same thing in miniature: flat in magnitude, but the
delay it adds depends on frequency. One section is inaudible. Forty of them in series,
which is what each spring here runs through, multiply that skew until an impulse comes out
the far end as a chirp.

The two delay lines are coprime lengths so their echoes never line up and comb the way a
single resonant tube would, which is also why real tanks use two springs of different
lengths.

Length and dispersion move together on one knob, because a longer spring really is more
dispersive. Winding POT2 out takes the tank from a short bright slap to something long and
slack. The length is interpolated slowly rather than jumping, so turning the knob sounds
like stretching the springs instead of re-pointing a buffer.

`NAP` sets how many allpass sections each spring runs through, and `DECAY` sets how much
survives each trip along it.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
