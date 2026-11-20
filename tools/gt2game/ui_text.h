#pragma once
// The language of the port's own menus (pc_overlay.cpp: settings, disc picker, wheel setup, campaign screens). The
// game's own text comes from the disc and is not touched.
//
// English is the default and the menus' source text. Brazilian Portuguese (ui_text_pt.inc) is chosen with
// SetUiLanguage("pt"): the Android phone build follows the system language, other builds GT2_UI_LANGUAGE=pt.
// Translated text is Latin-1 (one byte per character, bytes 0xA0..0xFF for the accented letters): the native menu
// font draws those from its extended glyphs, and every width computed from string lengths stays right.
#include <string>

namespace gt2game {

void SetUiLanguage(const std::string& code); // "pt", "pt-BR", "pt_BR": Portuguese; anything else: English
bool UiPortuguese();
// `english` as shown in the current language. Whole strings first; then "Label: value" (each half on its own),
// "A / B / C" titles (each segment), "Prefix rest" for prefixes ending in a space, and known suffixes such as
// " (default)". Unknown text (names, numbers, file paths) comes back unchanged.
std::string Tr(const std::string& english);

} // namespace gt2game
