#include "lr_paths.hpp"

#include <plg/plugin.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace levels_ranks {
namespace {

constexpr std::string_view kPluginName = "levels_ranks";

std::string g_config_root;
std::string g_config_dir;
std::string g_translations_dir;
std::string g_data_dir;
bool g_cached = false;

void Invalidate() {
	g_cached = false;
	g_config_root.clear();
	g_config_dir.clear();
	g_translations_dir.clear();
	g_data_dir.clear();
}

void FillCache() {
	if (g_cached) {
		return;
	}
	g_config_root = plg::GetConfigsDir ? std::string(plg::GetConfigsDir()) : std::string();
	const std::string data_root = plg::GetDataDir ? std::string(plg::GetDataDir()) : std::string();
	g_config_dir = JoinPath(g_config_root, kPluginName);
	g_translations_dir = JoinPath(g_config_dir, "translations");
	g_data_dir = JoinPath(data_root, kPluginName);
	g_cached = true;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string_view StripPrefix(std::string_view text, std::string_view prefix) {
	return StartsWith(text, prefix) ? text.substr(prefix.size()) : std::string_view{};
}

std::string FirstExisting(const std::vector<std::string>& candidates) {
	for (const std::string& candidate : candidates) {
		if (!candidate.empty() && FileExists(candidate)) {
			return candidate;
		}
	}
	return {};
}

// pisexcompat's Candidates(): the same order, so the first existing file stays authoritative.
std::vector<std::string> Candidates(std::string_view path) {
	std::vector<std::string> out;
	if (path.empty()) {
		return out;
	}
	const std::string raw(path);
	const bool absolute = raw.front() == '/';
	out.push_back(raw); // as given (absolute, or relative to the server working directory)
	if (absolute) {
		return out;
	}

	std::string_view relative = path;
	bool from_translations = false;
	bool from_data = false;
	if (auto rest = StripPrefix(path, "addons/configs/"); !rest.empty()) {
		relative = rest;
		// "levels_ranks/core.ini" -> "core.ini" when it points at our own config dir.
		if (StartsWith(rest, std::string(kPluginName) + "/")) {
			relative = rest.substr(kPluginName.size() + 1);
		}
	} else if (auto rest = StripPrefix(path, "addons/translations/"); !rest.empty()) {
		relative = rest;
		from_translations = true;
	} else if (auto rest = StripPrefix(path, "addons/data/"); !rest.empty()) {
		relative = rest;
		from_data = true;
	} else if (auto rest = StripPrefix(path, "addons/"); !rest.empty()) {
		relative = rest;
	}

	FillCache();
	const std::string rel(relative);
	if (from_translations) {
		out.push_back(JoinPath(g_translations_dir, rel));
	} else if (from_data) {
		out.push_back(JoinPath(g_data_dir, rel));
	} else {
		out.push_back(JoinPath(g_config_dir, rel));
		out.push_back(JoinPath(g_translations_dir, rel));
		out.push_back(JoinPath(g_data_dir, rel));
	}
	return out;
}

} // namespace

void ResetPathCache() {
	Invalidate();
}

std::string ConfigRoot() {
	FillCache();
	return g_config_root;
}

std::string ConfigDir() {
	FillCache();
	return g_config_dir;
}

std::string TranslationsDir() {
	FillCache();
	return g_translations_dir;
}

std::string DataDir() {
	FillCache();
	return g_data_dir;
}

std::string CoreConfigPath() {
	return JoinPath(ConfigRoot(), "core.cfg");
}

std::string JoinPath(std::string_view a, std::string_view b) {
	if (a.empty()) {
		return std::string(b);
	}
	if (b.empty()) {
		return std::string(a);
	}
	std::string out(a);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (b.front() == '/') {
		b.remove_prefix(1);
	}
	out.append(b);
	return out;
}

bool FileExists(std::string_view path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(std::string(path), std::ios::binary);
	return file.good();
}

std::string ReadFile(std::string_view path, bool* ok) {
	std::string content;
	std::ifstream file(std::string(path), std::ios::binary);
	if (!file) {
		if (ok) {
			*ok = false;
		}
		return content;
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	content = buffer.str();
	if (ok) {
		*ok = true;
	}
	return content;
}

std::string ResolveConfigPath(std::string_view path) {
	if (path.empty()) {
		return {};
	}
	const std::vector<std::string> candidates = Candidates(path);
	if (const std::string resolved = FirstExisting(candidates); !resolved.empty()) {
		return resolved;
	}
	// Nothing on disk: hand back the most likely location so the caller can log a useful path.
	if (candidates.size() > 1) {
		return candidates[1];
	}
	return candidates.empty() ? std::string() : candidates.front();
}

std::string ResolveTranslationsPath(std::string_view path) {
	if (path.empty()) {
		return {};
	}
	std::string file(path);
	if (file.size() < 4 || file.compare(file.size() - 4, 4, ".txt") != 0) {
		file += ".txt"; // the original's LoadTranslations() appends ".txt"
	}
	FillCache();
	const std::string direct = FirstExisting(
		{file, JoinPath(g_translations_dir, file), JoinPath(g_config_dir, file)});
	if (!direct.empty()) {
		return direct;
	}
	std::string_view relative = file;
	if (auto rest = StripPrefix(file, "addons/translations/"); !rest.empty()) {
		relative = rest;
	}
	// The Metamod spelling "addons/translations/x.phrases.txt" resolves inside the translations dir.
	return JoinPath(g_translations_dir, relative);
}

std::string ResolveDataPath(std::string_view path) {
	if (path.empty()) {
		return {};
	}
	std::string file(path);
	if (auto rest = StripPrefix(file, "addons/data/"); !rest.empty()) {
		file = std::string(rest);
	}
	FillCache();
	return JoinPath(g_data_dir, file);
}

} // namespace levels_ranks
