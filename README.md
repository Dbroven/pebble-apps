# pebble-apps

A collection of games for Pebble smartwatches (Rebble ecosystem). Each game lives in its own folder and builds with the Pebble SDK (`pebble-tool`). All games target the **emery** platform (Pebble Time 2).

## Games

| Game | Version | Description |
|------|---------|-------------|
| [NeonRun](neon-run/) | v1.2.2 | Neon cat runner — jump, double jump, slide past spikes and drones, collect crystals, build combos. |
| [NeonRunLanes](neon-run-lanes/) | v1.6.3 | 3-lane pseudo-3D neon runner — switch lanes, jump barriers, ride trains, dodge hover drones, trail crystals. |
| [Dice5](dice-five/) | v1.0.0 | Roll 5 dice at once — staggered tumble animation, sum, persisted best roll. |
| [PULSE](pulse-runner/) | v2.0.0 | Endless tunnel runner with Tron-style walls, rifts, procedural chiptune audio and an animated Pulse character. |

## Previews

**NeonRun**

<p align="center">
  <img src="neon-run/screenshots/title.png" width="180" alt="NeonRun title screen">
  <img src="neon-run/screenshots/gameplay.png" width="180" alt="NeonRun gameplay">
</p>

## Install

Download a ready-made `.pbw` from [Releases](../../releases) and sideload it with the Pebble app, or build from source:

```bash
cd neon-run && pebble build
```
