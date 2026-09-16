# thestrongestpokemon

Determines the strongest Pokemon by a set of user-defined criteria

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
