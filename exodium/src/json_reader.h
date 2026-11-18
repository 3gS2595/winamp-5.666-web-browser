// Just enough of a JSON reader for what the plugin is handed as JSON: the DevTools
// protocol's answers (session_history.cpp) and other browsers' bookmark files
// (bookmarks.cpp). Values are read in the order they come, and the ones that aren't
// wanted are skipped.
#pragma once

#include "text_util.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace exodium {

class JsonReader {
public:
    // Deeper than anything read with it; past it a value isn't skipped but refused.
    static constexpr int kMaxDepth = 64;

    explicit JsonReader(std::wstring_view text) noexcept : text_(text) {}

    [[nodiscard]] bool AtEnd() noexcept {
        SkipSpace();
        return at_ == text_.size();
    }

    // Whether the value that comes next is an object.
    [[nodiscard]] bool AtObject() noexcept {
        SkipSpace();
        return at_ < text_.size() && text_[at_] == L'{';
    }

    // An object: `member` is called with each key, reads its value and says whether it could.
    template <typename Member>
    [[nodiscard]] bool Object(Member&& member) {
        if (!Take(L'{')) return false;
        if (Take(L'}')) return true;
        do {
            const std::optional<std::wstring> key = String();
            if (!key || !Take(L':') || !member(*key)) return false;
        } while (Take(L','));
        return Take(L'}');
    }

    // An array: `element` reads each value and says whether it could.
    template <typename Element>
    [[nodiscard]] bool Array(Element&& element) {
        if (!Take(L'[')) return false;
        if (Take(L']')) return true;
        do {
            if (!element()) return false;
        } while (Take(L','));
        return Take(L']');
    }

    [[nodiscard]] std::optional<std::wstring> String() {
        if (!Take(L'"')) return std::nullopt;
        std::wstring text;
        while (at_ < text_.size()) {
            const wchar_t c = text_[at_++];
            if (c == L'"') return text;
            if (c != L'\\') {
                text += c;
                continue;
            }
            if (at_ == text_.size()) break;
            switch (const wchar_t escaped = text_[at_++]) {
            case L'"':
            case L'\\':
            case L'/':
                text += escaped;
                break;
            case L'b':
                text += L'\b';
                break;
            case L'f':
                text += L'\f';
                break;
            case L'n':
                text += L'\n';
                break;
            case L'r':
                text += L'\r';
                break;
            case L't':
                text += L'\t';
                break;
            case L'u': {
                // One UTF-16 unit, which is what a wstring holds: a character outside the
                // basic plane comes as two escapes, one for each half of its pair.
                unsigned unit = 0;
                for (int i = 0; i < 4; ++i) {
                    const int digit = at_ < text_.size() ? HexDigitValue(text_[at_++]) : -1;
                    if (digit < 0) return std::nullopt;
                    unit = unit * 16 + static_cast<unsigned>(digit);
                }
                text += static_cast<wchar_t>(unit);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt; // never closed
    }

    [[nodiscard]] std::optional<int> Integer() {
        SkipSpace();
        const std::size_t begin = at_;
        if (at_ < text_.size() && text_[at_] == L'-') ++at_;
        while (at_ < text_.size() && IsDigit(text_[at_])) ++at_;
        // A fraction or an exponent makes it no integer.
        if (at_ < text_.size() && (text_[at_] == L'.' || text_[at_] == L'e' || text_[at_] == L'E')) return std::nullopt;
        return ParseNumber<int>(text_.substr(begin, at_ - begin));
    }

    [[nodiscard]] bool Skip(int depth = 0) {
        SkipSpace();
        if (at_ == text_.size() || depth > kMaxDepth) return false;
        switch (text_[at_]) {
        case L'{':
            return Object([&](const std::wstring&) { return Skip(depth + 1); });
        case L'[':
            return Array([&] { return Skip(depth + 1); });
        case L'"':
            return String().has_value();
        case L't':
            return TakeWord(L"true");
        case L'f':
            return TakeWord(L"false");
        case L'n':
            return TakeWord(L"null");
        default: {
            const std::size_t begin = at_;
            while (at_ < text_.size() && (IsDigit(text_[at_]) || std::wstring_view(L"+-.eE").contains(text_[at_]))) {
                ++at_;
            }
            return at_ > begin;
        }
        }
    }

private:
    [[nodiscard]] static constexpr bool IsDigit(wchar_t c) noexcept { return c >= L'0' && c <= L'9'; }

    void SkipSpace() noexcept {
        while (at_ < text_.size() && std::wstring_view(L" \t\r\n").contains(text_[at_])) ++at_;
    }

    [[nodiscard]] bool Take(wchar_t c) noexcept {
        SkipSpace();
        if (at_ == text_.size() || text_[at_] != c) return false;
        ++at_;
        return true;
    }

    [[nodiscard]] bool TakeWord(std::wstring_view word) noexcept {
        if (!text_.substr(at_).starts_with(word)) return false;
        at_ += word.size();
        return true;
    }

    std::wstring_view text_;
    std::size_t at_ = 0;
};

} // namespace exodium
