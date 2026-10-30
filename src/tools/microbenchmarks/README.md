# Micro-benchmarks

Standalone, engine-independent programs. They are **not** part of `QUARK ENGINE.sln`
and are not built with the game. Each one re-implements the data structure and loop of a
past engine optimization in a few lines, checks that the optimized variant keeps the
original semantics (prints `...=PASS` / `...=FAIL`), and then compares old and new timing.

| File | Models | Engine code it mirrors |
|---|---|---|
| `relcase_shared_manager_smoke.cpp` | broadcasting "object destroyed" to the shared relcase manager once per object per frame instead of once per broadcaster | the "object released" (`relcase`) broadcast to shared managers, e.g. `xrGame/ai/monsters/ai_monster_squad_manager.*`, `base_monster.*` |
| `ph_commander_compaction_smoke.cpp` | compacting the physics commander call list in one pass instead of repeated `vector::erase` | `CPHCommander` (`xrGame/PHCommander.h`) |

Build and run (any C++17 compiler):

```
cl /O2 /EHsc /std:c++17 relcase_shared_manager_smoke.cpp
cl /O2 /EHsc /std:c++17 ph_commander_compaction_smoke.cpp
```

or `g++ -O2 -std=c++17 <file>.cpp -o <name>`.

The programs are self-contained models, not tests of the real engine classes; the mapping above is
the closest production code, not a link checked by the compiler.
