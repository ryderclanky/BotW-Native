// Shared text and selection state for the Wii U software keyboard.
#pragma once

#include <algorithm>
#include <mutex>
#include <string>

namespace swkbd {

inline bool decode_utf8(const std::string& input, std::u16string& output) {
    output.clear();
    for (size_t i = 0; i < input.size();) {
        const auto first = static_cast<uint8_t>(input[i]);
        uint32_t codepoint = 0;
        size_t count = 0;
        if (first <= 0x7f) { codepoint = first; count = 1; }
        else if (first >= 0xc2 && first <= 0xdf) { codepoint = first & 0x1f; count = 2; }
        else if (first >= 0xe0 && first <= 0xef) { codepoint = first & 0x0f; count = 3; }
        else if (first >= 0xf0 && first <= 0xf4) { codepoint = first & 0x07; count = 4; }
        else return false;
        if (i + count > input.size()) return false;
        for (size_t j = 1; j < count; ++j) {
            const auto next = static_cast<uint8_t>(input[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((count == 3 && codepoint < 0x800) || (count == 4 && codepoint < 0x10000) ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
        if (codepoint <= 0xffff) output.push_back(static_cast<char16_t>(codepoint));
        else {
            codepoint -= 0x10000;
            output.push_back(static_cast<char16_t>(0xd800 + (codepoint >> 10)));
            output.push_back(static_cast<char16_t>(0xdc00 + (codepoint & 0x3ff)));
        }
        i += count;
    }
    return true;
}

inline std::string encode_utf8(const std::u16string& input) {
    std::string output;
    for (size_t i = 0; i < input.size(); ++i) {
        uint32_t codepoint = input[i];
        if (codepoint >= 0xd800 && codepoint <= 0xdbff && i + 1 < input.size() &&
            input[i + 1] >= 0xdc00 && input[i + 1] <= 0xdfff) {
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (input[++i] - 0xdc00);
        } else if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
            codepoint = 0xfffd;
        }
        if (codepoint <= 0x7f) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }
    return output;
}

struct View {
    bool active = false;
    bool accepted = false;
    bool cancelled = false;
    int selected = 0;
    std::string text;
};

class Keyboard {
public:
    static constexpr const char* keys = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-?";

    void show(const std::u16string& initial, size_t max_length) {
        std::lock_guard<std::mutex> lock(mu_);
        limit_ = std::min<size_t>(max_length ? max_length : 4095, 4095);
        text_ = initial.substr(0, limit_);
        if (!text_.empty() && text_.back() >= 0xd800 && text_.back() <= 0xdbff) text_.pop_back();
        active_ = true;
        accepted_ = cancelled_ = false;
        selected_ = 0;
        dirty_ = true;
    }

    void close() { std::lock_guard<std::mutex> lock(mu_); active_ = false; }

    void replace(const std::u16string& text) {
        std::lock_guard<std::mutex> lock(mu_);
        text_ = text.substr(0, limit_);
        if (!text_.empty() && text_.back() >= 0xd800 && text_.back() <= 0xdbff) text_.pop_back();
        dirty_ = true;
    }

    void input(const std::string& utf8) {
        std::u16string decoded;
        if (!decode_utf8(utf8, decoded)) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (!active_ || accepted_ || cancelled_) return;
        for (size_t i = 0; i < decoded.size();) {
            char16_t c = decoded[i];
            size_t n = c >= 0xd800 && c <= 0xdbff && i + 1 < decoded.size() ? 2 : 1;
            if (c >= 32 && text_.size() + n <= limit_) {
                text_.append(decoded, i, n);
                dirty_ = true;
            }
            i += n;
        }
    }

    void backspace() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!active_ || accepted_ || cancelled_ || text_.empty()) return;
        char16_t c = text_.back();
        text_.pop_back();
        if (c >= 0xdc00 && c <= 0xdfff && !text_.empty() &&
            text_.back() >= 0xd800 && text_.back() <= 0xdbff) text_.pop_back();
        dirty_ = true;
    }

    void accept() { std::lock_guard<std::mutex> lock(mu_); if (active_) accepted_ = true; }
    void cancel() { std::lock_guard<std::mutex> lock(mu_); if (active_) cancelled_ = true; }

    void move(int dx, int dy) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!active_) return;
        int row = selected_ / 10, col = selected_ % 10;
        row = (row + dy + 5) % 5;
        int width = row == 4 ? 4 : 10;
        col = std::min(col, width - 1);
        col = (col + dx + width) % width;
        selected_ = row * 10 + col;
    }

    void select(int index) {
        if (index < 0 || index >= 44) return;
        if (index < 40) input(std::string(1, keys[index]));
        else if (index == 40) input(" ");
        else if (index == 41) backspace();
        else if (index == 42) accept();
        else cancel();
    }

    View view() const {
        std::lock_guard<std::mutex> lock(mu_);
        View result{active_, accepted_, cancelled_, selected_, {}};
        result.text = encode_utf8(text_);
        return result;
    }

    std::u16string text(bool* changed = nullptr) {
        std::lock_guard<std::mutex> lock(mu_);
        if (changed) { *changed = dirty_; dirty_ = false; }
        return text_;
    }

private:
    mutable std::mutex mu_;
    std::u16string text_;
    size_t limit_ = 4095;
    int selected_ = 0;
    bool active_ = false, accepted_ = false, cancelled_ = false, dirty_ = false;
};

Keyboard& keyboard();
}  // namespace swkbd
