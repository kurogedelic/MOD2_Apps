# Wavefolder

> Not yet tested on hardware. Compiles and runs, but nobody has listened to it.

Where a distortion flattens a waveform against a ceiling, a folder turns it back on itself,
so peaks come back down and the harmonics it adds move around rather than just piling up.
It is the west coast way of getting from a plain tone to a complex one without a filter,
and it wants to be fed something simple: a sine or a triangle folds beautifully, a full mix
turns to mud.

| Control | |
|---|---|
| POT1 | Fold, how hard the signal is driven into the folder |
| POT2 | Symmetry, offsetting the signal so the two halves fold differently |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Trigger an envelope that adds to the fold |
| IN2 | Doubles the fold while held |
| Button | Cycles the folding curve, sine / triangle / clip |
| LED | How much folding is happening |

Symmetry is the reason this app suits this module in particular. Offsetting the signal
before folding decides whether it folds evenly on both halves, and that needs the input to
carry DC. This one does.

IN1 runs an envelope into the fold amount rather than into a filter, which is how a west
coast patch gets its shape. Feed a steady tone in and trigger it, and the timbre moves
instead of the volume.

The three curves are worth comparing directly. Sine is smooth and stays musical at high
drive; triangle reflects rather than curves, giving sharper corners and a brighter, more
aggressive result, which is closer to what a diode folder does in hardware. Clip is in
there for contrast: same drive control, but it flattens instead of folding, and the
difference is immediate.

Folding leaves a DC component whenever symmetry is off centre, so there is a blocker on the
way out; without it the offset would push the output off its midpoint rather than making a
sound.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
