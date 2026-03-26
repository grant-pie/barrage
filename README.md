# Barrage

**16-step burst gate sequencer for VCV Rack**
*VONK — by Grant Pieterse*

---

## Overview

Barrage is a 16-step sequencer where each step is a self-contained burst generator. Instead of a single gate per clock, each step fires a burst of evenly-spaced gates — with independent control over how many gates fire, how long each one is, whether the step fires at all, and how fast the burst runs relative to the clock.

---

## Module Reference

### Per-step controls (columns 1–16)

| Control | Range | Description |
|---------|-------|-------------|
| **COUNT** | 1 – 16 | Number of gates to fire within the step (integer) |
| **LENGTH** | 0 – 100% | High-time fraction of each gate subdivision |
| **PROB** | 0 – 100% | Probability the step fires on each pass |
| **SPEED** | 0.25× – 4× | Burst speed relative to the clock period |

### Global controls

| Control | Description |
|---------|-------------|
| **STEPS** | Number of active steps in the sequence (1–16, integer) |
| **DIR** | Direction mode knob — selects Forward, Reverse, Ping-pong, or Random |

### Inputs

| Jack | Description |
|------|-------------|
| **CLK** | Burst clock — each rising edge fires one gate on the active step's output. Gate high-time = CLK period × LENGTH. |
| **STEP** | Step advance — each rising edge moves the sequencer to the next step. |
| **RST** | Reset input — returns to step 1 on the next step advance. |

### Outputs

| Jack | Description |
|------|-------------|
| **GATE 1–16** | Per-step burst gate output |
| **EOC 1–16** | Fires a pulse when that step's burst completes |
| **EOC** | Fires a pulse at the end of each full sequence cycle (not fired in Random mode) |
| **STEP CV** | Current step as a 0–10 V CV value (0 V = step 1, 10 V = step 16) |

---

## How it works

On each rising **STEP** edge Barrage advances to the next active step and rolls against that step's **PROB** knob. On each rising **CLK** edge, if the current step is active and its burst is not yet complete, a gate fires on that step's output. The gate stays high for the leading **LENGTH** fraction of the CLK period, then goes low. This repeats for **COUNT** CLK pulses, after which the burst is complete and the per-step **EOC** fires. Sending a new STEP edge before the burst finishes cuts it short and fires EOC immediately.

Each step has its own **GATE** output, so all 16 burst generators can be routed independently to different instruments or modules simultaneously.

The **DIR** knob selects one of four direction modes:

| Mode | Behaviour | EOC |
|------|-----------|-----|
| **Forward** | Steps advance 1 → 2 → … → N → 1 | Fires on wrap |
| **Reverse** | Steps advance N → N-1 → … → 1 → N | Fires on wrap |
| **Ping-pong** | Steps bounce 1 → N → 1 → N → … without repeating endpoints | Fires when bouncing off step 1 |
| **Random** | A random step is chosen on each clock | Never fires |

---

## Tips

- Patch **STEP CV** into a quantizer or V/Oct input to turn the sequencer into a melodic line driven by burst rhythm.
- Use **EOC 1–16** to chain modules or trigger envelopes at the moment each burst finishes rather than when it starts.
- Set **SPEED** above 1× with **COUNT** > 1 to pack many fast gates into a single clock step; set it below 1× for slow, sparse single gates that trail into the next step.
- Clock two instances together with different **DIR** modes — e.g. one in Forward and one in Ping-pong — for evolving polyrhythmic textures.

---

## Building

Requires the [VCV Rack SDK](https://vcvrack.com/manual/Building).

```bash
# clone into your Rack plugins folder
git clone <repo-url> Barrage
cd Barrage
make install
```

---

## License

Proprietary — © Grant Pieterse. All rights reserved.
