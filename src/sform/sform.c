/*
 * sform.c -- sForm: a shapeable LFO / one-oscillator mono synth, MS-70CDR.
 *
 * Made as the key source for XDuck: put sForm right before XDuck, Out = XDuck,
 * XDuck Route = sForm (or sFPan). XDuck then takes the LFO value sample by
 * sample (no detector, no envelope), so it follows the wave at any rate.
 * With Out = Drone / DrnSt the same oscillator is heard as a tone.
 * 9 knobs (UI value = params[slot] * 100, see manifest_pedal.json):
 *   Wave   (params[5])  - Sine Tri SawUp SawDn Square S&H Drift
 *   Rate   (params[6])  - 0.1 .. 10.0 Hz in 0.1 Hz steps (pedal shows Hz)
 *   Mult   (params[7])  - x.01 .02 .05 .1 .2 .5 1 2 5 10 20 50 100
 *   Skew   (params[8])  - symmetry; duty cycle of Square (50 = square)
 *   Curve  (params[9])  - log .. linear .. exp bend of the 0..1 shape
 *   Smooth (params[10]) - output one-pole, 0 .. ~200 ms
 *   Out    (params[11]) - XDuck (R carries two LFO values for XDuck: even
 *                         samples = the LFO, odd samples = the LFO 180
 *                         degrees later, sign-flipped; L untouched),
 *                         Drone (R = the oscillator as audio, L untouched),
 *                         DrnSt (both outputs = the oscillator)
 *   Depth  (params[12]) - 0..1: LFO depth (XDuck); for Drone / DrnSt the
 *                         mix: 0 = guitar, 50 = guitar + synth both full,
 *                         100 = synth only
 *   Note   (params[13]) - Off (Rate x Mult), C0..A7 (that pitch), or
 *                         in-48 in-36 in-24 in-12 in in+12 in+24: the pitch
 *                         tracked from IN L, shifted by octaves; the synth
 *                         is gated by the input (silent when it is)
 *
 * The LFO is unipolar 0..1 (a gain-like control), scaled by Depth.
 *
 * Safe-DSP: no switch (float compares only), no math lib (bit-trick
 * log2/exp2, polynomial sine, Newton reciprocals instead of divides), no
 * static arrays, tiny state in the ctx[3] arena, ctx[11]/ctx[12] shuttle kept.
 */

#include <stdint.h>

#include "../../airwindows/common/zoom_params.h"
#include "sform_params.h"

#ifndef SFORM_AUDIO_FUNC
#define SFORM_AUDIO_FUNC Fx_DYN_sForm
#endif

#define SF_DO_PRAGMA(x) _Pragma(#x)
#define SF_EXPAND_PRAGMA(x) SF_DO_PRAGMA(x)
#define SF_CODE_SECTION(f) SF_EXPAND_PRAGMA(CODE_SECTION(f, ".audio"))

SF_CODE_SECTION(SFORM_AUDIO_FUNC)

#define ZDL_PTR(type, word) ((type)(uintptr_t)(word))

#define SF_MAGIC   0x73466F31u   /* 'sFo1' */
#define SF_VERSION 7u

#define SF_INV_SR      0.0000226757f     /* 1 / 44100 */
#define SF_INC_MAX     0.45351474f       /* 20 kHz / 44100 */
#define SF_SEMI        0.08333333f       /* 1/12 octave */
#define SF_TWO31       2147483648.0f
#define SF_INV_TWO31   4.656612873e-10f
#define SF_LOG2E       1.44269504f
#define SF_SMTH_X0     0.04535147f       /* 1 / (0.5 ms * 44100) */
#define SF_SMTH_OCT    0.0862f           /* 0.5 ms * 2^8.62 = ~200 ms */
#define SF_TRK_LPK     11.785f           /* 2*pi*1.3*log2(e): tracker LP at 1.3 x f0 */
#define SF_TRK_GATE    0.003f            /* -50 dB: below this the pitch is held */
#define SF_ENV_DECAY   0.9995f           /* peak followers, ~45 ms */
#define SF_TWO_PI      6.28318530718f
#define SF_PI          3.14159265359f

typedef struct SFormState {
    uint32_t magic;
    uint32_t version;
    uint32_t initialized;
    uint32_t rng;
    uint32_t phase;  /* 0..2^32 = one cycle (exact at 0.01 Hz too) */
    float held;      /* S&H value / Drift target */
    float prev;      /* Drift start value */
    float smooth;    /* smoothed output */
    float smooth2;   /* smoothed 180-degree copy (Link, for XDuck sFPan) */
    float trkHx;     /* pitch tracker: input DC-block state */
    float trkHy;
    float trkL1;     /* tracker low-pass, two one-poles */
    float trkL2;
    float trkEnvF;   /* level of the filtered signal (hysteresis) */
    float trkEnvIn;  /* level of IN L (gate, synth amplitude) */
    float trkCnt;    /* samples since the last rising crossing */
    float trkPer;    /* smoothed period, samples */
    float trkCand;   /* candidate period for a note change */
    float trkHigh;   /* Schmitt trigger state */
    float trkEnvS;   /* slow level, for attack detection */
    float trkFper;   /* period that sets the tracker low-pass */
    float trkLastP;  /* previous raw period */
    float trkSumP;   /* previous sum of two raw periods */
    float trkGate;   /* "in" modes: synth gate, 0..1, follows the input */
    float trkPeak;   /* loudest level of the current note */
    float dcX;       /* drone DC blocker */
    float dcY;
} SFormState;

typedef union { float f; uint32_t u; } SfBits;

SF_DO_PRAGMA(FUNC_ALWAYS_INLINE(sf_ui))
static inline float sf_ui(float raw, float def_ui)
{
    if (!(raw >= 0.0f && raw <= 100.0f)) return def_ui;
    if (raw <= 1.0f) return raw * 100.0f;
    return raw;
}

SF_DO_PRAGMA(FUNC_ALWAYS_INLINE(sf_log2))
static inline float sf_log2(float x)
{
    SfBits v;
    v.f = x;
    float e = (float)((int32_t)((v.u >> 23) & 255u) - 127);
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    float m = v.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 1.67487759f;
}

/* 2^x for -60 <= x <= 60. */
SF_DO_PRAGMA(FUNC_ALWAYS_INLINE(sf_exp2))
static inline float sf_exp2(float x)
{
    if (x < -60.0f) x = -60.0f;
    if (x > 60.0f) x = 60.0f;
    int32_t i = (int32_t)x;
    if ((float)i > x) i -= 1;
    float f = x - (float)i;
    float p = 1.0f + f * (0.69606564f + f * (0.22449433f + f * 0.07944023f));
    SfBits v;
    v.u = ((uint32_t)(i + 127)) << 23;
    return v.f * p;
}

/* 1/x for x in [0.01, 100]: bit-trick seed, two Newton steps. */
SF_DO_PRAGMA(FUNC_ALWAYS_INLINE(sf_recip))
static inline float sf_recip(float x)
{
    float r = sf_exp2(-sf_log2(x));
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    return r;
}

/* 1/x for any normal positive x (log2/exp2 seed, two Newton steps). */
SF_DO_PRAGMA(FUNC_ALWAYS_INLINE(sf_recip_any))
static inline float sf_recip_any(float x)
{
    float r = sf_exp2(-sf_log2(x));
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    return r;
}

/* sin(x) for x in [-pi, pi]. */
SF_DO_PRAGMA(FUNC_ALWAYS_INLINE(sf_sin))
static inline float sf_sin(float x)
{
    float ax = x < 0.0f ? -x : x;
    float y = 1.2732395447f * x - 0.4052847346f * x * ax;
    float ay = y < 0.0f ? -y : y;
    return y + 0.225f * (y * ay - y);
}

/* 0.5 - 0.5*cos(2*pi*q), q in [0,1): a raised cosine, 0 at q=0, 1 at q=0.5. */
SF_DO_PRAGMA(FUNC_ALWAYS_INLINE(sf_rcos))
static inline float sf_rcos(float q)
{
    float a = SF_TWO_PI * (q + 0.25f);       /* cos(t) = sin(t + pi/2) */
    if (a > SF_PI) a -= SF_TWO_PI;
    if (a > SF_PI) a -= SF_TWO_PI;
    return 0.5f - 0.5f * sf_sin(a);
}

SF_DO_PRAGMA(FUNC_ALWAYS_INLINE(sf_rand))
static inline float sf_rand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    SfBits v;
    v.u = (*s >> 9) | 0x3F800000u;           /* [1,2) */
    return v.f - 1.0f;
}

/* Deterministic shapes (Sine Tri SawUp SawDn Square), 0..1, curve applied. */
SF_DO_PRAGMA(FUNC_ALWAYS_INLINE(sf_shape))
static inline float sf_shape(float p, float waveUi, float skew, float invS,
                             float invS1, float bend)
{
    /* Skew-warped phase: the first half of the cycle spans 0..skew. */
    float q = (p < skew) ? 0.5f * p * invS : 0.5f + 0.5f * (p - skew) * invS1;
    float u;
    if      (waveUi < 0.5f) u = sf_rcos(q);                                 /* Sine */
    else if (waveUi < 1.5f) u = (p < skew) ? p * invS : (1.0f - p) * invS1; /* Tri */
    else if (waveUi < 2.5f) u = q;                                          /* SawUp */
    else if (waveUi < 3.5f) u = 1.0f - q;                                   /* SawDn */
    else                    u = (p < skew) ? 1.0f : 0.0f;                   /* Square, duty = Skew */
    if (u > 1.0f) u = 1.0f;
    /* Curve: u^bend (bend 1 = linear). */
    if (u > 0.000001f) return sf_exp2(bend * sf_log2(u));
    return 0.0f;
}

void SFORM_AUDIO_FUNC(unsigned int *ctx)
{
    float *params = ZDL_PTR(float *, ctx[1]);
    float *fxBuf  = ZDL_PTR(float *, ctx[5]);

    unsigned int *magicSrc = ZDL_PTR(unsigned int *, ctx[12]);
    unsigned int *magicDst = ZDL_PTR(unsigned int *, *(unsigned int *)ZDL_PTR(unsigned int *, ctx[11]));
    *magicDst = *magicSrc;

    if (params[0] < 0.5f) return;

    volatile unsigned int *desc = ZDL_PTR(volatile unsigned int *, ctx[3]);
    if (!desc) return;
    uintptr_t base = (uintptr_t)desc[0];
    uintptr_t end  = (uintptr_t)desc[1];
    if (base == 0u || end <= base) return;
    if ((base & 3u) != 0u) return;
    if (end - base < (uintptr_t)sizeof(SFormState)) return;

    SFormState *st = (SFormState *)base;
    if (st->magic != SF_MAGIC || st->version != SF_VERSION || !st->initialized) {
        st->magic = SF_MAGIC;
        st->version = SF_VERSION;
        st->rng = 0x2545F491u;
        st->phase = 0u;
        st->held = 0.5f;
        st->prev = 0.5f;
        st->smooth = 0.0f;
        st->smooth2 = 0.0f;
        st->trkHx = 0.0f; st->trkHy = 0.0f;
        st->trkL1 = 0.0f; st->trkL2 = 0.0f;
        st->trkEnvF = 0.0f; st->trkEnvIn = 0.0f;
        st->trkCnt = 0.0f; st->trkPer = 200.0f;
        st->trkCand = 0.0f; st->trkHigh = 0.0f;
        st->trkEnvS = 0.0f; st->trkFper = 200.0f;
        st->trkLastP = 0.0f; st->trkSumP = 0.0f;
        st->trkGate = 0.0f;
        st->trkPeak = 0.0f;
        st->dcX = 0.0f; st->dcY = 0.0f;
        st->initialized = 1u;
    }

    float rateUi  = sf_ui(params[SFORM_RATE_SLOT],   (float)SFORM_RATE_UI_DEFAULT);
    float multUi  = sf_ui(params[SFORM_MULT_SLOT],   (float)SFORM_MULT_UI_DEFAULT);
    float waveUi  = sf_ui(params[SFORM_WAVE_SLOT],   (float)SFORM_WAVE_UI_DEFAULT);
    float skewUi  = sf_ui(params[SFORM_SKEW_SLOT],   (float)SFORM_SKEW_UI_DEFAULT);
    float curveUi = sf_ui(params[SFORM_CURVE_SLOT],  (float)SFORM_CURVE_UI_DEFAULT);
    float smthUi  = sf_ui(params[SFORM_SMOOTH_SLOT], (float)SFORM_SMOOTH_UI_DEFAULT);
    float depth   = sf_ui(params[SFORM_DEPTH_SLOT],  (float)SFORM_DEPTH_UI_DEFAULT) * 0.01f;
    /* Note has 102 positions (max 101, the largest proven on the pedal: 103
     * failed to install in 0.16). The LineSel handler sends UI*0.01, so 101
     * arrives as 1.01, which sf_ui would take as UI 1 (C0). */
    float noteRaw = params[SFORM_NOTE_SLOT];
    float noteUi;
    if (!(noteRaw >= 0.0f && noteRaw <= 200.0f)) noteUi = (float)SFORM_NOTE_UI_DEFAULT;
    else if (noteRaw < 1.5f)                     noteUi = noteRaw * 100.0f;
    else                                         noteUi = noteRaw;
    float outUi   = sf_ui(params[SFORM_OUT_SLOT],    (float)SFORM_OUT_UI_DEFAULT);

    if (rateUi > 99.0f) rateUi = 99.0f;
    if (depth > 1.0f) depth = 1.0f;
    if (noteUi > 101.0f) noteUi = 101.0f;
    /* Integer knob steps arrive as UI*0.01 floats: round to the step. */
    rateUi = (float)(int32_t)(rateUi + 0.5f);
    noteUi = (float)(int32_t)(noteUi + 0.5f);

    /* Multiplier ladder (if/else on floats, never a switch). */
    float mult;
    if      (multUi < 0.5f)  mult = 0.01f;
    else if (multUi < 1.5f)  mult = 0.02f;
    else if (multUi < 2.5f)  mult = 0.05f;
    else if (multUi < 3.5f)  mult = 0.1f;
    else if (multUi < 4.5f)  mult = 0.2f;
    else if (multUi < 5.5f)  mult = 0.5f;
    else if (multUi < 6.5f)  mult = 1.0f;
    else if (multUi < 7.5f)  mult = 2.0f;
    else if (multUi < 8.5f)  mult = 5.0f;
    else if (multUi < 9.5f)  mult = 10.0f;
    else if (multUi < 10.5f) mult = 20.0f;
    else if (multUi < 11.5f) mult = 50.0f;
    else                     mult = 100.0f;

    float hz = 0.1f * (rateUi + 1.0f) * mult;
    /* Note n (1 = C0 = MIDI 12): 440 * 2^((n + 11 - 69) / 12). */
    if (noteUi > 0.5f && noteUi < 94.5f) hz = 440.0f * sf_exp2((noteUi - 58.0f) * SF_SEMI);
    float track = (noteUi > 94.5f) ? 1.0f : 0.0f;

    /* Pitch tracker state; its period gives the rate in the "in" modes. */
    float hx = st->trkHx, hy = st->trkHy, l1 = st->trkL1, l2 = st->trkL2;
    float envF = st->trkEnvF, envIn = st->trkEnvIn, cnt = st->trkCnt;
    float per = st->trkPer, cand = st->trkCand, high = st->trkHigh;
    float envS = st->trkEnvS, fper = st->trkFper;
    float lastP = st->trkLastP, sumP = st->trkSumP;
    float rPer = sf_recip_any(per);
    float aTrk = 1.0f - sf_exp2(-SF_TRK_LPK * sf_recip_any(fper));

    float incF = hz * SF_INV_SR;
    if (track > 0.5f) incF = rPer * sf_exp2(noteUi - 99.0f);   /* in-48 .. in+24 */
    if (incF > SF_INC_MAX) incF = SF_INC_MAX;
    /* Fixed-point phase step; int32 conversions only (no RTS helpers). */
    uint32_t inc = ((uint32_t)(int32_t)(incF * SF_TWO31)) << 1;

    float skew = 0.02f + skewUi * 0.0096f;               /* 0.02 .. 0.98 */
    float invS  = sf_recip(skew);
    float invS1 = sf_recip(1.0f - skew);
    float bend = sf_exp2((curveUi - 50.0f) * 0.06f);      /* x1/8 .. x8 */
    float aSm = 1.0f;
    if (smthUi > 0.5f) {
        float x = SF_SMTH_X0 * sf_exp2(-SF_SMTH_OCT * smthUi);
        aSm = 1.0f - sf_exp2(-x * SF_LOG2E);
    }

    uint32_t phase = st->phase;
    float held = st->held, prev = st->prev;
    float sm = st->smooth, sm2 = st->smooth2;
    float dcX = st->dcX, dcY = st->dcY;
    float gate = st->trkGate, peak = st->trkPeak;
    /* Drone mix: Depth 0..50 brings the synth in over the full guitar,
     * 50..100 takes the guitar out (100 = synth only). */
    float synG = 1.4f * depth;                   /* synth tops out at 0.7 (-3 dB) */
    float dryG = 2.0f - 2.0f * depth;
    if (synG > 0.7f) synG = 0.7f;
    if (dryG > 1.0f) dryG = 1.0f;
    uint32_t rng = st->rng;
    int i;
    for (i = 0; i < 8; i++) {
        float inL = fxBuf[i];

        /* ---- pitch tracker on IN L: DC block, low-pass at ~1.3 x the
         * current pitch (keeps the fundamental, drops the harmonics),
         * Schmitt trigger, period = samples between rising crossings. */
        hy = 0.9929f * (hy + inL - hx);                  /* ~50 Hz high-pass */
        hx = inL;
        l1 += aTrk * (hy - l1);
        l2 += aTrk * (l1 - l2);
        float ay = l2 < 0.0f ? -l2 : l2;
        float ax = inL < 0.0f ? -inL : inL;
        envF  = (ay > envF)  ? ay : envF  * SF_ENV_DECAY;
        envIn = (ax > envIn) ? ax : envIn * SF_ENV_DECAY;
        /* A pick attack opens the low-pass down to ~115 Hz, so a lower new
         * note is caught by its fundamental, not by a harmonic. */
        envS += 0.00008f * (envIn - envS);
        if (envIn > 1.5f * envS + SF_TRK_GATE) { fper = 500.0f; cand = 0.0f; envS = envIn; peak = envIn; }
        if (envIn > peak) peak = envIn;
        cnt += 1.0f;
        if (cnt > 4000.0f) cnt = 4000.0f;
        float hyst = 0.25f * envF;
        if (high < 0.5f && l2 > hyst) {
            high = 1.0f;
            float P = cnt;
            cnt = 0.0f;
            /* Pitch is only re-measured while the note is strong: in its tail
             * (below ~-18 dB of its peak, or near the gate) the last pitch is
             * held, so a dying note does not drag the synth down. */
            if (envIn > 0.006f && envIn > 0.12f * peak && P >= 18.0f && P <= 1400.0f) {
                float d = P - per;
                float ad = d < 0.0f ? -d : d;
                float dd = P - cand;
                float add = dd < 0.0f ? -dd : dd;
                float ds = P + lastP - sumP;
                float ads = ds < 0.0f ? -ds : ds;
                /* Upward moves are followed freely (hammer-ons, bends, up to a
                 * fourth per step, an octave via the candidate). Downward moves
                 * need a strong note: a string let go from the fret rings on
                 * open, quieter, and must not pull the synth down (a right-hand
                 * mute just stops it). In a weak tail only ~3 % down (vibrato). */
                float strong = (envIn > 0.35f * peak) ? 1.0f : 0.0f;
                float up = (P < per) ? 1.0f : 0.0f;
                float sp = P + lastP;
                if (ad < 0.03f * per || ((strong > 0.5f || up > 0.5f) && ad < 0.25f * per)) {
                    per += 0.3f * d; cand = 0.0f; fper = per;                   /* same note */
                }
                else if (strong < 0.5f && up < 0.5f) { }                        /* weak, down: hold */
                else if (add < 0.15f * cand && (strong > 0.5f || P < per)) {
                    per = P; cand = 0.0f; fper = per;                           /* new note, seen twice */
                }
                else if (ads < 0.1f * sumP && sp <= 1400.0f && (strong > 0.5f || sp < per)) {
                    per = sp; cand = 0.0f; fper = per;                          /* two crossings per period */
                }
                else cand = P;
                sumP = P + lastP;
                lastP = P;
            }
        } else if (high > 0.5f && l2 < -hyst) {
            high = 0.0f;
        }

        /* ---- oscillator / LFO */
        uint32_t last = phase;
        phase += inc;
        if (phase < last) {                      /* wrapped: new cycle */
            prev = held;
            held = sf_rand(&rng);
        }
        float p = (float)(int32_t)(phase >> 1) * SF_INV_TWO31;
        if (p >= 1.0f) p = 0.99999994f;
        float p2 = p + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        float u, u2;
        if (waveUi < 4.5f) {
            u  = sf_shape(p,  waveUi, skew, invS, invS1, bend);
            u2 = sf_shape(p2, waveUi, skew, invS, invS1, bend);
        } else {
            if (waveUi < 5.5f) u = held;                                        /* S&H */
            else {                                                              /* Drift */
                float s = p * p * (3.0f - 2.0f * p);
                u = prev + s * (held - prev);
            }
            if (u > 1.0f) u = 1.0f;
            if (u > 0.000001f) u = sf_exp2(bend * sf_log2(u));
            else u = 0.0f;
            u2 = 1.0f - u;                       /* random waves: mirror image */
        }

        sm += aSm * (u - sm);
        sm2 += aSm * (u2 - sm2);

        /* Drone: bipolar, DC-blocked (Square with Skew != 50 has DC), steady
         * level; in the "in" modes IN L sets only its pitch (held between
         * notes). */
        float bip = 2.0f * sm - 1.0f;
        dcY = 0.9986f * (dcY + bip - dcX);
        dcX = bip;
        /* "in" modes: the synth sounds only while the guitar does (gate with
         * hysteresis, ~2 ms open / ~75 ms close); fixed-pitch modes drone on. */
        float gOpen = (envIn > 0.006f) ? 1.0f : ((envIn < 0.003f) ? 0.0f : ((gate > 0.5f) ? 1.0f : 0.0f));
        gate += ((gOpen > gate) ? 0.01f : 0.0003f) * (gOpen - gate);
        float sg = 1.0f + track * (gate - 1.0f);
        float drone = dryG * inL + synG * sg * dcY;

        if (outUi < 0.5f) {                                                     /* XDuck */
            fxBuf[i + 8] = (i & 1) ? -depth * sm2 : depth * sm;
        } else if (outUi < 1.5f) {                                              /* Drone */
            fxBuf[i + 8] = drone;
        } else {                                                                /* DrnSt */
            fxBuf[i]     = drone;
            fxBuf[i + 8] = drone;
        }
    }

    st->phase = phase;
    st->held = held;
    st->prev = prev;
    st->smooth = sm;
    st->smooth2 = sm2;
    st->rng = rng;
    st->dcX = dcX; st->dcY = dcY;
    st->trkGate = gate;
    st->trkPeak = peak;
    st->trkHx = hx; st->trkHy = hy; st->trkL1 = l1; st->trkL2 = l2;
    st->trkEnvF = envF; st->trkEnvIn = envIn; st->trkCnt = cnt;
    st->trkPer = per; st->trkCand = cand; st->trkHigh = high;
    st->trkEnvS = envS; st->trkFper = fper;
    st->trkLastP = lastP; st->trkSumP = sumP;
}

/* Value labels computed instead of if-chains (selector_metadata keeps the same
 * strings in SFORM_LABELS). Stock GetString ABI: value, destination -> length.
 * No divides: digits and octaves by repeated subtraction. */
int ZDL_GetLabel_1(unsigned int value, char *out)          /* Rate: 0.1 .. 10.0 */
{
    unsigned int v = value + 1u, tens = 0u, n = 0u;        /* tenths of Hz */
    if (v > 100u) v = 100u;
    if (v == 100u) { out[0] = '1'; out[1] = '0'; out[2] = '.'; out[3] = '0'; out[4] = 0; return 4; }
    while (v >= 10u) { v -= 10u; tens++; }
    out[n++] = (char)('0' + tens);
    out[n++] = '.';
    out[n++] = (char)('0' + v);
    out[n] = 0;
    return (int)n;
}

int ZDL_GetLabel_8(unsigned int value, char *out)          /* Note: Off, C0..A7, in-48..in+24 */
{
    unsigned int k, oct = 0u, n = 0u;
    char c;
    int sharp;
    if (value >= 95u && value <= 101u) {
        out[0] = 'i'; out[1] = 'n';
        if (value == 99u) { out[2] = 0; return 2; }
        out[2] = (value < 99u) ? '-' : '+';
        if      (value == 95u)                   { out[3] = '4'; out[4] = '8'; }
        else if (value == 96u)                   { out[3] = '3'; out[4] = '6'; }
        else if (value == 97u || value == 101u)  { out[3] = '2'; out[4] = '4'; }
        else                                     { out[3] = '1'; out[4] = '2'; }
        out[5] = 0;
        return 5;
    }
    if (value == 0u || value > 94u) {
        out[0] = 'O'; out[1] = 'f'; out[2] = 'f'; out[3] = 0;
        return 3;
    }
    k = value - 1u;
    while (k >= 12u) { k -= 12u; oct++; }
    sharp = (k == 1u || k == 3u || k == 6u || k == 8u || k == 10u);
    if (sharp) k -= 1u;
    if      (k == 0u)  c = 'C';
    else if (k == 2u)  c = 'D';
    else if (k == 4u)  c = 'E';
    else if (k == 5u)  c = 'F';
    else if (k == 7u)  c = 'G';
    else if (k == 9u)  c = 'A';
    else               c = 'B';
    out[n++] = c;
    if (sharp) out[n++] = '#';
    out[n++] = (char)('0' + oct);
    out[n] = 0;
    return (int)n;
}
