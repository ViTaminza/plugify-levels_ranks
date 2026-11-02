#pragma once
// levels_ranks — where the plugin's configs, translations and data live.
//
// The production files are the original Metamod ones, so the lookup order of the previous port
// (pisexcompat's ResolvePath) is kept exactly; the first existing candidate wins:
//
//   addons/configs/<x>/file   -> the "addons/configs/" prefix is stripped; when the rest starts
//                                with the plugin name that part is stripped as well, then
//                                <configs>/levels_ranks/file, translations/file, data/file
//   addons/translations/file  -> <configs>/levels_ranks/translations/file
//   addons/data/file          -> <data>/levels_ranks/file
//   anything else             -> as given (relative to the server working directory), then
//                                <configs>/levels_ranks/file, translations/file, data/file
//
// Phrases additionally try the translations directory first (LoadPhrasesFile of the original
// called GetTranslationsDir()). The configs root is also where core.cfg ("ServerLang") lives.
//
// The plugify host fills plg::GetConfigsDir/GetDataDir; a test provides them itself.

#include <string>
#include <string_view>

namespace levels_ranks {

// Drops the cached directories (a host/test that changes plg::GetConfigsDir between runs).
void ResetPathCache();

std::string ConfigRoot();        // <GetConfigsDir()>
std::string ConfigDir();         // <configs>/levels_ranks
std::string TranslationsDir();   // <configs>/levels_ranks/translations
std::string DataDir();           // <data>/levels_ranks
std::string CoreConfigPath();    // <configs>/core.cfg (cs2-menus "ServerLang")

std::string JoinPath(std::string_view a, std::string_view b);
bool FileExists(std::string_view path);
// Reads a whole file; returns an empty string and sets *ok=false when it cannot be opened.
std::string ReadFile(std::string_view path, bool* ok = nullptr);

// The candidate search described above; returns the first existing path, or the most likely
// location when nothing exists (so errors can log a useful path).
std::string ResolveConfigPath(std::string_view path);
// The translations-first search (".txt" is appended when missing, like the original).
std::string ResolveTranslationsPath(std::string_view path);
// The data-directory-first search (SQLite files).
std::string ResolveDataPath(std::string_view path);

} // namespace levels_ranks
