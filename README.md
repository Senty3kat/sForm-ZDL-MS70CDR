# sForm — shapeable LFO and one-oscillator guitar synth for Zoom MS-70CDR

[Русская версия](README.ru.md)

![sForm on the MS-70CDR screen](docs/screen.png)

**Release 1.02.** Custom `.ZDL` effect for the **Zoom MS-70CDR** (firmware 2.10).
One oscillator with a shapeable waveform that does three jobs:

- **a modulation source for [XDuck](https://github.com/Senty3kat/XDuck-ZDL-MS70CDR)** —
  sForm → XDuck makes XDuck gate, duck, tremolo or auto-pan your instrument exactly
  along sForm's wave, from 0.001 Hz to audio rate;
- **a drone** at any note C0…A7;
- **a mono guitar synth** — the oscillator follows the pitch you play (pitch tracking on
  IN L), shifted by up to four octaves down or two up.

> Made for the **MS-70CDR**: it writes its output to the R channel, and XDuck needs the CDR's
> two separate inputs.

## Install

1. Download [`release/sForm.ZDL`](release/sForm.ZDL) and [`release/sForm.png`](release/sForm.png)
   (the PNG is the Effect Manager thumbnail; keep it next to the ZDL).
2. Close Ableton Live and any browser tab that uses Web MIDI (they grab the pedal's MIDI port).
3. Add sForm with Zoom Effect Manager. It is in the **Dynamics** category, effect ID **502**.
   Effect Manager does not replace an effect with the same ID: delete an older sForm first
   (test builds 0.11 and older used ID 488 in Delay — delete that one by hand too).

## Screen

One value box at the bottom left showing the first knob of the current page; on page 1 that is
**Wave**, drawn as its shape: sine, triangle, saw up, saw down, square, S&H, drift.

## Controls (3 pages)

| Page | Knob | What it does |
|---|---|---|
| 1 | **Wave** | `Sine`, `Tri`, `SawUp`, `SawDn`, `Squar` (duty set by Skew), `S&H` (random steps), `Drift` (smooth random). |
| 1 | **Rate** | 0.1…10.0 Hz in 0.1 Hz steps (the pedal shows Hz). Used when Note = `Off`. |
| 1 | **Mult** | Rate multiplier x.01 x.02 x.05 x0.1 x0.2 x0.5 x1 x2 x5 x10 x20 x50 x100 — Rate × Mult = 0.001 Hz…1 kHz. |
| 2 | **Skew** | Symmetry: where the peak of Sine/Tri/Saw sits in the cycle; for Square the pulse width (50 = square). |
| 2 | **Curve** | Bends the shape: below 50 rounder, 50 linear, above 50 snappier. |
| 2 | **Smth** | Smoothing 0…200 ms: rounds corners of Square / S&H; on a synth note it acts like a low-pass. |
| 3 | **Out** | `XDuck` — the wave goes to XDuck on the R channel (a control signal, not for listening), L untouched. `Drone` — the oscillator as sound on the right output, L untouched. `DrnSt` — the oscillator on both outputs. |
| 3 | **Depth** | Out = XDuck: depth of the wave. Drone / DrnSt: the mix — 0 = guitar only, 50 = guitar and synth both full, 100 = synth only. |
| 3 | **Note** | `Off` — speed from Rate × Mult. `C0`…`A7` — the oscillator runs at that note. `in-48`, `in-36`, `in-24`, `in-12`, `in`, `in+12`, `in+24` — the pitch of the guitar on IN L, shifted by octaves. |

### Pitch tracking (Note = `in…`)

- The synth plays at a fixed, quiet level (about a clean DI guitar at Depth 50).
- Single notes, not chords. The synth sounds while the guitar sounds and stops when it stops
  (opens in ~2 ms, closes ~75 ms after silence); between notes it keeps the last pitch.
- Pitch follows upward moves freely (hammer-ons, bends); it goes down only on a new, strong
  note — letting a fret go (the string ringing open) does not drag it down, a right-hand
  mute just stops it.
- With Out = `XDuck` the wave runs at the pitch you play: through XDuck the guitar is
  amplitude-modulated by its own note (`in-48`/`in-36` give rhythmic pulses tied to pitch).

## Working with XDuck

Chain: **sForm → XDuck** (sForm directly before XDuck), sForm **Out = `XDuck`**,
XDuck **Route = `sForm`** (or **`sFPan`** for auto-pan). XDuck reads the wave every sample —
no detector, no envelope — so it follows the shape exactly at any rate:

| XDuck Type | Volume of the instrument |
|---|---|
| `Duck` | 1 − wave |
| `Tex` | follows the wave |
| `Gate` | opens where the wave crosses Thrsh (30 = mid-wave), Ratio = steepness |

XDuck's Range sets the depth. On the R channel sForm sends two values per sample pair —
the wave and the wave half a cycle later — which is what lets `sFPan` pan.

## Building from source

Built with the toolchain of [themanro/ZoomMultistompZDL](https://github.com/themanro/ZoomMultistompZDL)
and the TI C6000 compiler 8.5.0.LTS.

1. Clone the themanro fork and copy `src/sform/` to `src/custom/sform/`.
2. Copy the files from `toolchain-patches/` over the same paths in the fork (full files based on
   the fork as of 2026-10-18 — the same set as in the XDuck repo: single value box and custom
   knob pictures in `linker.py`, value words in `selector_metadata.py`, the screen in
   `pack_covers.py`).
3. In `src/custom/sform/build.py` set `TI_ROOT` and `out_dir`, then run
   `python src/custom/sform/build.py`.

`sform.c` follows the fork's safe-DSP rules: no switch, no math library, no divides
(bit-trick log2/exp2, Newton reciprocals), no static arrays, state in the `ctx[3]` arena.
The phase is a 32-bit fixed-point accumulator (exact down to 0.001 Hz). The pitch tracker
is a DC-blocked, adaptive two-pole low-pass at ~1.3 × the current pitch, a Schmitt trigger
and period counting with octave-error guards. The Rate and Note value labels are computed
in C (`ZDL_GetLabel_1/_8`) — as lookup chains they cost ~9 KB.

The screen art (`sform_art.json`) is a 1-bit conversion of a reference picture drawn for this
effect; the wave icons are in `knob_icons.py`.

## History

See [CHANGELOG.md](CHANGELOG.md) (in Russian).

## License

MIT — see [LICENSE](LICENSE).
