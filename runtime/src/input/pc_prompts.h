// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was
// not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Derived from Zelda-on-PC src/pcedition/PCPrompts.{h,cpp} (https://github.com/mircea32000/Zelda-on-PC).
//
// Keyboard & mouse button prompts. The game draws its button prompts with glyphs U+E040..U+E08F of
// Font/Font_<region>.sbfarc:External_00.bffnt. At startup the archive is taken from the game (or a mod that
// replaces it), External_00.bffnt is swapped for Zelda-on-PC's prompt font (resources/pcedition, one glyph per
// key / mouse button at U+E100+VK), the game's button codes are pointed at the glyphs of the current bindings,
// and the result (plus the grown ResourceSizeTable entry) is written to <user>/pc_prompts. Those files are
// served ahead of every mod (mods::set_private_override); a release build sharing the user folder ignores them.
#pragma once
#include <stdint.h>

#include <string>

namespace pcprompts {
// After mods::init. Rebuilds only when the bindings, the template or the source files changed.
void prepare(const std::string& game_root, const std::string& user_root);
bool active();
// Glyphs for the prompts of the game's menus (pc_ui.cpp re-targets them there): left click, mouse wheel, the Back
// key, Sort (the Crouch key, which is Y in menus). 0 when the prompts are off or the font has no such glyph.
enum class MenuGlyph { Click, Wheel, Back, Sort };
uint16_t menu_glyph(MenuGlyph which);
}  // namespace pcprompts
