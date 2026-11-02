#pragma once
// levels_ranks — a self-contained Valve KeyValues (KV1) reader.
//
// The production configs of the ported plugin are the original Metamod files and must keep being
// read byte for byte: settings.ini, databases.cfg, core.cfg (ServerLang) and the two
// *.phrases.txt files. The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7), so the hl2sdk
// KeyValues the original used through pisexcompat is replaced by this reader. The grammar and the
// lookup semantics are the ones the previous port relied on:
//
//   * quoted ("key" "value") and unquoted tokens, nested "key" { ... } blocks, a key may carry a
//     value and a block,
//   * // and /* */ comments, escapes \" \\ \n \r \t inside quotes,
//   * #base / #include / #define lines are skipped (no inclusion),
//   * a file that is a single "Root" { ... } block is merged into the object (Valve's loader
//     behaviour), so KeyValues kv("LR_Settings"); kv.LoadFromFile(...); kv.FindKey("MainSettings")
//     works exactly like the Pisex loader did,
//   * FindKey is case-insensitive; GetInt/GetFloat/GetBool fall back to Valve's atoi/strtof
//     semantics and answer the default for a missing/empty value,
//   * iteration (GetFirstTrueSubKey/GetNextTrueSubKey, GetFirstValue/GetNextValue) keeps the file
//     order, which the Ranks block depends on.
//
// A malformed file leaves an empty tree and returns false, so callers keep their defaults instead
// of reading a half-parsed config.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace levels_ranks {

class KeyValues {
public:
	explicit KeyValues(std::string_view name);
	KeyValues(const KeyValues&) = delete;
	KeyValues& operator=(const KeyValues&) = delete;
	~KeyValues();

	// --- loading -----------------------------------------------------------------------------
	// The path is used as given: callers resolve the plugin config locations themselves.
	bool LoadFromFile(std::string_view path);
	bool LoadFromBuffer(std::string_view text);

	// --- identity ----------------------------------------------------------------------------
	const char* GetName() const { return m_name.c_str(); }
	void SetName(std::string_view name) { m_name = std::string(name); }
	bool IsEmpty(std::string_view key) const;

	// --- lookup ------------------------------------------------------------------------------
	// First child with that name (case-insensitive); bCreate adds an empty child when missing.
	KeyValues* FindKey(const char* key, bool create = false);
	const KeyValues* FindKey(const char* key) const;
	KeyValues* FindKeyWithValue(const char* key);

	// --- values ------------------------------------------------------------------------------
	std::string_view GetStringValue(const char* key = nullptr, std::string_view def = {}) const;
	const char* GetString(const char* key = nullptr, const char* def = "") const;
	int GetInt(const char* key = nullptr, int def = 0) const;
	float GetFloat(const char* key = nullptr, float def = 0.0f) const;
	bool GetBool(const char* key = nullptr, bool def = false) const;

	void SetString(std::string_view key, std::string_view value);
	void SetInt(std::string_view key, int value);

	// --- structure / iteration (Valve's naming) ----------------------------------------------
	bool HasChildren() const { return !m_children.empty(); }
	bool HasValue() const { return m_has_value; }
	std::size_t GetSubKeyCount() const { return m_children.size(); }

	KeyValues* AddSubKey(std::string_view name);
	KeyValues* GetSubKey(std::size_t index);
	const KeyValues* GetSubKey(std::size_t index) const;

	KeyValues* GetFirstSubKey();
	const KeyValues* GetFirstSubKey() const;
	KeyValues* GetNextKey();
	const KeyValues* GetNextKey() const;

	KeyValues* GetFirstTrueSubKey();
	const KeyValues* GetFirstTrueSubKey() const;
	KeyValues* GetNextTrueSubKey();
	const KeyValues* GetNextTrueSubKey() const;

	KeyValues* GetFirstValue();
	const KeyValues* GetFirstValue() const;
	KeyValues* GetNextValue();
	const KeyValues* GetNextValue() const;

	std::vector<KeyValues*> GetSubKeyList();
	void Clear();

private:
	bool IsValueNode() const { return m_has_value; }
	bool IsBlockNode() const { return !m_children.empty(); }

	std::string m_name;
	std::string m_value;
	bool m_has_value = false;
	KeyValues* m_parent = nullptr;
	std::vector<std::unique_ptr<KeyValues>> m_children;
};

// Pisex (hl2sdk) iteration macros, kept verbatim so ported loops compile unchanged.
#define FOR_EACH_SUBKEY(kvParent, kvName) \
	for (levels_ranks::KeyValues* kvName = (kvParent)->GetFirstTrueSubKey(); kvName; \
		 kvName = kvName->GetNextTrueSubKey())

#define FOR_EACH_VALUE(kvParent, kvName) \
	for (levels_ranks::KeyValues* kvName = (kvParent)->GetFirstValue(); kvName; \
		 kvName = kvName->GetNextValue())

} // namespace levels_ranks
