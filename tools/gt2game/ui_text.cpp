#include "ui_text.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gt2game {
namespace {

struct Entry {
    const char* english;
    const char* portuguese; // UTF-8
};

constexpr Entry kPortuguese[] = {
#include "ui_text_pt.inc"
};

// Suffixes the menus append to values ("130% (restart)", "105 cm (default)").
constexpr Entry kSuffixes[] = {
    {" (restart)", " (reiniciar)"},
    {" (default)", " (padrão)"},
    {"  [Left / Right]", "  [Esq. / Dir.]"},
    {" [extended]", " [estendido]"},
    {" [unavailable]", " [indisponível]"},
    {"   [not set]", "   [não definido]"},
    {"   [set]", "   [definido]"},
    {"...", "..."},
};

// UTF-8 -> Latin-1 (U+0000..U+00FF); anything beyond that becomes '?', which the translations never contain.
std::string Latin1(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) { out.push_back(char(c)); continue; }
        if ((c & 0xe0) == 0xc0 && i + 1 < utf8.size()) {
            const unsigned code = ((c & 0x1fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3fu);
            out.push_back(code <= 0xff ? char(code) : '?');
            ++i;
            continue;
        }
        out.push_back('?');
        while (i + 1 < utf8.size() && (static_cast<unsigned char>(utf8[i + 1]) & 0xc0) == 0x80) ++i;
    }
    return out;
}

struct Tables {
    std::unordered_map<std::string, std::string> whole;
    std::vector<std::pair<std::string, std::string>> prefixes; // keys ending in ' ' (or ": "), longest first
    std::vector<std::pair<std::string, std::string>> suffixes;
    Tables() {
        for (const Entry& e : kPortuguese) {
            const std::string english = e.english;
            if (whole.contains(english)) continue;
            whole.emplace(english, Latin1(e.portuguese));
            if (english.size() > 1 && english.back() == ' ') prefixes.emplace_back(english, Latin1(e.portuguese));
        }
        std::sort(prefixes.begin(), prefixes.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
        for (const Entry& e : kSuffixes) suffixes.emplace_back(e.english, Latin1(e.portuguese));
    }
};

const Tables& Portuguese() {
    static const Tables tables;
    return tables;
}

std::atomic<bool> g_portuguese{[] {
    const char* code = std::getenv("GT2_UI_LANGUAGE");
    return code && (code[0] == 'p' || code[0] == 'P') && (code[1] == 't' || code[1] == 'T');
}()};

// A value after "Label: ": whole match, or a known suffix after a translatable / untranslatable head.
std::string Value(const Tables& t, const std::string& value) {
    if (value.empty()) return value;
    if (const auto it = t.whole.find(value); it != t.whole.end()) return it->second;
    for (const auto& [english, portuguese] : t.suffixes)
        if (value.size() > english.size() && value.ends_with(english))
            return Value(t, value.substr(0, value.size() - english.size())) + portuguese;
    for (const auto& [english, portuguese] : t.prefixes) // "Button 3", "Gear 2"
        if (value.size() > english.size() && value.starts_with(english)) return portuguese + value.substr(english.size());
    return value;
}

std::string Translate(const Tables& t, const std::string& text, int depth) {
    if (text.empty() || depth > 4) return text;
    if (const auto it = t.whole.find(text); it != t.whole.end()) return it->second;
    // "GT2 VR / HUD / PAGE 2": each segment.
    if (const size_t slash = text.find(" / "); slash != std::string::npos && text.find(": ") == std::string::npos) {
        std::string out;
        size_t start = 0;
        for (;;) {
            const size_t end = text.find(" / ", start);
            const std::string segment = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            out += Translate(t, segment, depth + 1);
            if (end == std::string::npos) break;
            out += " / ";
            start = end + 3;
        }
        return out;
    }
    // "Label: value".
    if (const size_t colon = text.find(": "); colon != std::string::npos) {
        const std::string label = text.substr(0, colon + 2);
        if (const auto it = t.whole.find(label); it != t.whole.end()) return it->second + Value(t, text.substr(colon + 2));
        const std::string bare = text.substr(0, colon); // button / axis names: "Steering: Axis 1"
        if (const auto it = t.whole.find(bare); it != t.whole.end()) return it->second + ": " + Value(t, text.substr(colon + 2));
    }
    // "Press a key for Throttle...", "PAGE 3", "Select gear 2".
    for (const auto& [english, portuguese] : t.prefixes)
        if (text.size() > english.size() && text.starts_with(english)) return portuguese + Value(t, text.substr(english.size()));
    return Value(t, text);
}

} // namespace

void SetUiLanguage(const std::string& code) {
    g_portuguese = code.size() >= 2 && (code[0] == 'p' || code[0] == 'P') && (code[1] == 't' || code[1] == 'T');
}

bool UiPortuguese() { return g_portuguese; }

std::string Tr(const std::string& english) {
    if (!g_portuguese) return english;
    return Translate(Portuguese(), english, 0);
}

} // namespace gt2game
