# Third-party notices

## GAdmin

Gambit Record reuses and adapts the SA-MP version detection, version-specific address tables, event identifiers, Direct3D9/ImGui integration approach, theme layout, and GTA cursor-state implementation from:

- Project: GAdmin — Plugin simplifying the work of administrators on Gambit-RP
- Repository: https://github.com/Vadim-Kamalov/GAdmin
- Commit: `c31749c02f3d76c1ab0f8ebf562c8dae0dc91152`
- Copyright: © 2023–2026 The Contributors
- License: GNU General Public License v3.0 only

The derived source is marked in `grecord/src/Plugin.cpp` and `grecord/src/UiStyle.cpp`. The palette format, style values, collapsible navigation, outlined switches and button animations are adapted from this revision. Gambit Record as a combined work is distributed under GPL-3.0-only.

## Embedded fonts and icons

- Noto Sans Regular, Bold and Light: Copyright 2022 The Noto Project Authors (https://github.com/notofonts/latin-greek-cyrillic), SIL Open Font License 1.1. Unmodified font files from the GAdmin revision above. Full license: `grecord/resources/OFL-NotoSans.txt` in source, `grecord/licenses/OFL-NotoSans.txt` when installed.
- Coolicons: Kryston Schwarze (https://github.com/krystonschwarze/coolicons), Creative Commons Attribution 4.0 International. The unmodified `coolicons.ttf` distributed by the GAdmin revision above is embedded; selected glyphs are used for navigation. Full license: `grecord/resources/CC-BY-4.0-Coolicons.txt` in source, `grecord/licenses/CC-BY-4.0-Coolicons.txt` when installed. License URL: https://creativecommons.org/licenses/by/4.0/.

## Other dependencies

- Dear ImGui — MIT License, pinned in `grecord/CMakeLists.txt`.
- MinHook — BSD 2-Clause License, pinned in `grecord/CMakeLists.txt`.
- nlohmann/json — MIT License, pinned through CMake/vcpkg.

The corresponding license texts are distributed by each dependency's build package and source repository.
