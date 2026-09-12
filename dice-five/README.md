# Dice5

Roll **five dice at once** on your Pebble Time 2 (emery). Built for board games and Yahtzee-style nights.

## How to play

- **SELECT / UP / DOWN** — roll all 5 dice
- Dice tumble with a staggered settle (each die lands a beat after the previous one)
- The **sum** shows below the table; **best roll** and **roll count** are persisted across launches

## Build

```bash
cd dice-five && pebble build
```

## Screenshots

Pending — emulator display stack is currently broken on the build host (SDL grey-frame issue); will be captured after a qemu+pypkjs restart.
