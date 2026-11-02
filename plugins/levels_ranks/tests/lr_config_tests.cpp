// levels_ranks config tests — the plugin-local replacements for the pieces pisexcompat supplied:
// the KV1 reader, the config/translation path priority, the phrase loader/colour tags and the
// KeyValuesView -> Settings adapter. No engine and no database: only files under tests/data.
//
// Run by ./test.sh (build/tests/lr_config_tests).

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

#include "lr_core.hpp"
#include "lr_keyvalues.hpp"
#include "lr_kv1.hpp"
#include "lr_paths.hpp"
#include "lr_phrases.hpp"

#ifndef LR_TEST_DATA_DIR
#define LR_TEST_DATA_DIR "."
#endif

// The plugify globals a plugin defines through PLUGIFY_PLUGIN. The path/phrase modules use the
// three directory getters and the logger; the test drives them with the fixture tree.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;
std::string g_data_dir;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (got \"%.*s\", want \"%.*s\")\n", what.c_str(),
					static_cast<int>(actual.size()), actual.data(), static_cast<int>(expected.size()),
					expected.data());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (got %lld, want %lld)\n", what.c_str(),
					static_cast<long long>(actual), static_cast<long long>(expected));
	}
}

plg::string FakeConfigsDirFn() {
	return plg::string((g_data_dir + "/configs").c_str());
}

plg::string FakeDataDirFn() {
	return plg::string((g_data_dir + "/data").c_str());
}

plg::string FakeLogsDirFn() {
	return plg::string((g_data_dir + "/logs").c_str());
}

// --- KV1 ---------------------------------------------------------------------------------------

void TestKv1() {
	std::printf("-- KV1 reader\n");
	levels_ranks::KeyValues root("Test");
	Check(root.LoadFromBuffer(R"(
"Test"
{
	// a comment
	"quoted" "value with spaces"
	unquoted value
	"escaped" "a\"b\\c\nd"
	"nested"
	{
		"inner" "42"
	}
	"both" "scalar" { "child" "1" }
	/* block comment */
	"#base" "ignored"
}
)"),
		  "the buffer parses");
	CheckEqStr(root.GetString("quoted", ""), "value with spaces", "quoted value");
	CheckEqStr(root.GetString("unquoted", ""), "value", "unquoted value");
	CheckEqStr(root.GetString("escaped", ""), "a\"b\\c\nd", "escape sequences");
	CheckEqInt(root.GetInt("nested/inner", -1), -1, "GetInt does not descend into a block");
	const levels_ranks::KeyValues* nested = root.FindKey("nested", false);
	Check(nested != nullptr, "nested block exists");
	CheckEqInt(nested != nullptr ? nested->GetInt("inner", -1) : -1, 42, "nested integer");
	const levels_ranks::KeyValues* both = root.FindKey("both", false);
	Check(both != nullptr, "a key may carry a value and a block");
	CheckEqStr(both != nullptr ? both->GetString(nullptr, "") : "", "scalar", "the key's own value");
	CheckEqInt(both != nullptr ? both->GetInt("child", -1) : -1, 1, "the key's block child");

	// Missing keys answer the defaults; GetInt keeps Valve's atoi semantics.
	CheckEqInt(root.GetInt("missing", 7), 7, "missing int default");
	CheckEqStr(root.GetString("missing", "def"), "def", "missing string default");
	Check(std::abs(root.GetFloat("quoted", 1.5f) - 1.5f) < 0.0001f, "non-numeric float keeps the default");

	// A malformed file is rejected and leaves an empty tree.
	levels_ranks::KeyValues broken("Broken");
	Check(!broken.LoadFromBuffer("\"Broken\" { \"unterminated"), "an unterminated string is rejected");
	Check(!broken.HasChildren(), "a rejected tree stays empty");

	// A missing file is false.
	Check(!root.LoadFromFile(g_data_dir + "/does-not-exist.ini"), "a missing file is rejected");
}

void TestKv1File() {
	std::printf("-- KV1 file + iteration order\n");
	levels_ranks::KeyValues kv("LR_Settings");
	Check(kv.LoadFromFile(g_data_dir + "/settings.ini"), "the shipped settings.ini parses");
	CheckEqStr(kv.GetString("table", "wrong"), "wrong", "root-level key lookup is direct-child only");
	const levels_ranks::KeyValues* main = kv.FindKey("MainSettings", false);
	Check(main != nullptr, "MainSettings exists");
	CheckEqStr(main != nullptr ? main->GetString("lr_table", "") : "", "lvl_base", "lr_table");
	const levels_ranks::KeyValues* ranks = kv.FindKey("Ranks", false);
	Check(ranks != nullptr, "Ranks exists");
	int count = 0;
	std::string first;
	std::string last;
	for (const levels_ranks::KeyValues* entry = ranks != nullptr ? ranks->GetFirstValue() : nullptr;
		 entry != nullptr; entry = entry->GetNextValue()) {
		if (count == 0) {
			first = entry->GetName();
		}
		last = entry->GetName();
		++count;
	}
	CheckEqInt(count, 18, "eighteen rank entries");
	CheckEqStr(first, "rank_1", "iteration keeps the file order (first)");
	CheckEqStr(last, "rank_18", "iteration keeps the file order (last)");
}

// --- paths -------------------------------------------------------------------------------------

void TestPaths() {
	std::printf("-- config/translation/data path priority\n");
	levels_ranks::ResetPathCache();
	CheckEqStr(levels_ranks::ConfigDir(), g_data_dir + "/configs/levels_ranks", "config dir");
	CheckEqStr(levels_ranks::TranslationsDir(), g_data_dir + "/configs/levels_ranks/translations",
			   "translations dir");
	CheckEqStr(levels_ranks::DataDir(), g_data_dir + "/data/levels_ranks", "data dir");
	CheckEqStr(levels_ranks::CoreConfigPath(), g_data_dir + "/configs/core.cfg", "core.cfg path");

	const std::string settings = levels_ranks::ResolveConfigPath("addons/configs/levels_ranks/settings.ini");
	CheckEqStr(settings, g_data_dir + "/configs/levels_ranks/settings.ini",
			   "the Metamod spelling resolves inside the plugin config dir");
	const std::string db = levels_ranks::ResolveConfigPath("addons/configs/databases.cfg");
	CheckEqStr(db, g_data_dir + "/configs/levels_ranks/databases.cfg", "databases.cfg resolves");
	const std::string phrase = levels_ranks::ResolveTranslationsPath("addons/translations/lr_core.phrases.txt");
	CheckEqStr(phrase, g_data_dir + "/configs/levels_ranks/translations/lr_core.phrases.txt",
			   "the translation spelling resolves in the translations dir");
	const std::string sqlite = levels_ranks::ResolveDataPath("addons/data/levels_ranks.sqlite3");
	CheckEqStr(sqlite, g_data_dir + "/data/levels_ranks/levels_ranks.sqlite3", "the data spelling resolves");

	// A path as given wins over the plugin dir (the previous port's first candidate). Change the
	// working directory to the raw fixture so the relative "addons/configs/..." really exists.
	const std::filesystem::path previous = std::filesystem::current_path();
	std::error_code error;
	std::filesystem::current_path(g_data_dir + "/rawprio", error);
	if (!error) {
		levels_ranks::ResetPathCache();
		const std::string raw = levels_ranks::ResolveConfigPath("addons/configs/levels_ranks/settings.ini");
		CheckEqStr(raw, "addons/configs/levels_ranks/settings.ini",
				   "the working-directory file is authoritative when it exists");
		levels_ranks::KeyValues kv("LR_Settings");
		Check(kv.LoadFromFile(raw), "the raw file loads");
		const levels_ranks::KeyValues* main = kv.FindKey("MainSettings", false);
		CheckEqStr(main != nullptr ? main->GetString("lr_table", "") : "", "raw_table",
				   "the raw fixture is the file the plugin would read");
		std::filesystem::current_path(previous, error);
	} else {
		Check(false, "could not enter the raw-priority fixture");
	}
	levels_ranks::ResetPathCache();
}

// --- phrases -----------------------------------------------------------------------------------

void TestPhrases() {
	std::printf("-- phrases / colour tags / language\n");
	levels_ranks::ResetPhrasesLanguage();
	CheckEqStr(levels_ranks::PhrasesLanguage(), "ru",
			   "core.cfg ServerLang (russian) selects the phrase language");

	levels_ranks::PhraseMap map;
	Check(levels_ranks::LoadPhrasesFile("addons/translations/lr_core.phrases.txt", map),
		  "the phrase file loads");
	CheckEqStr(map["Prefix"], "[LR]", "the selected language");
	CheckEqStr(map["Multi"], "RU text", "selected language entry");
	CheckEqStr(map["OnlyEn"], "English only", "the en fallback");
	CheckEqInt(static_cast<int64_t>(map.size()), 3, "one entry per phrase key");

	levels_ranks::PhraseMap english;
	Check(levels_ranks::LoadPhrasesFile("addons/translations/lr_core.phrases.txt", english, "de"),
		  "an unknown language still loads the file");
	CheckEqStr(english["Multi"], "EN text", "the en fallback for an unknown language");
	CheckEqStr(english["OnlyEn"], "English only", "the only language available");

	CheckEqStr(levels_ranks::Colorize("{RED}x{DEFAULT}\\ny"),
			   std::string("\x02x\x01\xe2\x80\xa9y"), "colour bytes and the chat newline");
	CheckEqStr(levels_ranks::StripColorTags("{RED}x\\ny"), "x\ny", "tags stripped for the console");
	CheckEqStr(levels_ranks::NormalizeLanguage("Russian"), "ru", "language name normalisation");
	CheckEqStr(levels_ranks::NormalizeLanguage("pt-BR"), "pt-BR", "language code normalisation");
	CheckEqStr(levels_ranks::NormalizeLanguage("Klingon"), "Klingon", "an unknown name is kept");
	CheckEqStr(levels_ranks::FormatString("%s %i", "x", 7), "x 7", "printf formatting");

	levels_ranks::SetPhrasesLanguage("en");
	CheckEqStr(levels_ranks::PhrasesLanguage(), "en", "SetPhrasesLanguage wins");
	levels_ranks::ResetPhrasesLanguage();
}

// --- settings adapter --------------------------------------------------------------------------

void TestSettingsAdapter() {
	std::printf("-- KeyValuesView -> Settings\n");
	levels_ranks::KeyValues kv("LR_Settings");
	Check(kv.LoadFromFile(g_data_dir + "/settings.ini"), "the shipped settings.ini loads");
	levels_ranks::KeyValuesView view(kv);
	std::string problem;
	const levels_ranks::Settings settings = levels_ranks::LoadSettings(view, &problem);
	Check(problem.empty(), "no structural problem");
	CheckEqStr(settings.table_name, "lvl_base", "lr_table");
	CheckEqStr(settings.plugin_title, "Levels Ranks", "lr_plugin_title");
	CheckEqInt(settings.Value(levels_ranks::LR_MinplayersCount), 4, "lr_minplayers_count");
	CheckEqInt(settings.Value(levels_ranks::LR_TopCount), 5, "lr_top_count");
	CheckEqInt(settings.Stats(levels_ranks::LR_ExpKill), 5, "Rating/Funded kill exp");
	CheckEqInt(static_cast<int64_t>(settings.rank_exp.size()), 18, "eighteen ranks");
	CheckEqStr(settings.rank_names.empty() ? "<empty>" : settings.rank_names.front(), "rank_1",
			   "first rank key");
	CheckEqInt(settings.rank_exp.empty() ? -1 : settings.rank_exp.back(), 100000, "last rank exp");

	// The config-dir fixture (a smaller file) parses through the same reader.
	levels_ranks::KeyValues small("LR_Settings");
	Check(small.LoadFromFile(levels_ranks::ResolveConfigPath("addons/configs/levels_ranks/settings.ini")),
		  "the config-dir settings.ini loads");
	levels_ranks::KeyValuesView small_view(small);
	const levels_ranks::Settings small_settings = levels_ranks::LoadSettings(small_view, nullptr);
	CheckEqStr(small_settings.table_name, "cfg_table", "the config-dir file is the one read");
	CheckEqStr(small_settings.plugin_title, "Config Dir Title", "config-dir title");

	// databases.cfg: the section and the SQLite path the plugin would build.
	levels_ranks::KeyValues db("Databases");
	Check(db.LoadFromFile(levels_ranks::ResolveConfigPath("addons/configs/databases.cfg")),
		  "databases.cfg loads");
	const levels_ranks::KeyValues* section = db.FindKey("levels_ranks", false);
	Check(section != nullptr, "the levels_ranks section exists");
	CheckEqStr(section != nullptr ? section->GetString("driver", "") : "", "sqlite", "driver");
	CheckEqStr(section != nullptr ? section->GetString("sqlite_path", "") : "", "levels_ranks.sqlite3",
			   "sqlite_path");
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("levels_ranks config tests\n");

	g_data_dir = LR_TEST_DATA_DIR;
	plg::GetConfigsDir = &FakeConfigsDirFn;
	plg::GetDataDir = &FakeDataDirFn;
	plg::GetLogsDir = &FakeLogsDirFn;

	TestKv1();
	TestKv1File();
	TestPaths();
	TestPhrases();
	TestSettingsAdapter();

	std::printf("lr_config_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
