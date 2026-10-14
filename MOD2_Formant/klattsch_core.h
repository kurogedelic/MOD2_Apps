/* Parallel-formant voice, for the MOD2 Formant sketch.
 *
 * Derived from Tony Gies's klattsch (https://github.com/tgies/klattsch): the
 * Rosenberg glottal pulse, xorshift noise, three constant-skirt-gain bandpass
 * biquads and the soft clip live here, along with the phoneme rows this app
 * actually speaks. The embedded shape of the sample routine follows the C port
 * in kurogedelic/klattsch-pd-test. Formant numbers are the bundled banks
 * klatt1980-en (Klatt 1980, amplitudes fitted by klattsch for three formants)
 * and ja-mokhtari-2000 (Mokhtari & Tanaka 2000).
 *
 * What the audio interrupt is allowed to do is fixed by the other sketches in
 * this repo. sinf() stays in klattsch_tables_init(), which runs once from
 * setup(). The interrupt reads a sine table. Bandpass coefficients are rebuilt
 * only after a formant has moved by more than about 0.75Hz, because a vowel
 * glide is tens of milliseconds long and a fresh RBJ solve every sample is
 * pure cost. Nothing here allocates.
 *
 * MIT License
 *
 * Copyright (c) 2026 Tony Gies
 * Copyright (c) 2026 Klattsch for Playdate contributors
 * Copyright (c) 2026 Leo Kuroshita
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <stdint.h>
#include <string.h>
#include <math.h>

/* always_inline so the sample routine lands in the ISR, which is copied to
 * RAM. An out-of-line copy would run from flash and miss the cache mid-sample. */
#if defined(__GNUC__)
#define KLATTSCH_INLINE static inline __attribute__((always_inline))
#else
#define KLATTSCH_INLINE static inline
#endif

#define KLATTSCH_SIN_N 1025
#define KLATTSCH_PHRASE_COUNT 4

enum {
  PH_O = 0, PH_U, PH_A, PH_E, PH_I,
  PH_HH, PH_AH, PH_L, PH_OW,
  PH_S, PH_P, PH_IY, PH_K,
  PH_D, PH_IH, PH_DH, PH_AE, PH_T,
  PH_N
};

enum { PH_F_STOP = 1, PH_F_GLIDE = 2 };

typedef struct {
  float voicing;
  float f[3];
  float bw[3];
  float amp[3];
  float gf[3];
  uint8_t flags;
} KlattschPhone;

/* Formant target. Pitch, effort, aspiration and tilt are performance controls
 * and stay outside this struct so a phrase ramp cannot fight the knobs. */
typedef struct {
  float voicing;
  float f[3];
  float bw[3];
  float amp[3];
} KlattschFormants;

typedef struct {
  float x1, x2, y1, y2;
  float b0, b2, a1, a2;
  float last_f, last_bw;
} KlattschBiquad;

typedef struct {
  float sr;
  KlattschFormants current, target, step;
  int transition_remaining;
  float glottal_phase;
  float tilt_previous;
  float f0, aspiration, effort, tilt, gain;
  uint32_t noise_state;
  KlattschBiquad formant[3];
} KlattschSynth;

typedef struct {
  int phrase;
  int index;
  int part;
  int left;
  int run; /* 0 idle, 1 playing, 2 releasing into silence */
} KlattschPhrase;

#define PHONE(v, f1, f2, f3, b1, b2, b3, a1, a2, a3, g1, g2, g3, fl) \
  { (v), { (f1), (f2), (f3) }, { (b1), (b2), (b3) }, { (a1), (a2), (a3) }, \
    { (g1), (g2), (g3) }, (uint8_t)(fl) }

/* Japanese vowels are Mokhtari & Tanaka 2000. English rows are Klatt 1980 as
 * published in klattsch's klatt1980-en bank. gf[] is the diphthong endpoint;
 * flags mark stops (closure then a short burst) and glides. */
static const KlattschPhone KLATTSCH_PHONE_ROM[PH_N] = {
  PHONE(1.00f, 460,  857, 2405,  61, 114,  134, 1.0f, 0.9f, 0.7f,    0,    0,    0, 0),             /* O  */
  PHONE(1.00f, 355, 1282, 2233,  55, 129,  135, 1.0f, 0.9f, 0.7f,    0,    0,    0, 0),             /* U  */
  PHONE(1.00f, 744, 1240, 2426, 182, 117,  222, 1.0f, 0.9f, 0.7f,    0,    0,    0, 0),             /* A  */
  PHONE(1.00f, 480, 1857, 2437,  65,  97,  257, 1.0f, 0.9f, 0.7f,    0,    0,    0, 0),             /* E  */
  PHONE(1.00f, 298, 2083, 2954,  67, 130,  145, 1.0f, 0.9f, 0.7f,    0,    0,    0, 0),             /* I  */
  PHONE(0.00f, 500, 1500, 2500, 300, 200,  300, 0.4f, 0.4f, 0.3f,    0,    0,    0, 0),             /* HH */
  PHONE(1.00f, 620, 1220, 2550,  80,  50,  140, 1.0f, 0.9f, 0.7f,    0,    0,    0, 0),             /* AH */
  PHONE(1.00f, 310, 1050, 2880,  50, 100,  280, 0.8f, 0.7f, 0.5f,    0,    0,    0, 0),             /* L  */
  PHONE(1.00f, 540, 1100, 2300,  80,  70,   70, 1.0f, 0.9f, 0.7f,  450,  900, 2300, PH_F_GLIDE),    /* OW */
  PHONE(0.00f, 320, 1390, 5500, 200, 200, 1000, 0.0f, 0.0f, 0.95f,   0,    0,    0, 0),             /* S  */
  PHONE(0.00f, 400, 1100, 2150, 300, 150,  220, 0.1f, 0.2f, 0.25f,   0,    0,    0, PH_F_STOP),     /* P  */
  PHONE(1.00f, 310, 2020, 2960,  45, 200,  400, 1.0f, 0.9f, 0.7f,  290, 2070, 2960, PH_F_GLIDE),    /* IY */
  PHONE(0.00f, 300, 1990, 2850, 250, 160,  330, 0.0f, 0.5f, 0.4f,    0,    0,    0, PH_F_STOP),     /* K  */
  PHONE(0.60f, 200, 1600, 2600,  60, 100,  170, 0.5f, 0.4f, 0.5f,    0,    0,    0, PH_F_STOP),     /* D  */
  PHONE(1.00f, 400, 1800, 2570,  50, 100,  140, 1.0f, 0.9f, 0.7f,  470, 1600, 2600, PH_F_GLIDE),    /* IH */
  PHONE(0.45f, 270, 1290, 2540,  80, 100,  800, 0.4f, 0.1f, 0.2f,    0,    0,    0, 0),             /* DH */
  PHONE(1.00f, 620, 1660, 2430,  70, 150,  320, 1.0f, 0.9f, 0.7f,  650, 1490, 2470, PH_F_GLIDE),    /* AE */
  PHONE(0.00f, 400, 1600, 2600, 300, 120,  250, 0.0f, 0.3f, 0.55f,   0,    0,    0, PH_F_STOP),     /* T  */
};

#undef PHONE

/* Knob order walks F2 from the back of the mouth toward the front. */
static const uint8_t KLATTSCH_VOWEL_PATH[5] = { PH_O, PH_U, PH_A, PH_E, PH_I };

static const uint8_t KLATTSCH_PH_HELLO[] = { PH_HH, PH_AH, PH_L, PH_OW };
static const uint8_t KLATTSCH_PH_SPEAK[] = { PH_S, PH_P, PH_IY, PH_K };
static const uint8_t KLATTSCH_PH_AIUEO[] = { PH_A, PH_I, PH_U, PH_E, PH_O };
static const uint8_t KLATTSCH_PH_DID[]   = { PH_D, PH_IH, PH_D, PH_DH, PH_AE, PH_T };

static const uint8_t *const KLATTSCH_PHRASE_ID[KLATTSCH_PHRASE_COUNT] = {
  KLATTSCH_PH_HELLO, KLATTSCH_PH_SPEAK, KLATTSCH_PH_AIUEO, KLATTSCH_PH_DID
};
static constexpr uint8_t KLATTSCH_PHRASE_LEN[KLATTSCH_PHRASE_COUNT] = {
  4, 4, 5, 6
};
static const char *const KLATTSCH_PHRASE_NAME[KLATTSCH_PHRASE_COUNT] = {
  "HELLO", "SPEAK", "AIUEO", "DID"
};

static_assert(sizeof(KLATTSCH_PHONE_ROM) / sizeof(KLATTSCH_PHONE_ROM[0]) == PH_N, "phoneme count");
static_assert(sizeof(KLATTSCH_PH_HELLO) == KLATTSCH_PHRASE_LEN[0], "HELLO length");
static_assert(sizeof(KLATTSCH_PH_SPEAK) == KLATTSCH_PHRASE_LEN[1], "SPEAK length");
static_assert(sizeof(KLATTSCH_PH_AIUEO) == KLATTSCH_PHRASE_LEN[2], "AIUEO length");
static_assert(sizeof(KLATTSCH_PH_DID) == KLATTSCH_PHRASE_LEN[3], "DID length");

static float klattsch_sin[KLATTSCH_SIN_N];
static KlattschPhone klattsch_phones[PH_N];

KLATTSCH_INLINE void klattsch_tables_init(void) {
  const float w = 6.28318530718f / (float)(KLATTSCH_SIN_N - 1);
  for (int i = 0; i < KLATTSCH_SIN_N; i++) klattsch_sin[i] = sinf(w * (float)i);
  memcpy(klattsch_phones, KLATTSCH_PHONE_ROM, sizeof(KLATTSCH_PHONE_ROM));
}

KLATTSCH_INLINE float klattsch_sin_at(float turns) {
  turns -= (float)(int)turns;
  if (turns < 0.0f) turns += 1.0f;
  float fi = turns * (float)(KLATTSCH_SIN_N - 1);
  int i = (int)fi;
  if (i >= KLATTSCH_SIN_N - 1) return klattsch_sin[0];
  return klattsch_sin[i] + (klattsch_sin[i + 1] - klattsch_sin[i]) * (fi - (float)i);
}

KLATTSCH_INLINE void klattsch_synth_init(KlattschSynth *s, float sr) {
  memset(s, 0, sizeof(*s));
  s->sr = sr;
  s->noise_state = 0xACE1ACE1u;
  s->f0 = 120.0f;
  s->gain = 3.5f;
  s->effort = 0.5f;
  for (int i = 0; i < 3; i++) s->formant[i].last_f = s->formant[i].last_bw = -1.0f;
}

KLATTSCH_INLINE void klattsch_set_formants(KlattschSynth *s, const KlattschFormants *t, int n) {
  if (n < 1) n = 1;
  s->target = *t;
  s->transition_remaining = n;
  float inv = 1.0f / (float)n;
  s->step.voicing = (t->voicing - s->current.voicing) * inv;
  for (int i = 0; i < 3; i++) {
    s->step.f[i] = (t->f[i] - s->current.f[i]) * inv;
    s->step.bw[i] = (t->bw[i] - s->current.bw[i]) * inv;
    s->step.amp[i] = (t->amp[i] - s->current.amp[i]) * inv;
  }
}

KLATTSCH_INLINE void klattsch_advance(KlattschSynth *s) {
  if (s->transition_remaining <= 0) return;
  s->current.voicing += s->step.voicing;
  for (int i = 0; i < 3; i++) {
    s->current.f[i] += s->step.f[i];
    s->current.bw[i] += s->step.bw[i];
    s->current.amp[i] += s->step.amp[i];
  }
  if (--s->transition_remaining == 0) s->current = s->target;
}

KLATTSCH_INLINE void klattsch_write_formants(KlattschSynth *s, const KlattschFormants *t) {
  s->current = *t;
  s->transition_remaining = 0;
}

KLATTSCH_INLINE void klattsch_vowel(float pos, KlattschFormants *out) {
  if (pos < 0.0f) pos = 0.0f;
  if (pos > 1.0f) pos = 1.0f;
  float x = pos * 4.0f;
  int i = (int)x;
  float fr = x - (float)i;
  if (i >= 4) { i = 3; fr = 1.0f; }
  const KlattschPhone *a = &klattsch_phones[KLATTSCH_VOWEL_PATH[i]];
  const KlattschPhone *b = &klattsch_phones[KLATTSCH_VOWEL_PATH[i + 1]];
  out->voicing = a->voicing + (b->voicing - a->voicing) * fr;
  for (int k = 0; k < 3; k++) {
    out->f[k] = a->f[k] + (b->f[k] - a->f[k]) * fr;
    out->bw[k] = a->bw[k] + (b->bw[k] - a->bw[k]) * fr;
    out->amp[k] = a->amp[k] + (b->amp[k] - a->amp[k]) * fr;
  }
}

/* sin(pi * u) for u in 0..1, from the full-cycle table. */
KLATTSCH_INLINE float klattsch_half_sin(float u) {
  if (u < 0.0f) u = 0.0f;
  if (u > 1.0f) u = 1.0f;
  return klattsch_sin_at(u * 0.5f);
}

/* Derivative of the Rosenberg pulse, normalised the way klattsch does it.
 * effort 0 is lax, 1 is a short sharp closure. */
KLATTSCH_INLINE float klattsch_glottal(float phase, float effort) {
  float e = effort;
  if (e < 0.0f) e = 0.0f;
  if (e > 1.0f) e = 1.0f;
  float tp = 0.5f - e * 0.2f;
  float tn = 0.25f - e * 0.17f;
  const float pi = 3.14159265f;
  if (phase < tp) return 0.05f * (pi / tp) * klattsch_half_sin(phase / tp);
  if (phase < tp + tn) {
    float u = (phase - tp) / (2.0f * tn);
    return -0.1f * (pi / (2.0f * tn)) * klattsch_half_sin(u);
  }
  return 0.0f;
}

KLATTSCH_INLINE void klattsch_set_biquad(KlattschBiquad *b, float f, float bw, float sr) {
  float hi = sr * 0.45f;
  if (f < 40.0f) f = 40.0f;
  if (f > hi) f = hi;
  if (bw < 20.0f) bw = 20.0f;
  float df = f - b->last_f; if (df < 0.0f) df = -df;
  float db = bw - b->last_bw; if (db < 0.0f) db = -db;
  if (b->last_f > 0.0f && df < 0.75f && db < 0.75f) return;
  b->last_f = f;
  b->last_bw = bw;
  float turns = f / sr;
  float s = klattsch_sin_at(turns);
  float c = klattsch_sin_at(turns + 0.25f);
  float q = f / bw;
  float alpha = s / (2.0f * q);
  float inv = 1.0f / (1.0f + alpha);
  b->b0 = alpha * inv;
  b->b2 = -b->b0;
  b->a1 = -2.0f * c * inv;
  b->a2 = (1.0f - alpha) * inv;
}

KLATTSCH_INLINE float klattsch_biquad(KlattschBiquad *b, float x) {
  float y = b->b0 * x + b->b2 * b->x2 - b->a1 * b->y1 - b->a2 * b->y2;
  b->x2 = b->x1;
  b->x1 = x;
  b->y2 = b->y1;
  b->y1 = y;
  if (y > 8.0f || y < -8.0f) {
    b->x1 = b->x2 = b->y1 = b->y2 = 0.0f;
    b->last_f = -1.0f;
    return 0.0f;
  }
  return y;
}

KLATTSCH_INLINE uint32_t klattsch_xorshift(uint32_t x) {
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

KLATTSCH_INLINE float klattsch_soft_clip(float x) {
  float a = x < 0.0f ? -x : x;
  if (a <= 0.85f) return x;
  float y = 0.85f + 0.15f * (a - 0.85f) / (a + 0.15f);
  return x < 0.0f ? -y : y;
}

/* One sample. scale multiplies formant frequency and bandwidth (vocal-tract
 * size). voice_mul is 1 for a voiced colour and 0 for a whisper. */
KLATTSCH_INLINE float klattsch_render(KlattschSynth *s, float scale, float voice_mul) {
  if (scale < 0.5f) scale = 0.5f;
  if (scale > 2.0f) scale = 2.0f;
  float v = s->current.voicing * voice_mul;
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;

  s->noise_state = klattsch_xorshift(s->noise_state);
  float noise = (float)(int32_t)s->noise_state / 2147483648.0f;
  float pulse = klattsch_glottal(s->glottal_phase, s->effort);
  float exc = v * pulse * (1.0f - s->aspiration * 0.85f)
            + (1.0f - v) * noise * 0.35f
            + s->aspiration * noise * 0.5f;

  s->glottal_phase += s->f0 / s->sr;
  if (s->glottal_phase >= 1.0f) s->glottal_phase -= (float)(int)s->glottal_phase;

  float y = 0.0f;
  for (int i = 0; i < 3; i++) {
    klattsch_set_biquad(&s->formant[i], s->current.f[i] * scale, s->current.bw[i] * scale, s->sr);
    y += klattsch_biquad(&s->formant[i], exc) * s->current.amp[i];
  }
  y *= s->gain;
  float tilted = y - s->tilt * s->tilt_previous;
  s->tilt_previous = y;
  y = klattsch_soft_clip(tilted);
  if (!(y <= 1.2f && y >= -1.2f)) {
    for (int i = 0; i < 3; i++) {
      s->formant[i].x1 = s->formant[i].x2 = 0.0f;
      s->formant[i].y1 = s->formant[i].y2 = 0.0f;
      s->formant[i].last_f = -1.0f;
    }
    s->tilt_previous = 0.0f;
    return 0.0f;
  }
  return y;
}

KLATTSCH_INLINE void klattsch_phone_formants(const KlattschPhone *p, int glide, KlattschFormants *t) {
  t->voicing = p->voicing;
  for (int i = 0; i < 3; i++) {
    t->f[i] = glide ? p->gf[i] : p->f[i];
    t->bw[i] = p->bw[i];
    t->amp[i] = p->amp[i];
  }
}

KLATTSCH_INLINE int klattsch_ms(float sr, float ms) {
  int n = (int)(ms * 0.001f * sr);
  return n < 1 ? 1 : n;
}

KLATTSCH_INLINE void klattsch_emit(KlattschSynth *s, const KlattschFormants *t, int trans, int dur, int *left) {
  if (trans < 1) trans = 1;
  if (dur < 1) dur = 1;
  if (trans > dur) trans = dur;
  klattsch_set_formants(s, t, trans);
  *left = dur;
}

/* Sequencer timings follow klattsch's compileString: a stop is a closure plus
 * a burst capped at 25ms, a glide spends the middle half of the slot moving to
 * its endpoint, everything else crossfades in min(35ms, 40% of the slot). */
KLATTSCH_INLINE void klattsch_phrase_start(KlattschPhrase *p, int which, int reset_phase, KlattschSynth *s) {
  if (which < 0) which = 0;
  if (which >= KLATTSCH_PHRASE_COUNT) which = KLATTSCH_PHRASE_COUNT - 1;
  p->phrase = which;
  p->index = 0;
  p->part = 0;
  p->left = 0;
  p->run = 1;
  if (reset_phase && s) s->glottal_phase = 0.0f;
}

KLATTSCH_INLINE void klattsch_phrase_service(KlattschPhrase *p, KlattschSynth *s, int slot, int loop) {
  if (p->run == 0) return;
  if (slot < 1) slot = 1;
  if (p->left > 0) {
    p->left--;
    klattsch_advance(s);
    return;
  }
  if (p->run == 2) { p->run = 0; return; }

  int len = KLATTSCH_PHRASE_LEN[p->phrase];
  if (p->index >= len) {
    if (loop) {
      p->index = 0;
      p->part = 0;
    } else {
      KlattschFormants t = s->current;
      t.voicing = 0.0f;
      t.amp[0] = t.amp[1] = t.amp[2] = 0.0f;
      int dur = klattsch_ms(s->sr, 80.0f);
      klattsch_emit(s, &t, klattsch_ms(s->sr, 50.0f), dur, &p->left);
      p->run = 2;
      p->left--;
      klattsch_advance(s);
      return;
    }
  }

  const KlattschPhone *ph = &klattsch_phones[KLATTSCH_PHRASE_ID[p->phrase][p->index]];
  KlattschFormants t;
  if (ph->flags & PH_F_STOP) {
    int burst = klattsch_ms(s->sr, 25.0f);
    int cap = (int)((float)slot * 0.3f);
    if (cap < 1) cap = 1;
    if (burst > cap) burst = cap;
    int sil = slot - burst;
    if (sil < 1) sil = 1;
    if (p->part == 0) {
      t = s->current;
      t.voicing = 0.0f;
      t.amp[0] = t.amp[1] = t.amp[2] = 0.0f;
      int trans = klattsch_ms(s->sr, 20.0f);
      int tcap = (int)((float)sil * 0.4f);
      if (tcap < 1) tcap = 1;
      if (trans > tcap) trans = tcap;
      klattsch_emit(s, &t, trans, sil, &p->left);
      p->part = 1;
    } else {
      klattsch_phone_formants(ph, 0, &t);
      int trans = klattsch_ms(s->sr, 5.0f);
      int tcap = (int)((float)burst * 0.2f);
      if (tcap < 1) tcap = 1;
      if (trans > tcap) trans = tcap;
      klattsch_emit(s, &t, trans, burst, &p->left);
      p->part = 0;
      p->index++;
    }
  } else if (ph->flags & PH_F_GLIDE) {
    if (p->part == 0) {
      int onset = (int)((float)slot * 0.25f);
      if (onset < 1) onset = 1;
      int trans = klattsch_ms(s->sr, 20.0f);
      if (trans > onset) trans = onset;
      klattsch_phone_formants(ph, 0, &t);
      klattsch_emit(s, &t, trans, onset, &p->left);
      p->part = 1;
    } else {
      int onset = (int)((float)slot * 0.25f);
      if (onset < 1) onset = 1;
      int glide = (int)((float)slot * 0.50f);
      if (glide < 1) glide = 1;
      int dur = slot - onset;
      if (dur < 1) dur = 1;
      klattsch_phone_formants(ph, 1, &t);
      klattsch_emit(s, &t, glide, dur, &p->left);
      p->part = 0;
      p->index++;
    }
  } else {
    int trans = klattsch_ms(s->sr, 35.0f);
    int tcap = (int)((float)slot * 0.4f);
    if (tcap < 1) tcap = 1;
    if (trans > tcap) trans = tcap;
    klattsch_phone_formants(ph, 0, &t);
    klattsch_emit(s, &t, trans, slot, &p->left);
    p->index++;
  }
  p->left--;
  klattsch_advance(s);
}
