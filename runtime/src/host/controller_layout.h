#pragma once

namespace controller_layout {

enum class Family { Xbox, PlayStation, Nintendo, Other };
enum class Action { A, B, X, Y };
enum class Face { South, East, West, North, Unknown };

// BotW is a Wii U game: its native face-button positions are A=right, B=bottom,
// X=top, Y=left. Auto keeps those positions on PlayStation and Nintendo pads;
// Xbox pads keep their labeled A/B/X/Y positions. Explicit layout choices win.
constexpr Face fixed_face(int layout, Family family, Action action) {
    constexpr Face native[] = {Face::East, Face::South, Face::North, Face::West};
    constexpr Face xbox[] = {Face::South, Face::East, Face::West, Face::North};
    const int i = static_cast<int>(action);
    if (i < 0 || i >= 4) return Face::Unknown;
    if (layout == 0 || (layout < 0 && (family == Family::Nintendo || family == Family::PlayStation)))
        return native[i];
    if (layout > 0) return xbox[i];
    return Face::Unknown;  // Auto can still use SDL's labels for other devices.
}

}  // namespace controller_layout
