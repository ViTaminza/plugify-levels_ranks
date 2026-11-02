#pragma once
// levels_ranks — translations, chat colours and the printf formatting of the ported plugin.
//
// The production phrase files are the original Metamod ones (KeyValues: "Key" { "ru" "..."
// "en" "..." }) and are read with the plugin-local KV1 reader. The behaviour is the one the
// previous port got from pisexcompat:
//
//   * the language is core.cfg "ServerLang" of the configs root, then s2sdk's ServerLanguage,
//     then "en"; Source language names are normalised to the codes the phrase files use,
//   * a missing selected language falls back to "en", then to any language the entry has,
//   * an unknown key is answered with the key itself,
//   * {TAG} colour tags become the cs2-menus chat bytes exactly as the Metamod server sent them,
//     and are stripped for the server console and log files.
//
// Everything here is engine-free apart from the optional s2sdk language query, so the tests drive
// it directly.

#include <map>
#include <string>
#include <string_view>

namespace levels_ranks {

using PhraseMap = std::map<std::string, std::string>;

// Replaces {TAG} colour tags with the cs2-menus chat colour bytes.
std::string Colorize(std::string_view text);
// The same tags removed ("\\n" becomes a real newline): for the server console / log files.
std::string StripColorTags(std::string_view text);

// printf into a std::string (2048 bytes, the buffer of the previous port).
std::string FormatString(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Maps a Source language name ("russian", "portuguese", ...) to the phrase-file code.
std::string NormalizeLanguage(std::string_view name);

// The current language; detected on first use.
void SetPhrasesLanguage(std::string_view language);
const std::string& PhrasesLanguage();
// Drops the cached language so the next PhrasesLanguage() detects it again (tests/reloads).
void ResetPhrasesLanguage();

// Loads one *.phrases.txt into `out`; when `language` is null the current language is used.
// Returns false when the file cannot be read or is malformed (out is left untouched then).
bool LoadPhrasesFile(const char* file, PhraseMap& out, const char* language = nullptr);

// --- logging ---------------------------------------------------------------------------------
// One line through plg::Log (guarded: a host without logging must not crash).
void LogInfo(std::string_view message);
void LogError(std::string_view message);
// Timestamped line to the server console and <logs>/<file>.txt (the original's LogToFile).
void LogToFile(const char* file, const char* format, ...) __attribute__((format(printf, 2, 3)));

} // namespace levels_ranks
