# Klattsch

> Beta. Ready to flash and try. A listen on the module is still open.

A parallel-formant voice. A glottal pulse (or noise, for a whisper) feeds three
bandpass formants, which is enough to read as a vowel. The module has two knobs,
so pitch and the vowel take them, and speed only appears once a phrase is
selected and the vowel is no longer yours to move.

| Control | |
|---|---|
| POT1 | Pitch, 50Hz to 400Hz, logarithmic |
| POT2 | Live: vowel, O U A E I. On a phrase: phoneme length, 45ms to 240ms |
| POT3 | Input DC bias. Leave centered, it is not a parameter |
| IN1 | Gate. See below |
| IN2 | Formant scale, +25% while held. A smaller vocal tract, brighter |
| Button | Tap cycles the program. Hold past 1.2s for voiced, breathy, whisper |
| LED | Output level |

Programs, in tap order:

1. **Vowel.** The voice drones, so a module with nothing patched still speaks. The first IN1 gate latches level to the jack: high sounds, low releases over 50ms. A rising edge restarts the glottal cycle, which is the attack. Power-cycle to get the drone back.
2. **HELLO** — `HH AH L OW`
3. **SPEAK** — `S P IY K`
4. **AIUEO** — `A I U E O`, the Japanese vowels in the usual order
5. **DID** — `D IH D DH AE T`

On a phrase, IN1's rising edge plays it once. Holding the gate loops it. POT1 still pitches it while it runs. Choosing the program plays it once, so the button is enough to hear which line you landed on.

The live vowel walks O → U → A → E → I. Second formant rises from the back of the mouth to the front, with the open A in the middle. Those five are Mokhtari and Tanaka's Japanese measurements. AIUEO is the same five vowels in textbook order, which is why the phrase and the knob do not trace the same path.

Voiced is the plain pulse. Breathy keeps the pulse and mixes noise under it. Whisper is noise through the same formants; the pitch comes back when you return to voiced, because a whisper has no glottal cycle to tune.

## What had to fit

The sample rate is 48.83kHz, same timebase as the other apps, 3072 cycles at 150MHz. The original synthesizer recomputes `sin` and `cos` for every formant whenever one moves. That stays out of the interrupt. A 1025-point sine table is filled in `setup()`, the glottal pulse reads it, and the bandpass coefficients are rebuilt only after a formant has moved by more than about 0.75Hz. Steady vowels reuse the previous coefficients. Float throughout: this build is the Arm core, which has an FPU, and every other sketch here is float too.

There is no heap and no sample buffer. The sine table and the phoneme rows are a few kilobytes of SRAM. Denormals are flushed so a quiet tail cannot stall the sample period.

The audio jack is still converted, because the pot reads borrow ADC slots from that conversion, the same way the other apps do. The sample itself is thrown away. `BIAS` is declared and unused.

Stops follow the upstream timing: a closure, then a burst no longer than 25ms. Diphthongs (`OW`, `IY`, `IH`, `AE`) glide to their endpoint across the middle half of the slot.

Left out, relative to the browser instrument: the text compiler, vibrato and tremolo, extra formants, and the rest of the phoneme inventory. The lines above are the whole repertoire.

`OUTLVL`, `F0_LO`, `SLOT_LO`, `SLOT_HI` and `SCALE_HI` at the top of the sketch set the ranges.

## Credit

The voice is [klattsch](https://github.com/tgies/klattsch) by Tony Gies, MIT licensed. The pulse, the noise source, the three bandpasses, the soft clip and the phoneme rows come from there (banks `klatt1980-en` and `ja-mokhtari-2000`; English frequencies from Klatt, D.H. (1980), JASA 67(3), amplitudes as fitted in that bank for a three-formant parallel synth). The embedded layout of the sample routine follows the C port in [klattsch-pd-test](https://github.com/kurogedelic/klattsch-pd-test). The MIT copyright and permission text is in [`klattsch_core.h`](klattsch_core.h).

---

See the [top-level README](../README.md) for hardware notes and build instructions.
