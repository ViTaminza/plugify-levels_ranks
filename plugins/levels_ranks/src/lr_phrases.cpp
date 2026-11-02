#include "lr_phrases.hpp"

#include "lr_kv1.hpp"
#include "lr_paths.hpp"

#include <plg/plugin.hpp>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace levels_ranks {
namespace {

// cs2-menus/utils.h: colors_text[] / colors_hex[], same order, same bytes. The last entry is the
// chat line break: "\n" -> U+2029 (e2 80 a9), which the CS2 chat renderer shows as a new line.
struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
												  {"{WHITE}", "\x01"},
												  {"{RED}", "\x02"},
												  {"{LIGHTPURPLE}", "\x03"},
												  {"{GREEN}", "\x04"},
												  {"{LIME}", "\x05"},
												  {"{LIGHTGREEN}", "\x06"},
												  {"{DARKRED}", "\x07"},
												  {"{GRAY}", "\x08"},
												  {"{LIGHTOLIVE}", "\x09"},
												  {"{OLIVE}", "\x10"},
												  {"{LIGHTBLUE}", "\x0B"},
												  {"{BLUE}", "\x0C"},
												  {"{PURPLE}", "\x0E"},
												  {"{LIGHTRED}", "\x0F"},
												  {"{GRAYBLUE}", "\x0A"},
												  {"\\n", "\xe2\x80\xa9"}}};

std::string g_language;
bool g_language_detected = false;

std::string CoreConfigLanguage() {
	KeyValues core("Core");
	if (!FileExists(CoreConfigPath()) || !core.LoadFromFile(CoreConfigPath())) {
		return {};
	}
	const char* language = core.GetString("ServerLang", "");
	return language != nullptr ? std::string(language) : std::string();
}

// One language for every phrase of the plugin, like the original: core.cfg "ServerLang" first,
// then s2sdk's ServerLanguage. Both are plain config reads, so this works at plugin start.
std::string DetectLanguage() {
	if (const std::string core = CoreConfigLanguage(); !core.empty()) {
		return NormalizeLanguage(core);
	}
	if (__s2sdk_GetServerLanguage == nullptr) {
		return "en";
	}
	try {
		const plg::string server_language = s2sdk::GetServerLanguage();
		if (server_language.empty()) {
			return "en";
		}
		return NormalizeLanguage(std::string_view(server_language.data(), server_language.size()));
	} catch (...) {
		return "en";
	}
}

std::string Timestamp(const char* format) {
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
	localtime_r(&now, &tm);
	char buffer[64] = {};
	std::strftime(buffer, sizeof(buffer), format, &tm);
	return std::string(buffer);
}

void AppendLogFile(const std::string& file, const std::string& line) {
	const std::string dir = plg::GetLogsDir ? std::string(plg::GetLogsDir()) : std::string();
	if (dir.empty()) {
		return;
	}
	std::ofstream out(JoinPath(dir, file), std::ios::app);
	if (out) {
		out << line;
	}
}

} // namespace

std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

std::string StripColorTags(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		const std::string_view replacement = color.tag == "\\n" ? std::string_view("\n") : std::string_view();
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), replacement);
			pos += replacement.size();
		}
	}
	return out;
}

std::string FormatString(const char* format, ...) {
	std::array<char, 2048> buffer{};
	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", args);
	va_end(args);
	if (written <= 0) {
		return {};
	}
	return std::string(buffer.data(), static_cast<std::size_t>(written) < buffer.size() - 1
									  ? static_cast<std::size_t>(written)
									  : buffer.size() - 1);
}

std::string NormalizeLanguage(std::string_view name) {
	std::string lower;
	lower.reserve(name.size());
	for (const char c : name) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (lower == "russian" || lower == "ru") return "ru";
	if (lower == "english" || lower == "en") return "en";
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") return "ua";
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") return "pt-BR";
	if (lower == "spanish" || lower == "es") return "es";
	if (lower == "german" || lower == "de") return "de";
	if (lower == "french" || lower == "fr") return "fr";
	if (lower == "polish" || lower == "pl") return "pl";
	if (lower == "turkish" || lower == "tr") return "tr";
	if (lower == "schinese" || lower == "zh-cn") return "zh-CN";
	return std::string(name);
}

void SetPhrasesLanguage(std::string_view language) {
	g_language.assign(language);
	g_language_detected = true;
}

void ResetPhrasesLanguage() {
	g_language.clear();
	g_language_detected = false;
}

const std::string& PhrasesLanguage() {
	if (!g_language_detected) {
		g_language = DetectLanguage();
		g_language_detected = true;
	}
	return g_language;
}

bool LoadPhrasesFile(const char* file, PhraseMap& out, const char* language) {
	if (file == nullptr || *file == '\0') {
		return false;
	}
	KeyValues root("Phrases");
	if (!root.LoadFromFile(ResolveTranslationsPath(file))) {
		return false;
	}

	std::string selected;
	if (language != nullptr && *language != '\0') {
		selected = language;
	} else {
		selected = PhrasesLanguage();
	}
	const std::string fallback = "en";

	for (KeyValues* key = root.GetFirstTrueSubKey(); key != nullptr; key = key->GetNextTrueSubKey()) {
		std::string text;
		if (const KeyValues* localized = key->FindKey(selected.c_str()); localized != nullptr &&
			localized->HasValue()) {
			text = std::string(localized->GetStringValue());
		}
		if (text.empty() && selected != fallback) {
			if (const KeyValues* english = key->FindKey(fallback.c_str());
				english != nullptr && english->HasValue()) {
				text = std::string(english->GetStringValue());
			}
		}
		if (text.empty()) {
			// Last resort: any language the file has, so a partially translated file still shows
			// something instead of an empty chat line.
			for (KeyValues* candidate = key->GetFirstValue(); candidate != nullptr;
				 candidate = candidate->GetNextValue()) {
				if (candidate->HasValue()) {
					text = std::string(candidate->GetStringValue());
					break;
				}
			}
		}
		out[key->GetName()] = std::move(text);
	}
	return true;
}

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

void LogToFile(const char* file, const char* format, ...) {
	std::array<char, 2048> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", args);
	va_end(args);

	const std::string line = "L " + Timestamp("%m/%d/%Y - %H:%M:%S") + ": " + buffer.data();
	if (plg::Log) {
		plg::Log(std::string_view(line), plg::Severity::Info, plg::source_location::current());
	}
	const std::string name = std::string(file != nullptr ? file : "log") + ".txt";
	AppendLogFile(name, line + "\n");
}

} // namespace levels_ranks
