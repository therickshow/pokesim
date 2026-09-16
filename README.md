# PokeSim

**Which Pokémon is actually the strongest?**

Not by base stat total, and not by anyone's opinion — by making all 1025 of them
fight each other and counting who wins.

PokeSim runs a full round robin: every species against every other species,
**524,800 pairings**, several battles each, using the real damage formula, the
real type chart, and real move data. Roughly six million battles take about six
seconds. The ranking is whatever falls out of that.

It is a desktop application written in C with a GTK 3 interface.

---

## What you can do with it

**Browse the Pokédex.** All 1025 species with sprites, stats, and their move
lists. Search by name, filter by type, sort by any column. Pick one and it shows
you what it is weak to, what it resists and what it ignores entirely — worked
out live from the type chart rather than looked up in a table.

**Run a duel.** Type two names (the boxes autocomplete), see their stats side by
side with the better of each pair highlighted, and run as many battles as you
like. You get a win rate with a confidence interval, how long fights last, total
damage each way, critical hits, misses, and which moves each side actually chose.

**Run the tournament.** One button, a progress bar, and a few seconds later a
ranked table of all 1025 — wins, losses, draws, win rate, average battle length
and a damage ratio. Sortable and filterable.

**Read the type chart.** All 18×18 of it.

---

## Running it

Requires [MSYS2](https://www.msys2.org/) with GTK 3 (`mingw-w64-x86_64-gtk3`)
and `mingw-w64-x86_64-pkgconf`.

```powershell
python "..\library\scripts\build.py" --run   # build and open the window
```

It also works from a terminal without the GUI:

```powershell
build\pokesim.exe --duel "Charizard" "Blastoise" 1000
build\pokesim.exe --tournament 11    # full round robin
build\pokesim.exe --report           # what is in the data files
build\pokesim.exe --test             # 57 self-tests
```

The window opens filling your screen's work area — the desktop minus the
taskbar — as reported by the system, and resizes freely from there.

---

## How a battle actually works

Two Pokémon, four moves each, alternating in speed order until one faints or a
turn cap is reached. Damage uses the real formula:

```
Damage = ( ((2L/5 + 2) × Power × A/D) / 50 + 2 ) × STAB × Type × Crit × Random × Burn
```

Also modelled: burn, poison, toxic, paralysis, sleep and freeze; stat stages;
healing; recoil and drain; power points, and Struggle when a Pokémon runs out of
them. The 29 damaging moves that have no fixed power get their real behaviour —
Seismic Toss deals damage equal to the level, Gyro Ball scales with the speed
ratio, Low Kick with the target's weight.

### Choosing four moves from a hundred

Most species learn far more than four moves, so something has to choose. PokeSim
takes **the best move of each distinct type first**, so a Pokémon has coverage,
fills any spare slots with the next best regardless of type, and saves the last
slot for the best status move it knows.

This matters more than it sounds. The obvious alternative — take the last four
moves it learns — gives Charizard four Fire moves and no coverage, throws away
its Air Slash and Dragon Claw for being learned at level 1, and makes it lose
every single fight to Blastoise.

### Choosing a move each turn

A small set of rules, in order: finish the target if a hit would do it; heal if
badly hurt; set up early; apply a status condition if the target has none;
otherwise hit as hard as possible.

"As hard as possible" means what a move *nets*, not what it deals. Flare Blitz
hits harder than Air Slash but costs its user a third of the damage in recoil.
Ignoring that, Charizard out-damages Venusaur eight to one and still loses 999
fights in 1000 to its own recoil.

---

## What is and is not simulated

Two deliberate choices shape every number this produces.

**Stats are raw base stats**, not run through the level/IV/EV formula. That is
fine for five of the six, because damage depends on Attack *divided by* Defence
and a ratio does not care what scale its halves are on. HP is the exception — it
is used on its own — so HP alone is multiplied by a constant, chosen by
measuring battle lengths rather than by taste.

**There is no switching.** Every fight is one Pokémon against one Pokémon until
someone faints. Real Pokémon answers Toxic and stall tactics largely by
switching out, so both are considerably stronger here than they would be in a
real battle. Bulky Pokémon that can heal do very well.

Not modelled at all: abilities, held items, weather, terrain, entry hazards, and
any status move outside a hand-written effects table. Fling and Natural Gift
need items; Bide and Spit Up need multi-turn state. Those are excluded outright
rather than approximated, and the move chooser knows never to pick them.

---

## Where the data comes from

| File | Contents |
|---|---|
| `pokemon_1025_stats_types_moves.csv` | 1025 species: types, six base stats, level-up moves |
| `type_chart_18x18.csv` | The full type effectiveness grid |
| `moves.csv` | 708 moves: type, category, power, accuracy, PP |
| `pokemon_weights.csv` | Weights, for Low Kick and the other weight-based moves |
| `sprites/` | 1025 32×32 sprites, one per Pokédex number |

Move and weight data is built from [PokéAPI](https://pokeapi.co/)'s published
CSVs. Every one of the 708 move names the roster refers to matched exactly.

Everything is validated on load and the program refuses to start on bad data
rather than silently ranking nonsense — every row's stated stat total has to
equal the sum of its six stats, Pokédex numbers must be contiguous, and the type
chart's header must match the order the code expects.

---

## Layout

| File | Role |
|---|---|
| `pokesim_data.{c,h}` | Loading and validating the CSVs. Knows nothing about GTK. |
| `pokesim_battle.{c,h}` | The simulator: damage, status, move choice, round robin. |
| `pokesim.c` | The GTK interface, plus the console modes and the self-tests. |

Built with MSYS2 gcc using `-Wall -Wextra -std=c17`, warning-free.

---

Part of the `Github projz` workspace. This project is self-contained and has its
own repository — it does not depend on any sibling project.
