# Tape

An endless loop of tape running past three heads. Sound is laid down, comes back around,
and is laid down again on top of itself. How much survives each lap is up to the erase
head, and that one control spans everything from a plain echo to a loop that never
forgets.

| Control | |
|---|---|
| POT1 | Tape speed |
| POT2 | Erase head |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Tape stop |
| IN2 | Reverse |
| Button | Tap lifts the record head so the loop plays on untouched. Hold past 1.2s to erase the tape |
| LED | Lit while recording, dim while held |

360KB of SRAM holds the tape: 3.7 seconds at nominal speed, close to 15 at the slowest.

**Tape speed** moves pitch and loop length together, because on a real machine they are
the same thing. Slowing the transport drags everything already recorded down with it, and
a slow tape genuinely loses detail here rather than pretending to: at low speed many input
samples average onto one spot of tape, which is the same reason a real machine sounds
duller slowed down.

**The erase head** is the interesting one. At full it wipes each lap clean and the module
is an echo. Backed off, old material survives a little longer each time around. At zero
nothing is erased at all and sound piles up indefinitely, held in check only by tape
saturation, which is how a Frippertronics rig behaves once the loop closes.

Wow and flutter are always running, the feedback path loses a little top end every lap,
and the whole thing soft clips rather than hitting a ceiling. `WOW_AMT`, `FLUT_AMT`,
`MOTOR_MS`, `SPD_MIN`, `SPD_MAX` and `DRY` at the top of the sketch set the feel.

Implementation note: erase-all sweeps a second head across the entire buffer instead of
waiting for the transport to carry every spot past the record head, which at low speed
would take most of a minute.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
