# Njordlinger Combat Gameplay Overhaul - copyright and licence

Copyright (C) 2026 ApocryphaRealm

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program (`LICENSE`). If not, see
<https://www.gnu.org/licenses/>.

SPDX-License-Identifier: GPL-3.0-or-later

## Credit: Character Progression Control

The progression module is copied from **Character Progression Control** (ApocryphaRealm, GPL-3.0-or-later) and trimmed:
the byte-signature scanner and the patch register (`Signature`, `Patches`), the skill-cap site and its assembly stub,
the skill-improve entry hook and the skill-to-level income hook (`Skills`, `SkillExperience`, `source/SkillCapStub.asm`),
the perk-pool write (`LevelUp`), the attribute-gain hook (`LevelUpGains`) and the level-up menu's choose-attribute
lookup, the settings reader and writer, and the settings-page and DevBench patterns. Each copied file says so at its
top. Character Progression Control credits the Skyrim Skill Uncapper lineage (Kassent, Vadfromnu, Elys, Kasplat) for
the location of the engine sites; no code of theirs is included.

## Why GPL-3.0-or-later

The Skyrim 1.7.x build statically links CommonLibSSE-NG 7.2.0, GPL-3.0-or-later WITH its Modding Exception and GPL-3.0
Linking Exception, and the progression module is copied from a GPL-3.0-or-later work. The whole mod - both build lines,
the data files and the source - is GPL-3.0-or-later.

Components under other licences, with their notices: `THIRD_PARTY_NOTICES.md`.
