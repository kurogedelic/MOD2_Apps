# Calibrate

Every app in this collection has a `BIAS` constant near the top, and every one of them is
wrong until you measure it. This is the sketch that measures it.

1. Patch nothing into the input and centre POT3.
2. Open the serial monitor at 115200.
3. Read the `BIAS` figure once it settles, and copy it into the `BIAS` define of whichever
   app you are building.

```
min=1919 max=1957 pp=  38   BIAS = 1938
```

## Why it needs measuring

`BIAS` is the ADC reading with nothing going in. It should sit near the middle of the
range, but the analog front end is an op-amp summing circuit fed from the -12V rail through
ordinary resistors, so component tolerance moves it by a few hundred counts from one module
to the next. Theory says 2332 on this hardware; the unit these apps were written on reads
1937.

Getting it wrong does not stop audio passing, since a DC blocker follows. It eats headroom
asymmetrically instead: the further the resting point is from centre, the sooner one half
of the waveform clips.

## As a test

It also passes audio straight through, so it doubles as a way to check the module is alive.
`pp` is the peak-to-peak of the raw input: a few tens with nothing patched is the noise
floor, and a signal should push it into the thousands.

If `BIAS` sits far outside 1900-2400 and POT3 does not move it, the analog front end has no
power. That means the Eurorack rail is missing, which is the usual cause: USB alone runs the
microcontroller but not the op-amps in front of it, and the symptom is an input that looks
completely dead.

---

See the [top-level README](../README.md) for hardware notes and build instructions.
