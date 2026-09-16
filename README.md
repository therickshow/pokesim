# thestrongestpokemon

Determines the strongest Pokemon by a set of user-defined criteria

## What it does

A GTK Pokedex over the 1025-species roster and the 18x18 type chart:
search, type filter, sortable columns, per-species stat bars, live type
matchups computed from the chart, and the full chart on its own tab.

```powershell
python "..\library\scripts\build.py" --run   # open the window
build\thestrongestpokemon.exe --report        # console summary
build\thestrongestpokemon.exe --test          # 38 self-tests
```

The window is a fixed 16:10 (1200x750 logical px) and is deliberately not
resizable -- the column widths are budgeted against that width.

There are no battles yet. Ranking every species by round-robin simulation
needs move data (power, accuracy, type, physical/special) for the 708 moves
the roster refers to, which the current CSVs do not contain.

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
