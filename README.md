# thestrongestpokemon

Determines the strongest Pokemon by a set of user-defined criteria

## What it does

Finds the strongest Pokemon by simulating a round robin: all 1025 species
fight every other species, and the ranking falls out of who wins.

Four tabs:

- **Pokedex** -- all 1025 species, searchable and filterable by type,
  with stats, sprites, live type matchups and each species' move list.
- **Duel** -- pick two by name (the boxes autocomplete), compare their stats
  side by side, and run any number of battles to get a win rate with a
  confidence interval, turn spread, damage, crits, misses and move usage.
- **Ranking** -- runs the full 524,800-pairing tournament behind a progress
  bar, then shows a sortable, filterable table of the results.
- **Type chart** -- the full 18x18 grid.

```powershell
python "..\library\scripts\build.py" --run     # open the window
build\thestrongestpokemon.exe --test            # 57 self-tests
build\thestrongestpokemon.exe --report          # data summary
build\thestrongestpokemon.exe --duel "Charizard" "Blastoise" 1000
build\thestrongestpokemon.exe --tournament 11   # round robin, console
```

The window is a fixed 16:10 (1200x750 logical px) and is deliberately not
resizable -- the column widths are budgeted against that width.

## How a battle works

The real damage formula, with STAB, type effectiveness, critical hits, the
85-100% damage roll, accuracy and speed-based turn order:

    Damage = ( ((2L/5 + 2) * Power * A/D) / 50 + 2 ) * STAB * Type * Crit
             * Random * Burn

Also modelled: burn, poison, toxic, paralysis, sleep and freeze; stat stages;
healing; recoil and drain; PP, and Struggle when a Pokemon runs dry. The 29
damaging moves with no fixed power get their real formulas -- Seismic Toss
deals damage equal to the level, Gyro Ball scales with the speed ratio, Low
Kick with the target's weight.

Each Pokemon fights with the last four moves it learns, and picks a move each
turn by a heuristic: finish the target if a hit would do it, heal below 30%
HP, set up early, apply a status condition, otherwise hit as hard as possible.

### Two deliberate choices worth knowing

**Stats are raw base stats**, not run through the level/IV/EV formula. That is
fine for five of the six, because damage uses Attack divided by Defence and a
ratio does not care about scale. HP is the exception -- it is used on its own
-- so it alone is multiplied by `HP_SCALE`, set to 14 by measuring battle
lengths rather than by taste. Fights average about eight turns.

**Not everything is modelled.** Fling and Natural Gift need held items, Bide
and Spit Up need multi-turn state; those four are excluded rather than faked.
Status moves outside the hand-written effects table do nothing, and the move
chooser knows not to pick them. There is no switching, so Toxic is far
stronger here than it is in a real game.

## Build

```powershell
python "..\library\scripts\build.py"          # debug
python "..\library\scripts\build.py" --release
python "..\library\scripts\build.py" --run    # build then run
```

Or in VS Code: **Ctrl+Shift+B**.

Compiled with `C:\msys64\mingw64\bin\gcc.exe` using `-Wall -Wextra -std=c17 -g`.
The executable lands in `build/`, which is gitignored.

## Push

```powershell
python "..\library\scripts\push.py" "what I changed"
```

---

Part of the `Github projz` workspace. This project is self-contained and has its
own repository — it does not depend on any sibling project.
