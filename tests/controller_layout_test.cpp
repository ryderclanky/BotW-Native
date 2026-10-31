#include "host/controller_layout.h"

#include <cassert>

int main() {
    using controller_layout::Action;
    using controller_layout::Face;
    using controller_layout::Family;

    // A DualSense in Auto follows BotW's native positions: Circle, Cross,
    // Triangle, Square map to A, B, X, Y respectively.
    assert(controller_layout::fixed_face(-1, Family::PlayStation, Action::A) == Face::East);
    assert(controller_layout::fixed_face(-1, Family::PlayStation, Action::B) == Face::South);
    assert(controller_layout::fixed_face(-1, Family::PlayStation, Action::X) == Face::North);
    assert(controller_layout::fixed_face(-1, Family::PlayStation, Action::Y) == Face::West);

    // Switch already uses native positions; Xbox remains label-based in Auto.
    assert(controller_layout::fixed_face(-1, Family::Nintendo, Action::A) == Face::East);
    assert(controller_layout::fixed_face(-1, Family::Xbox, Action::A) == Face::Unknown);

    // Explicit preferences override device family.
    assert(controller_layout::fixed_face(0, Family::Xbox, Action::A) == Face::East);
    assert(controller_layout::fixed_face(1, Family::PlayStation, Action::A) == Face::South);
}
