# Third-party notices

## GAdmin

Gambit Record reuses and adapts the SA-MP version detection, version-specific address tables, event identifiers, and Direct3D9/ImGui integration approach from:

- Project: GAdmin — Plugin simplifying the work of administrators on Gambit-RP
- Repository: https://github.com/Vadim-Kamalov/GAdmin
- Commit: `c31749c02f3d76c1ab0f8f562c8dae0dc91152`
- Copyright: © 2023–2026 The Contributors
- License: GNU General Public License v3.0 only

The derived source is marked in `grecord/src/Plugin.cpp`. Gambit Record as a combined work is distributed under GPL-3.0-only.

## Other dependencies

- Dear ImGui — MIT License, pinned in `grecord/CMakeLists.txt`.
- MinHook — BSD 2-Clause License, pinned in `grecord/CMakeLists.txt`.
- nlohmann/json — MIT License, pinned through CMake/vcpkg.

The corresponding license texts are distributed by each dependency's build package and source repository.
