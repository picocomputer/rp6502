# YM3812 per-sample specification

The behaviour of the YM3812 (OPL2) reduced to one step per output sample, as exact
integer rules. Source of truth: nukeykt's YM3812-LLE `fmopl2.c` (gate-level die model,
revision a61c464b). This text and its pseudo-code are written from an understanding of
that model; no LLE or Nuked-OPL2-Lite code is reproduced. ROM contents are generated from
formulas (chip data).

## Status: verified

`core/aud/opl.c` implements these rules. `tests/host/emu/test_opl.c` checks it
against LLE sample by sample, LLE driven only through its pins by
`tests/host/emu/opl_lle.c` with writes at the phases of section 7.6, on both the
engine's output and that of `tests/host/emu/opl_model.c`, this spec as plain code.
The cycle-level rules of sections 7.2-7.5 were measured on LLE with writes at random
cycle phases. Not covered: timers and CSM (section 8.4).

---------------------------------------------------------------------------------------

## 0. Terms

- **Master clock** φM (3.579545 MHz). **Cycle** = 4 φM. **Chip sample** = 18 cycles =
  72 φM; fs = φM/72 = 49 715.9 Hz.
- **Cycle index** t = 0..17 within a chip sample.
- **Slot** s = 0..17, numbered in processing order. Register offset of slot s:
  `off(s) = s + 2*(s/6)` (0x00-05, 0x08-0D, 0x10-15). Channel of slot s:
  `ch(s) = 3*(s/6) + s%3`. Slot s is a **modulator** if `s%6 < 3`, else a **carrier**;
  the carrier of a channel is its modulator + 3.

  | ch | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
  |---|---|---|---|---|---|---|---|---|---|
  | modulator slot | 0 | 1 | 2 | 6 | 7 | 8 | 12 | 13 | 14 |
  | carrier slot | 3 | 4 | 5 | 9 | 10 | 11 | 15 | 16 | 17 |

- **Chip sample n**: the 18 cycles in which slot s has its envelope (EG) and phase (PG)
  evaluated in cycle s.
- **Output n**: the accumulator value latched at the start of cycle 17 of chip sample n
  (section 6).
- `>>` on a negative value is an arithmetic shift (floor division). All widths are stated.

---------------------------------------------------------------------------------------

## 1. Frame, processing order, global counters

### 1.1 What the die does in one chip sample

The die is a pipeline: in cycle t it evaluates the EG and PG of slot t, the log-sin
lookup of slot t, the exp lookup of slot t−1, and adds the output of slot t−2 to the
accumulator. A per-sample engine evaluates each slot completely in slot order; what it
must preserve is the cycle at which each input is read. Those read cycles are:

| Input | Read at cycle (relative to slot s's chip sample) |
|---|---|
| Operator registers of slot s (20-95, E0-F5 fields) | s |
| Channel F-number, block, KON for slot s | s |
| Channel CON for a channel | its modulator's cycle (decides both "add modulator" and "modulate carrier") |
| Channel FB for a channel | its modulator's cycle − 3 (ch 0-2: cycles 15-17 of the previous chip sample) |
| DAM, DVB, NTS, test bits 0 and 2 | s |
| Drum-key enable (rhythm bit) for slots 12-17 | 12 |
| Drum KON bits (BD register) for slot s = 12..17 | s |
| Rhythm bit for HH/SD/TC phase substitution | 13 / 16 / 17 |
| Rhythm bit for "no feedback" on slots 13 / 14 | 12 / 13 |
| Rhythm bit for "no modulation" into slots 16 / 17, and for not storing slot 13 / 14 into feedback history | 15 / 16 |
| Rhythm bit for leaving out slot 12 / adding slots 13, 14 regardless of CON | 14 / 15, 16 |
| Rhythm bit for the ×2 weight of slot u (u = 12..17) | u + 2 (slots 16, 17: cycles 0, 1 of the **next** chip sample) |
| Noise LFSR bit for HH / SD | 13 / 16 |
| Tremolo value (`tout`) | latched at cycle 0, same for all 18 slots |
| Vibrato counter, EG timer latch (`egshift`, `eglow`), EG parity (`sub`) | constant through the chip sample |
| Accumulator latch (output n) | start of cycle 17: holds slots 15-17 of chip sample n−1 and slots 0-14 of chip sample n |

When registers only change between samples (the engine's case, section 7.6), every
"rhythm at cycle x" above is simply the rhythm bit of that chip sample, except the ×2
weights of slots 16 and 17, which use the rhythm bit of the next chip sample.

### 1.2 Where the order matters

1. **Modulator before carrier, same sample.** The carrier of a channel is modulated by its
   modulator's output computed in the same chip sample (3 slots earlier).
2. **Feedback history.** A modulator's feedback input uses its own outputs from the
   previous two chip samples. The new output is pushed into the history after use.
3. **Slots 15-17 go into the next output.** The accumulator closes after slot 14, so the
   carriers of channels 6, 7, 8 (melodic) or BD-carrier, SD, TC (rhythm) computed in chip
   sample n are heard in output n+1. Output n = slots 15-17 of chip sample n−1 + slots
   0-14 of chip sample n.
4. **Rhythm phase bits.** HH (slot 13) and TC (slot 17) both use
   `rm = (h2^h7) | (h3^c5) | (c3^c5)`. The h-bits come from slot 13's phase of **this**
   chip sample (captured just before slot 13). The c-bits come from slot 17's phase,
   captured during slot 17 only while rhythm mode is on. So HH uses c-bits from the
   **previous** rhythm-mode chip sample; SD and TC use h-bits of this chip sample; TC uses
   c-bits just captured from its own phase.
5. **Noise.** The LFSR steps once per cycle, 18 times per chip sample, whether or not
   rhythm mode is on. HH reads it at cycle 13 and SD at cycle 16 (3 steps apart).
6. **Channel registers of channels 0-2 reach the carrier one chip sample before the
   modulator** (section 7.4). FB of channels 3-5 reaches the modulator one chip sample
   after the rest of the C0 register.

### 1.3 Global counters, per chip sample

All updates happen at the end of chip sample n (cycle 17) and are seen by all of chip
sample n+1, except the tremolo adder, which runs in cycles 0-8 of chip sample n+1.

**LFO counter** `lfo` (10 bit), **vibrato position** `vib` (3 bit):
```
lfo(n+1) = test7 ? 0 : (lfo(n) + 1) & 1023
vib(n+1) = test7 ? 0 : (vib(n) + (lfo(n) == 1023 || test3)) & 7
```
Vibrato steps every 1024 samples (period 8192 samples, 6.07 Hz). Test bit 7 clears
`lfo` and `vib` from cycle 0 of each chip sample it is set in, after slot 0 has read
`vib`: in the first chip sample with bit 7 set, slot 0 uses the old vibrato position and
slots 1-17 use 0.

**Tremolo** — 9-bit counter `tv`, direction `tdir` (0 up, 1 down), last edge flag `tof`,
step flag `tstep`:
```
start of chip sample n:
    tout = tv & 127                     // used by every slot of chip sample n
    of   = (tout == 0) || ((tout & 105) == 105)
    if (of && !tof) tdir ^= 1           // toggles on the first sample at 0 or 105
    tof  = of
    if (tstep || test3) tv = (tv + (tdir ? -1 : +1)) & 511     // seen as tout in n+1
    if (test7) { tv = 0; tdir = 0; }
end of chip sample n:
    tstep = (lfo(n) & 63) == 63         // the step applied in chip sample n+1
```
The counter runs 0,1,...,105,104,...,1,0,1,... each value lasting 64 samples: period
210·64 = 13 440 samples (3.70 Hz). The value seen by the EG lags the step decision by two
samples (decided at the end of n, added in n+1, seen in n+2).

**EG timer** — 18-bit `T`, end-around `carry`, parity `sub`, latched `egshift` (0..13)
and `eglow` (0..3):
```
end of chip sample n:
    cin   = carry | sub
    T     = T + cin;  carry = T >> 18;  T &= 0x3FFFF
    was   = sub;  sub ^= 1
    if (was) {                           // latch for the next two samples
        eglow  = T & 3
        egshift = (T & 0x1FFF) ? ctz(T) + 1 : 0      // only bits 0..12 count
    }
```
So T increments once every 2 samples (on samples with `sub == 1`). The EG of chip sample
n uses `sub(n)`, `egshift`, `eglow` as they stand at its start. Wrap quirk: 0x3FFFF → 0,
then the carry adds 1 on the following (sub = 0) sample: the latched sequence is
…, 0x3FFFF, 0, 2, 3, … (1 is skipped once every 2^18 ticks, ~10.5 s).

**Noise LFSR** — 23 bits `N`, written here in LLE's orientation:
```
step:    b = ((N >> 22) ^ (N >> 8)) & 1;  if (N == 0) b = 1;  if (test7) b = 1
         N = ((N << 1) | b) & 0x7FFFFF
chip sample n:   hh_noise = (N >> 9) & 1;   sd_noise = (N >> 6) & 1      // N at the start of n
                 then N = 18 steps later
```
(bit 22 after 13 steps is bit 9 now; after 16 steps, bit 6). Taps at bits 22 and 8,
period 2^23 − 1; Nuked-OPL2-Lite holds the same sequence bit-mirrored (its
x^23 + x^14 + 1 form).

---------------------------------------------------------------------------------------

## 2. Phase generator

Per slot: 19-bit phase accumulator `P`. Inputs: F-number `fnum` (10 bit), `block`
(3 bit) of the slot's view of its channel (section 7.4), `MULT` (4 bit), `VIB` bit.

```
static const uint8 mt[16] = {1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30};   // 2×MULT

f = fnum
if (VIB) {
    r = 0
    if ((vib & 3) == 2)      r = DVB ? (fnum >> 7) & 7 : (fnum >> 8) & 3     // peak
    else if (vib & 1)        r = DVB ? (fnum >> 8) & 3 : (fnum >> 9) & 1     // half
    if (vib & 4) f = (f - r) & 1023          // negative half: 10-bit wrap
    else         f = f + r                    // positive half: up to 1030, not masked
}
inc   = (((f << block) >> 1) * mt[MULT]) >> 1          // two truncations, ≤ 988 800
phase = (P >> 9) & 1023                                  // the 10-bit phase used THIS sample
P     = ((keyon_edge || test2) ? 0 : P) + inc,  & 0x7FFFF
```
- `vib` pattern over positions 0..7: 0, +½, +1, +½, 0, −½, −1, −½ of `fnum>>7` (DVB=1) or
  `fnum>>8` (DVB=0); the half steps are taken from the next lower F-number bits, not by
  halving a signed value, so the pattern is symmetric.
- Block 0 drops the F-number LSB; MULT 0 (×½) truncates once more.
- `keyon_edge` is the EG's key-on edge for this slot this sample (section 3.3). On that
  sample the waveform still uses the old phase; the next sample uses `inc`, then `2·inc`, …
- The PG runs every sample for every slot, keyed or not.
- KSL and KSR use the un-vibrated `fnum` and `block`.

---------------------------------------------------------------------------------------

## 3. Envelope generator

Per slot: 9-bit attenuation `level` (0 loudest, 0x1FF silent; 1 LSB = 0.1875 dB) and
state ∈ {ATTACK, DECAY, SUSTAIN, RELEASE}.

### 3.1 Key

```
key = KON(slot's view)                    // B0-B8 bit 5
    | drum_key(s)                         // slots 12..17, section 5.1
    | csm_pulse                           // section 8.4; absent if CSM is not implemented
```

### 3.2 Rate

```
dokon = (state == RELEASE) && key         // the key-on edge, as the chip sees it
sel   = dokon ? ATTACK : state
R     = sel == ATTACK  ? AR :
        sel == DECAY   ? DR :
        sel == RELEASE ? RR :
        /* SUSTAIN */    (EGT ? 0 : RR)
ns    = NTS ? (fnum >> 8) & 1 : (fnum >> 9) & 1          // reg 08 bit 6
ks    = KSR ? (block << 1) | ns : block >> 1               // 0..15
rhi   = min(R + (ks >> 2), 15)                              // rate6 = 4R + ks, top 4 bits
rlo   = ks & 3
```
`R == 0` means no step at all (the `rhi` computed from `ks` alone never steps).

### 3.3 Step size this sample

`k` = number of steps of size 1, 2 or 4 (encoded as `step` = 0 none, 1, 2, 3):
```
static const uint8 hi[4][4] = {{0,0,0,0},{1,0,0,0},{1,0,1,0},{1,1,1,0}};  // [rlo][eglow]
step = 0
if (rhi < 12) {
    if (R != 0 && sub) {
        x = rhi + egshift
        if (x == 12 || (x == 13 && (rlo & 2)) || (x == 14 && (rlo & 1))) step = 1
    }
} else {
    h = hi[rlo][eglow]
    if      (rhi == 12) step = (h || sub) ? 1 : 0
    else if (rhi == 13) step = h ? 2 : 1
    else if (rhi == 14) step = h ? 3 : 2
    else                step = 3                 // rhi == 15
}
```
- Rates below 48 step only on `sub == 1` samples. Average rate (4 + rlo)/4 · 2^(rhi−12)
  steps per 2-sample tick, at fixed positions set by the timer's bit pattern.
- Rate 48-51 (rhi 12): one step on every `sub` sample plus `hi` samples; 52-55: 1 or 2;
  56-59: 2 or 3 (size 4 when 3); 60-63: size 4 every sample (decay/sustain/release).

### 3.4 State machine and level update

```
SLv     = (SL == 15) ? 31 : SL
slreach = (level >> 4) == SLv
zero    = level == 0
silent  = (level & 0x1F8) == 0x1F8          // level ≥ 0x1F8
mute    = state != ATTACK && silent && !dokon

next = dokon          ? ATTACK :
       !key           ? RELEASE :
       state == ATTACK ? (zero ? DECAY : ATTACK) :
       state == DECAY  ? (slreach ? SUSTAIN : DECAY) :
                         state

linear = !dokon && !silent && (state == SUSTAIN || state == RELEASE || (state == DECAY && !slreach))
expo   = state == ATTACK && key && rhi != 15 && !zero
base   = mute ? 0x1FF : (dokon && rhi == 15) ? 0 : level

add = expo ? ((~level) >> 1) : linear ? 4 : 0          // ~level = −level−1, arithmetic shift
inc = step == 1 ? add >> 2 : step == 2 ? add >> 1 : step == 3 ? add : 0

eg_out = attenuation from the OLD level (3.5)          // computed before the update
level  = (base + inc) & 0x1FF
state  = next
slot_muted_this_sample = mute                           // keeps the output out of the mix
```
Consequences:
- Attack is exponential: `level −= ceil((level+1) / 8)`, `/4` or `/2` per step; it reaches
  0 exactly; ATTACK → DECAY happens on the evaluation after level reaches 0.
- **Key-on does not reset the level.** Attack starts from the current level. Only at the
  key-on edge with `rhi == 15` (AR 15, or AR 14 with large KSR) does the level jump to 0.
- On the key-on sample there is no step, the state becomes ATTACK, the phase resets
  (section 2), and the slot is **not** muted even if its level is silent.
- A key-on while not in RELEASE does nothing (no retrigger without a key-off).
- `!key` moves the state to RELEASE for the next sample; this sample still steps with the
  old state's rate.
- In ATTACK with `rhi == 15` and level ≠ 0 the envelope does not move (AR raised to 15
  after the key-on edge freezes it until key-off).
- Sustain level 15 means 0x1F0 (−93 dB), and DECAY → SUSTAIN happens there.
- EGT = 1: sustain rate 0 (holds); EGT = 0: sustain decays at RR while keyed.
- A slot in DECAY/SUSTAIN/RELEASE whose level reached ≥ 0x1F8 snaps to 0x1FF and is
  left out of the mix. In ATTACK it is never muted.

### 3.5 Attenuation (`eg_out`, 9 bit), from the level before this sample's update

```
static const uint8 kslrom[16] = {0,32,40,45,48,51,53,55,56,58,59,60,61,62,63,64};
k6   = max(0, kslrom[fnum >> 6] - 8*(8 - block))                  // 0..56
kslv = KSL == 0 ? 0 : KSL == 1 ? 2*k6 : KSL == 2 ? k6 : 4*k6       // KSL = reg40 bits 7:6 as 2-bit value
am   = AM ? (DAM ? tout >> 2 : tout >> 4) : 0                      // 0..26 or 0..6
eg_out = min(level + kslv + 4*TL + am, 0x1FF)
if (test0) eg_out = 0
```
KSL value 1 (bit 6 set) is 3 dB/oct, 2 (bit 7) is 1.5 dB/oct, 3 is 6 dB/oct.
`kslv + 4·TL + am` ≤ 502, so only the final sum can exceed 0x1FF.

### 3.6 What changes only on specific samples

- `level` changes only on samples where the slot steps, at a key-on edge with rate 15, or
  when it snaps to 0x1FF. For rates < 48 that is only on `sub == 1` samples, and only when
  `egshift ∈ {12−rhi, 13−rhi (rlo bit 1), 14−rhi (rlo bit 0)}`: so once per sample the
  engine can derive, from `egshift` alone, the set of rate6 values (bitmask over 0..47)
  that step; every other slot with a rate below 48 does no level work.
- `state` changes only when level hits 0 (ATTACK→DECAY) or the SL band (DECAY→SUSTAIN),
  on key changes (writes, drum keys, CSM), or on the key-on edge.
- `eg_out` changes when `level` changes, when `tout` changes (at most every 64 samples, AM
  slots only), or on writes to TL/KSL/AM/A0/B0/DAM.
- The rate inputs (`R`, `rhi`, `rlo`) change only on state changes and register writes
  (AR/DR/SL/RR/KSR/EGT, B0 block, A0/B0 F-number bit 9 or 8, NTS).

---------------------------------------------------------------------------------------

## 4. Operator

### 4.1 ROMs (generate in double precision; the closest value to a rounding tie is 0.0003)

```
logsin[i] = round(-log2(sin((i + 0.5) * π / 512)) * 256)      i = 0..255   (2137 … 0)
expt[i]   = round((2^((255 - i) / 256) - 1) * 1024)            i = 0..255   (1018 … 0)
```
Both match LLE's compressed ROMs bit for bit (checked, all 256 entries). The die stores
128 entries plus a delta for even addresses; the expanded form above is equivalent.

### 4.2 Output (13-bit two's complement, −4085..+4084)

```
p       = (phase + mod) & 1023        // phase: section 2 or rhythm substitute; mod: 4.3
sign    = p & 512
quarter = p & 256
idx     = (p & 255) ^ (quarter ? 255 : 0)
att     = logsin[idx] + (eg_out << 3);  if (att > 4095) att = 4095      // 12-bit clamp
v       = ((expt[att & 255] | 1024) << 1) >> (att >> 8)                // 0..4084
switch (WS) {
  case 0: if (sign) v = ~v;   break      // −v−1 (one's complement in 13 bits)
  case 1: if (sign) v = 0;    break
  case 2:                     break      // |sin|
  case 3: if (quarter) v = 0; break      // rising quarter in quarters 0 and 2
}
```
- A silent sine still outputs −1 in its negative half (eg_out ≥ 384 gives v = 0, then
  `~0`). Whether it reaches the mix depends only on the EG mute (section 6).
- WS 1 and 3 muted parts are exactly 0.
- WS is the slot's stored value; register 01 bit 5 only gates writes (section 7.1).

### 4.3 Modulation input `mod` (10 bit)

```
modulator slot of channel c:
    fbsum = h1[c] + h2[c]                     // last two outputs of this modulator, 13-bit signed each
    mod   = FB ? (fbsum >> (9 - FB)) & 1023 : 0
    // rhythm on: slots 13 (HH) and 14 (TOM) get mod = 0
carrier slot of channel c:
    mod   = CON ? 0 : out(modulator of c, this chip sample) & 1023
    // rhythm on: slots 16 (SD) and 17 (TC) get mod = 0
after the modulator's output `o` is computed:
    h2[c] = h1[c];  h1[c] = o                 // rhythm on: not done for slots 13, 14 (history frozen)
```
- FB = 1..7 → shift 8..2 (π/16 … 4π). The full 13-bit output is added to the phase,
  modulo 1024, with no scaling.
- The history and the carrier input use the modulator's output even when the modulator is
  EG-muted (−1/0 values included).
- `FB` and `CON` here are the modulator-side values (section 7.4).

---------------------------------------------------------------------------------------

## 5. Rhythm mode (BD bit 5)

### 5.1 Keys
```
drum_key(12) = rhy && BD_kon     drum_key(15) = rhy12 && BD_kon      // bass drum, both slots
drum_key(13) = rhy12 && HH_kon   drum_key(14) = rhy12 && TOM_kon
drum_key(16) = rhy12 && SD_kon   drum_key(17) = rhy12 && TC_kon
```
`BD_kon … HH_kon` are BD register bits 4, 3 (SD), 2 (TOM), 1 (TC), 0 (HH). `rhy` is the
rhythm bit at cycle 12 for slot 12; `rhy12` is the rhythm bit latched at cycle 12 and used
by slots 13-17. Drum keys are OR'ed with the channel's own KON (B6-B8). Clearing rhythm
drops all drum keys.

### 5.2 Phases (10-bit phase replacing the PG output for the operator)
```
h2,h3,h7,h8 = bits 2,3,7,8 of slot 13's phase (P>>9) this sample   // captured every sample
if rhythm: c3,c5 = bits 3,5 of slot 17's phase this sample          // captured only in rhythm mode, at slot 17
rm = (h2 ^ h7) | (h3 ^ c5) | (c3 ^ c5)
HH  (slot 13) = (rm << 9) | ((rm ^ hh_noise) ? 0x0D0 : 0x034)       // c-bits from the last capture
SD  (slot 16) = (h8 << 9) | ((h8 ^ sd_noise) << 8)
TC  (slot 17) = (rm << 9) | 0x100                                    // c-bits captured just now
TOM (slot 14), BD (12, 15): normal phases
```
The PG accumulators of slots 13, 16, 17 keep running normally (and reset on key-on); their
own phase values are only used for the h/c bits.

### 5.3 Mix in rhythm mode
- Slot 12 (BD modulator) is never added, even with CON = 1. BD is normal 2-op FM with
  feedback; CON = 1 leaves the BD carrier unmodulated.
- Slots 13 (HH) and 14 (TOM) are always added (CON of ch 7, 8 ignored).
- Slots 12-17 are added ×2 (slot 12 never, as above).
- Ch 7, 8 FB and CON have no effect; their feedback history stays frozen and resumes
  from the frozen values when rhythm mode is turned off.

### 5.4 The rhythm-on cymbal-latch race (LLE artefact)
When the rhythm bit goes 0 → 1 on the clock edge that ends cycle 17 (a global write
placed at cycle 0, which is the test convention of 7.6), LLE opens the c-bit latch for
one evaluation while the phase bus already shows **slot 0's** phase. Result: for that
chip sample only, HH uses `c3, c5 = bits 3, 5 of slot 0's phase (P>>9 at the start of the
sample)`; TC recaptures from slot 17 as usual. Per-sample rule: *when a BD write turns
rhythm on, set c3/c5 from slot 0's current 10-bit phase.* It is two bit copies in the BD
write path. Whether silicon does this depends on gate delays; LLE does, and the test
compares against LLE. (A BD write placed at cycles 2-12 avoids it but splits DAM/DVB
across slots; see 7.6.)

---------------------------------------------------------------------------------------

## 6. Mix, accumulator, DAC

### 6.1 Accumulator (18-bit two's complement)
For each slot u, with its output `o` and EG mute flag of the chip sample it was computed in:
```
include = carrier(u) || (modulator(u) && CON_mod(ch(u)))
if (rhythm) { if (u == 12) include = 0;  if (u == 13 || u == 14) include = 1; }
if (include && !mute(u)) acc += (rhythm && u >= 12) ? 2*o : o
```
Output n is latched at the start of cycle 17 of chip sample n:
```
out18(n) = Σ contributions of slots 15,16,17 of chip sample n−1
         + Σ contributions of slots 0..14 of chip sample n
```
The ×2 weight of slot 15 uses the rhythm bit of chip sample n−1; slots 16 and 17 use the
rhythm bit of chip sample n (they are added in cycles 0, 1 of n). An engine holds the raw
outputs (and mute flags) of slots 15-17 and adds them into the next output.
Range: at most ±89 870, so 18 bits never wrap.

### 6.2 DAC word (what the YM3014B is fed)
The die sends a 10-bit offset-binary mantissa (sign included) and a 3-bit exponent 1..7.
Decoded as an integer, `value = (mantissa − 512) << (exponent − 1)`, it equals:
```
s = clamp(out18, -32768, 32767)
t = s >> 9;  if (t < 0) t = ~t;  t &= 63              // magnitude of the top bits
e1 = t & 32 ? 6 : t & 16 ? 5 : t & 8 ? 4 : t & 4 ? 3 : t & 2 ? 2 : t ? 1 : 0   // exponent − 1
dac = (s >> e1) << e1                                  // floor to a multiple of 2^e1
```
Positive overflow gives mantissa 0x3FF exponent 7 = +32 704; negative overflow gives
−32 768. **`dac` is "the chip's level"**: the value the engine outputs, 16-bit two's
complement, compared bit-exactly with LLE's serial MO word in the test. Scale: one
full-scale operator peak is 4 084 (−18 dBFS); nine full-scale channels clip.
The YM3014B's analog stage adds half an LSB of the current exponent toward positive
(`(mantissa − 511.5)·2^(exponent−1)`, from the YM3014B data sheet formula); that is
analog detail below the digital word and is not modelled.

---------------------------------------------------------------------------------------

## 7. Register writes

### 7.1 Decode (byte address → target)

| Address | Target | Notes |
|---|---|---|
| 01 | test register (bits below) | bit 5 = WSE |
| 02, 03, 04 | timer 1, timer 2, timer control | section 8.4 |
| 08 | bit 6 NTS, bit 7 CSM | |
| BD | bit 7 DAM, 6 DVB, 5 rhythm, 4-0 BD SD TOM TC HH keys | not a B-channel alias |
| 20-35 | slot: AM, VIB, EGT, KSR, MULT | offsets 06,07,0E,0F,16-1F ignored |
| 40-55 | slot: KSL (bits 7:6), TL | |
| 60-75 | slot: AR (7:4), DR (3:0) | |
| 80-95 | slot: SL (7:4), RR (3:0) | |
| E0-F5 | slot: WS (1:0) | stored only while WSE (01 bit 5) is set at landing time |
| A0-A8 | channel F-number bits 7:0 | |
| B0-B8 | channel KON (5), block (4:2), F-number bits 9:8 (1:0) | |
| C0-C8 | channel FB (3:1), CON (0) | |
| A9, AA, AB / B9-BB / C9-CB | channels 0, 1, 2 (aliases, land at cycles 9, 10, 11) | |
| AE, AF / BE, BF / CE, CF | channels 0, 1 (aliases, land at cycles 0, 1) | |
| AC, AD, BC, CC, CD, all else | ignored | |

The slot of an operator register is the slot whose `off(s)` equals `address & 0x1F`.
WSE only gates writes: an E0 write while WSE = 0 is discarded; stored WS values stay in
effect when WSE is cleared; setting WSE later does not recover a discarded write.

### 7.2 Bus to register file (LLE)
- An address write latches the byte. Bytes with bits 7:5 = 0 select 01-08 only (and stop
  any pending operator/channel landing); others become the operator/channel address.
- A data write passes a 2-stage synchroniser: a one-φM WR pulse in the first φM of cycle
  C makes the internal write pulse (`write1`) high during cycles C+2 and C+3 (the test
  driver places writes by this internal cycle, `K`). Address writes behave the same (`write0`).
- Global registers (01-04, 08, BD) change at the start of `write1`'s first cycle k and
  are seen by everything read from cycle k on.
- Operator/channel registers are written into their slot only when the slot counter
  reaches the target slot, from cycle k+1 on, and **again every chip sample** until the
  next address write (the "address once, data many" mode). An address write before the
  landing cancels it. Each data write needs 18 cycles after k+1 for every slot to be
  reachable, so the next address write must come no sooner than cycle k+19.

### 7.3 Landing cycles

| Register | Lands at cycle | First used by |
|---|---|---|
| Operator register of slot s | s | slot s in the **next** chip sample (slot s reads it in cycle s, before the write port) |
| Channel c register (A0-A8, B0-B8, C0-C8, AE, AF, BE, BF, CE, CF) | c (0..8) | reads after cycle c in the same chip sample |
| Channel 0-2 alias (A9-AB, B9-BB, C9-CB) | 9 + c | reads after cycle 9 + c |

### 7.4 First chip sample that sees a write

`write1` starts in cycle k of chip sample m. "ℓ" = the chip sample of the landing:
ℓ = m if the landing cycle > k, else m + 1.

| Field | First chip sample using the new value |
|---|---|
| Operator register, slot s | m+1 if s > k, else m+2 |
| Channel 0-2: carrier F-number, block, KON | ℓ |
| Channel 0-2: modulator F-number, block, KON; CON; FB | ℓ + 1 |
| Channel 3-5: F-number, block, KON (both slots); CON | ℓ |
| Channel 3-5: FB | ℓ + 1 |
| Channel 6-8: everything | ℓ |
| Channel 0-2 aliases (landing 9+c): everything | ℓ + 1 |
| DAM, DVB, NTS, test 0/2 for slot s | m if s ≥ k, else m+1 |
| Other global reads | per the read cycle in 1.1 |

Carrier outputs of ch 6-8 then reach the output one sample later (1.2 item 3).

Consequences no write phase removes:
- A channel 0-2 write (A0-A2, B0-B2, C0-C2, AE, AF, BE, BF, CE, CF) reaches the carrier
  one chip sample before the modulator; a key-on starts the carrier one sample before the
  modulator, and CON and FB follow the modulator.
- FB of channels 3-5 changes one chip sample after their CON and F-number.
- Operator registers wait a full sample in the register ring, channel registers do not.
  The read instants span 26 cycles, more than one chip sample, so **no single bus phase
  makes every register take effect in the same chip sample.**

### 7.5 Two writes in one sample; key-off then key-on

- A register holds one value; each slot reads it once per chip sample at its read cycle.
  A value is seen only if it is stored at a landing cycle and is still there at a read.
- Two data writes to the same register before its landing cycle: only the second is ever
  seen. A new address write before the landing cancels the first write.
- **Key-off then key-on of one channel** retriggers if and only if the key-off value is
  stored at the channel's landing cycle (it then stays for ≥ 18 cycles, so every slot of
  the channel reads KON = 0 at least once, enters RELEASE, and the key-on that lands later
  is a key-on edge: phase reset, attack from the current level). Measured with LLE
  (ch 0 and ch 4, data writes 2..26 cycles apart, with and without a new address write):
  the pair retriggers exactly when the key-off's landing window reached the landing
  cycle. Ch 0 carrier sees the release one sample before the modulator.
- At datasheet pacing (≥ 12 φM after an address write, ≥ 84 φM after a data write: data
  writes ≥ 24 cycles apart) every write lands, and every value written to a register is
  seen by every slot that reads it for at least one chip sample. So on a legal bus the
  chip always retriggers on key-off/key-on, and two writes to the same register never
  take effect in the same chip sample. (Writes to different groups can: an operator write
  and a following ch 6-8 write can both first apply in one chip sample.)
- Per-sample engine semantics for several writes applied before one sample: in order, the
  last value per register wins, and a key-off immediately followed by a key-on of the same
  channel is lost. An engine that spreads bursts must therefore put two writes to the same
  register (address byte) into different samples, in order; writes to different
  registers may share a sample (the chip cannot do that, and it is a superset of chip
  behaviour, not a contradiction).

### 7.6 The per-sample convention (convention A, recommended, verified)

Engine rule: **a write applied before chip sample n takes effect for every slot in chip
sample n**, with three exceptions that are part of the die:
1. Channels 0-2 (A0-A2, B0-B2, C0-C2, AE, AF, BE, BF, CE, CF): the modulator's F-number,
   block, KON and the channel's CON and FB take effect in chip sample n+1; the carrier's
   F-number, block, KON in n.
2. Channels 3-5, C3-C5: FB takes effect in n+1; CON in n.
3. BD turning rhythm on: c3/c5 ← bits 3/5 of slot 0's phase (5.4).

Channel 0-2 aliases A9-AB, B9-BB, C9-CB take effect for both slots and FB in n.

Test driver phases that make LLE do exactly this (internal `write1` start `K`, relative
to cycle 0 of chip sample n):

| Group | `write1` starts at | i.e. |
|---|---|---|
| Operator registers 20-95, E0-F5 | K = −19 | cycle 17 of chip sample n−2 (at the latch of output n−2) |
| Channel registers A0-CF | K = −10 | cycle 8 of chip sample n−1 (any K in −10..−1 if aliases are not written) |
| Global 01, 08, BD | K = 0 | cycle 0 of chip sample n (4 φM after the latch of output n−1) |

Only one K works for operator registers (each slot lands exactly at its own cycle) and
only K = 0 works for DAM/DVB/NTS/test bits. With these phases the engine's output n is
compared with LLE's latch at cycle 17 of chip sample n.

Write spacing in the test: the next address write must start ≥ 19 cycles after an
operator/channel `write1` and ≥ 4 cycles after a global `write1` (the bus data latch is
shared). With the phases above, writes 2 samples apart always satisfy LLE's limits;
1 sample apart works for operator→channel, operator→global, channel→global and
global→global. The datasheet's 84 φM wait is also met at 2 samples, except
global→operator, which needs 3. **The oracle cannot take one write every sample at a
fixed phase** (that is faster than the chip allows: 72 φM < 12 + 84 φM). Scripts with
writes ≥ 2 samples apart test the paced behaviour; one write per sample is defined by the
same engine rule but has no LLE reference.

### 7.7 Alternative: convention B (two phases, verified)

Operator and channel writes at K = −1 (cycle 17 of chip sample n−1), globals at K = 0.
Engine: channel and global writes as in A (same exceptions); **operator registers and
channel 0-2 aliases take effect in n+1** (a one-sample delay line for those writes).
It saves one test phase at the cost of a delay queue and one sample of extra latency for
operator registers; A is preferred because operator writes then take effect in the sample
they arrive.

### 7.8 The engine's write queue and pacing

Writes wait in a queue of 256 and are applied in order, at most one before each chip
sample, at the chip's pace of one per 96 φM. A credit, 96 at reset, paces them:

1. Before chip sample n, the oldest waiting write is applied if the credit is at least 96,
   and the credit pays 96, **unless sample n applies a late half**: the modulator half of a
   channel 0-2 write applied before sample n−1 (7.6 exception 1; A0-A2, B0-B2, C0-C2 and
   AE, AF, BE, BF, CE, CF, which write channels 0-1 the same way). Such a sample applies no
   queued write.
2. Then the credit gains 72, and stops at 192 while writes wait and at 96 while none do.

The FB lag of channels 3-5 (7.6 exception 2) holds nothing back. A burst from rest applies
at samples 0, 2, 3, 4, 6, 7, 8, …, leaving 1, 5, 9, … empty; with a channel 0 write applied
at 2, its late half takes sample 3 and the burst applies at 0, 2, 4, 5, 6, 7, 8. A late
half takes the place of the next empty sample, the credit carrying the write it held back,
so the burst ends when it would have unless no empty sample was left before its last
write. Two late halves between the same two empty samples cost one sample, as the credit
stops at 192, and a run of only channel 0-2 writes goes one per two samples. A write to an
offset that is not a register (the ignored addresses of 7.1 and the timers 02-04) never
enters the queue; one that finds the queue full is dropped and counted.

---------------------------------------------------------------------------------------

## 8. Reset, power-on, timers, test register

### 8.1 IC (reset pin) clears
All registers (test, timers, NTS, CSM, rhythm, DAM, DVB, drum keys, every operator and
channel field), every slot's EG (state RELEASE, level 0x1FF, muted), the EG timer
(T = 0, carry = 0, sub = 0), the tremolo counter and direction, timer state.

### 8.2 IC does not clear
LFO counter, vibrato position (only test bit 7 clears them), noise LFSR (test bit 7
fills it with ones), all 18 phase accumulators, the feedback history, the h/c rhythm
bits, the `egshift`/`eglow` latches, `tstep`. Their power-on values are arbitrary on
silicon; LLE starts them at 0 (memset) and lets the LFO count during reset.

### 8.3 Engine power-on state and test seeding (verified)
Engine reset (also what "mapping the OPL2 resets it" should do):
```
all registers 0; every slot: state RELEASE, level 0x1FF, P = 0; h1 = h2 = 0
T = 0, carry = 0, sub = 0, egshift = 0, eglow = 0
lfo = 0, vib = 0, tv = 0, tdir = 0, tof = 1, tstep = 0
N = 1 (any non-zero value; LLE orientation)
h2 = h3 = h7 = h8 = c3 = c5 = 0; held outputs of slots 15-17 = 0 (muted)
```
LLE: hold IC for several samples and release it at the cycle edge that starts cycle 0
of chip sample 0. The first operator write can be applied before chip sample 2 (its
`write1` must come after the release). Writes during IC are ignored by the die. The
tests then hold test bit 7 over chip samples 2-5, on the die and the engine alike, which
clears the LFO, vibrato and tremolo counters and fills the noise register whatever IC
left there.

### 8.4 Timers, CSM, status: not observable on the RP6502
- Timer 1 (8 bit) counts on samples where `lfo & 3 == 3` (every 4 samples, aligned to the
  LFO counter), timer 2 where `lfo & 15 == 15`; on overflow they reload the preset and set
  a status flag unless masked; a stopped timer reads 0; register 04 bit 7 clears flags.
- Status and IRQ are read paths. The RP6502 OPL2 takes writes to a block of XRAM, with no status
  read and no IRQ, and the user docs already say "no timers, interrupts, or status
  register". So 02, 03, 04 have no observable effect.
- CSM (08 bit 7) is the one audible use: when timer 1 loads (overflow or start edge), all
  18 slots get KON for one chip sample (`csm_pulse`), restarting any slot in RELEASE.
  Recommendation: leave CSM out (consistent with the docs); the test must not set 08
  bit 7 together with 04 bit 0. Adding it later costs a timer-1 counter per sample and an
  OR into every slot's key.

### 8.5 Test register (01)
| Bit | Effect (per sample, with the write at K = 0) |
|---|---|
| 0 | `eg_out = 0` for every slot (full volume); mute flags unchanged |
| 1 | timers count every cycle (unobservable here) |
| 2 | phase accumulators reload `inc` every sample (P = inc) |
| 3 | vibrato position and tremolo counter step every sample |
| 4 | none |
| 5 | WSE: E0-F5 writes are stored only while set |
| 6 | status bit 0 = debug serial stream (unobservable here) |
| 7 | holds LFO, vibrato, tremolo counter/direction at 0 and shifts ones into the noise LFSR; in the first chip sample, slot 0 still uses the old vibrato position (1.3) |

Bits 0, 2, 3, 7 are audible. With the write at a cycle other than 0 the die applies them
mid-sample (bit 3 toggled during cycles 1-8 adds a partial ±mask to the tremolo counter;
the bus-mode model reproduces this bit-serially). The per-sample rules above are exact
for the K = 0 placement. The engine supports bits 3, 5 and 7 and ignores bits 0 and 2.

---------------------------------------------------------------------------------------

## 9. Where emulators usually diverge from the die

1. Negative sine half is `~v` (−v−1), so silent sines output −1; EG-muted slots are left
   out of the mix rather than producing −1s.
2. A silent (muted) modulator still modulates its carrier and feeds its history with
   0/−1 values.
3. Same-sample modulation of the carrier; feedback from the previous two samples; full
   13-bit modulator output added to the 10-bit phase without scaling.
4. Channels 6-8 carriers reach the output one sample late.
5. Phase increment truncates twice: `((f << block) >> 1) * mt >> 1`; MULT table
   1/2,1,2,…,10,10,12,12,15,15.
6. Vibrato modifies the F-number before block/MULT, with half steps from lower F-number
   bits, symmetric, period 8192 samples; KSL/KSR ignore vibrato.
7. Key-on keeps the level, sets P = 0 + inc (waveform keeps the old phase that sample),
   instant attack only at the edge with rhi = 15, AR raised to 15 mid-attack freezes.
8. No retrigger from ATTACK/DECAY/SUSTAIN; key-off/key-on pairs depend on landing
   (7.5).
9. EG: rates < 48 step only on every other sample at positions fixed by the 18-bit timer
   (ctz pattern, bits 0..12, wrap quirk); rates ≥ 48 use the `hi[rlo][T&3]` table; attack
   is `(~level) >> (4 − step)`; SL 15 = 0x1F0; decay→sustain by equality on level >> 4.
10. Tremolo is 0..105 triangle stepping every 64 samples, scaled `>> 2` / `>> 4`
    (0..26 / 0..6), period 13 440 samples; the YM2413 0..13 table is wrong for OPL2.
11. Noise: x^23 + x^14 + 1 stepped 18 times per sample; HH and SD read bits 3 steps
    apart.
12. Rhythm: TC phase `| 0x100` (OPL3 uses 0x80), `rm` uses `(h3 ^ c5)`, HH uses
    previous-sample c-bits, c-bits frozen outside rhythm mode, SD and TC unmodulated, HH and
    TOM without feedback, ch 7/8 feedback history frozen, slot 12 never output, ×2 weights,
    drum keys OR'ed with B6-B8 KON.
13. Output is floored to the YM3014B float format after a 16-bit clamp; positive clip is
    32 704.
14. Channel registers 0-2 reach carrier and modulator one sample apart; FB 3-5 lags CON;
    operator registers wait one sample in the register ring (7.4).
15. The KSL ROM's top entry is 64 (emu8950's ×4 table has 255 where 256 belongs); NTS = 0
    selects F-number bit 9 (the application manual describes it backwards).
16. WSE gates writes only.
17. Aliased channel addresses (A9-AB, AE, AF, …).
18. IC leaves phases, LFOs, noise and feedback history untouched.

---------------------------------------------------------------------------------------

## 10. Reference algorithm (one chip sample)

State as named above. `rhy`, `dam`, `dvb`, `nts`, `test` are the global register values
for this sample. `cv[c]`: carrier view of
channel c {fnum, block, kon}; `mv[c]`: modulator view {fnum, block, kon, con, fb}
(identical except during the one-sample lags of 7.6).

```
sample():
    tout = tv & 127
    of = tout == 0 || (tout & 105) == 105;  if (of && !tof) tdir ^= 1;  tof = of
    if (tstep || test3) tv = (tv + (tdir ? 511 : 1)) & 511
    if (test7) { tv = 0; tdir = 0 }
    hhn = (N >> 9) & 1;  sdn = (N >> 6) & 1

    acc = held15 + w(rhy, 16) * held16 + w(rhy, 17) * held17
          // held15: slot 15's weighted contribution from last sample
          // held16, held17: raw outputs of slots 16, 17 last sample (0 if muted)
    for s in 0..17:
        c = ch(s);  V = modulator(s) ? mv[c] : cv[c]
        key = V.kon | drum_key(s)
        run EG (3.2-3.5) with V.fnum, V.block → eg_out, mute(s), dokon
        phase = (P[s] >> 9) & 1023;  run PG (2) with dokon
        if s == 0 && test7: vib = 0, lfo = 0                 (1.3)
        if s == 13: h-bits ← phase
        if s == 17 && rhy: c-bits ← phase
        if rhy && s in {13,16,17}: phase ← substitute (5.2)
        mod = modulator(s) ? (rhy && s in {13,14} ? 0 : feedback(c, mv[c].fb))
                           : (mv[c].con || (rhy && s in {16,17}) ? 0 : out[mod(c)] & 1023)
        out[s] = operator(phase + mod, WS[s], eg_out)      (4.2)
        if modulator(s) && !(rhy && s in {13,14}): h2[c] = h1[c]; h1[c] = out[s]
        if s <= 14 && included(s) && !mute(s): acc += weight(rhy, s) * out[s]
    held15 = mute(15) ? 0 : w(rhy, 15) * out[15]
    held16 = mute(16) ? 0 : out[16];  held17 = mute(17) ? 0 : out[17]
    output = dac(acc)                                     (6.2)

    // end of sample
    tstep = (lfo & 63) == 63
    vib = test7 ? 0 : (vib + (lfo == 1023 || test3)) & 7
    lfo = test7 ? 0 : (lfo + 1) & 1023
    EG timer update (1.3)
    N = 18 LFSR steps (1.3)
```
`w(r, s)` is 2 when r is set and s ≥ 12, else 1.
`tests/host/emu/opl_model.c` is the executable version: it runs each chip sample
cycle by cycle, reading every field at its cycle of 1.1, with writes applied between
samples as in 7.6.

---------------------------------------------------------------------------------------

## 11. Cost-relevant facts

- **Unavoidable per sample:** 18 PG adds; 18 EG evaluations (most are a table test, 3.6);
  the operator of every non-muted carrier; the operator of every modulator (history and
  carrier input need it every sample), with a cheap path when `eg_out ≥ 384`:
  output = (WS == 0 && sign of `phase + mod`) ? −1 : 0; 18 noise-LFSR steps (word-parallel:
  two 9-step blocks); mix and DAC floor.
- A muted carrier needs no operator evaluation at all (its output goes nowhere).
- **Event-driven per-slot caches** (all exact): phase increment (changes on A0/B0/20
  writes, DVB writes, and when `vib` changes every 1024 samples, VIB slots only);
  `kslv + 4·TL` (writes only); AM term (every 64 samples at most); rate selection
  (state changes and writes); EG step mask per sample from `egshift`/`eglow`/`sub` (once
  per sample).
- **Peak-sample coincidences** the engine must budget for, all on fixed schedules: the
  vibrato position change (every 1024 samples: every VIB slot's increment changes in the
  same sample), the tremolo change (every 64 samples: every AM slot's `eg_out` changes),
  rate ≥ 48 slots stepping every sample, rate < 48 slots stepping together on
  `sub = 1` samples whose `egshift` matches (shift 1 on half of the ticks), and writes.
  Writes that touch every slot: BD (DVB, DAM, rhythm), 08 (NTS), 01 (test bits).
- **Write cost classes** (for spreading bursts): 20 (MULT/VIB/KSR/EGT/AM: increment and
  rate cache of one slot), 40 (attenuation cache of one slot), 60/80 (rate cache), E0
  (WS), A0/B0 (increment, KSL and KSR caches of two slots, KON), C0 (CON/FB), BD/08/01
  (all slots).
- The one-sample lags of 7.6 need a small pending-update record per channel (ch 0-2
  modulator view, ch 3-5 FB) applied at the start of the next sample.
