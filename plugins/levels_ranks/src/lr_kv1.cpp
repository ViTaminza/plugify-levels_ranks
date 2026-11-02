#include "lr_kv1.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace levels_ranks {
namespace {

char Lower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (Lower(a[i]) != Lower(b[i])) {
			return false;
		}
	}
	return true;
}

std::string_view Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

// The tokenizer and grammar of the KeyValues reader the previous port used (pisexcompat's, the
// same code s2guard shipped):
//
//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
class Parser {
public:
	explicit Parser(std::string_view text) : m_text(text) {}

	bool Parse(KeyValues& root) {
		ParseNodes(root, false);
		return !m_failed;
	}

private:
	void SkipSpace() {
		while (m_pos < m_text.size()) {
			const char c = m_text[m_pos];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++m_pos;
				continue;
			}
			if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
				m_pos += 2;
				while (m_pos < m_text.size() && m_text[m_pos] != '\n') {
					++m_pos;
				}
				continue;
			}
			if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '*') {
				m_pos += 2;
				while (m_pos + 1 < m_text.size() && !(m_text[m_pos] == '*' && m_text[m_pos + 1] == '/')) {
					++m_pos;
				}
				m_pos = (m_pos + 1 < m_text.size()) ? m_pos + 2 : m_text.size();
				continue;
			}
			break;
		}
	}

	// Reads one token. Returns false at end of input or at a '}' (out stays untouched).
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (m_pos >= m_text.size() || m_text[m_pos] == '}') {
			return false;
		}
		out.clear();
		if (m_text[m_pos] == '"') {
			++m_pos;
			while (m_pos < m_text.size()) {
				const char c = m_text[m_pos++];
				if (c == '\\' && m_pos < m_text.size()) {
					const char esc = m_text[m_pos++];
					switch (esc) {
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					default:
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			m_failed = true; // unterminated string
			return true;
		}
		while (m_pos < m_text.size()) {
			const char c = m_text[m_pos];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			if (c == '/' && m_pos + 1 < m_text.size() &&
				(m_text[m_pos + 1] == '/' || m_text[m_pos + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++m_pos;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return m_pos < m_text.size() && m_text[m_pos] == c;
	}

	void ParseNodes(KeyValues& parent, bool insideBlock) {
		while (true) {
			SkipSpace();
			if (m_pos >= m_text.size()) {
				if (insideBlock) {
					m_failed = true; // missing '}'
				}
				return;
			}
			if (m_text[m_pos] == '}') {
				if (!insideBlock) {
					m_failed = true; // stray '}'
				}
				++m_pos;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: no inclusion, skip the line.
				while (m_pos < m_text.size() && m_text[m_pos] != '\n') {
					++m_pos;
				}
				continue;
			}
			KeyValues* node = parent.AddSubKey(key);
			if (!PeekIs('{')) {
				std::string value;
				if (ReadToken(value)) {
					node->SetString({}, value);
				}
			}
			if (PeekIs('{')) {
				++m_pos;
				ParseNodes(*node, true);
			}
		}
	}

	std::string_view m_text;
	std::size_t m_pos = 0;
	bool m_failed = false;
};

} // namespace

KeyValues::KeyValues(std::string_view name) : m_name(name) {}

KeyValues::~KeyValues() = default;

bool KeyValues::LoadFromFile(std::string_view path) {
	Clear();
	std::ifstream file(std::string(path), std::ios::binary);
	if (!file) {
		return false;
	}
	std::ostringstream content;
	content << file.rdbuf();
	return LoadFromBuffer(content.str());
}

bool KeyValues::LoadFromBuffer(std::string_view text) {
	Clear();
	Parser parser(text);
	if (!parser.Parse(*this)) {
		// A malformed file must not leave a half-parsed tree behind: the callers check the return
		// value and use defaults, and a partial tree would silently override them.
		Clear();
		return false;
	}

	// KV1 files are "RootName" { ... }: Valve's loader merges that single top-level block into the
	// object it parses into, which is why KeyValues kv("Config"); kv.LoadFromFile(...);
	// kv.GetString("key") reads a key *inside* the block. Same here.
	if (m_children.size() == 1 && m_children.front()->HasChildren()) {
		std::unique_ptr<KeyValues> top = std::move(m_children.front());
		m_children.clear();
		m_name = top->m_name;
		if (top->m_has_value) {
			m_value = top->m_value;
			m_has_value = true;
		}
		m_children = std::move(top->m_children);
		for (auto& child : m_children) {
			child->m_parent = this;
		}
	}
	return true;
}

bool KeyValues::IsEmpty(std::string_view key) const {
	const KeyValues* found = key.empty() ? this : FindKey(std::string(key).c_str());
	if (found == nullptr) {
		return true;
	}
	return !found->HasValue() || found->m_value.empty();
}

KeyValues* KeyValues::FindKey(const char* key, bool create) {
	if (key == nullptr || *key == '\0') {
		return nullptr;
	}
	for (auto& child : m_children) {
		if (EqualNoCase(child->m_name, key)) {
			return child.get();
		}
	}
	if (create) {
		return AddSubKey(key);
	}
	return nullptr;
}

const KeyValues* KeyValues::FindKey(const char* key) const {
	return const_cast<KeyValues*>(this)->FindKey(key, false);
}

KeyValues* KeyValues::FindKeyWithValue(const char* key) {
	KeyValues* found = FindKey(key, false);
	return (found != nullptr && found->HasValue()) ? found : nullptr;
}

std::string_view KeyValues::GetStringValue(const char* key, std::string_view def) const {
	if (key == nullptr || *key == '\0') {
		return m_has_value ? std::string_view(m_value) : def;
	}
	const KeyValues* found = FindKey(key);
	if (found == nullptr || !found->HasValue()) {
		return def;
	}
	return std::string_view(found->m_value);
}

const char* KeyValues::GetString(const char* key, const char* def) const {
	if (key == nullptr || *key == '\0') {
		return m_has_value ? m_value.c_str() : def;
	}
	const KeyValues* found = FindKey(key);
	if (found == nullptr || !found->HasValue()) {
		return def;
	}
	return found->m_value.c_str();
}

int KeyValues::GetInt(const char* key, int def) const {
	const std::string_view text = Trim(GetStringValue(key, {}));
	if (text.empty()) {
		return def;
	}
	int value = 0;
	const auto* begin = text.data();
	const auto* end = text.data() + text.size();
	const auto result = std::from_chars(begin, end, value);
	if (result.ec != std::errc{}) {
		// Valve falls back to atoi semantics ("5 // comment", "1.5", "abc" -> 0/partial).
		return std::atoi(std::string(text).c_str());
	}
	return value;
}

float KeyValues::GetFloat(const char* key, float def) const {
	const std::string_view text = Trim(GetStringValue(key, {}));
	if (text.empty()) {
		return def;
	}
	const std::string copy(text);
	char* end = nullptr;
	const float value = std::strtof(copy.c_str(), &end);
	if (end == copy.c_str()) {
		return def;
	}
	return value;
}

bool KeyValues::GetBool(const char* key, bool def) const {
	const std::string_view text = Trim(GetStringValue(key, {}));
	if (text.empty()) {
		return def;
	}
	if (EqualNoCase(text, "true") || EqualNoCase(text, "yes") || EqualNoCase(text, "on")) {
		return true;
	}
	if (EqualNoCase(text, "false") || EqualNoCase(text, "no") || EqualNoCase(text, "off")) {
		return false;
	}
	return GetInt(key, def ? 1 : 0) != 0;
}

void KeyValues::SetString(std::string_view key, std::string_view value) {
	if (key.empty()) {
		m_value.assign(value);
		m_has_value = true;
		return;
	}
	AddSubKey(key)->SetString({}, value);
}

void KeyValues::SetInt(std::string_view key, int value) {
	SetString(key, std::to_string(value));
}

KeyValues* KeyValues::AddSubKey(std::string_view name) {
	auto child = std::make_unique<KeyValues>(name);
	child->m_parent = this;
	m_children.push_back(std::move(child));
	return m_children.back().get();
}

KeyValues* KeyValues::GetSubKey(std::size_t index) {
	return index < m_children.size() ? m_children[index].get() : nullptr;
}

const KeyValues* KeyValues::GetSubKey(std::size_t index) const {
	return index < m_children.size() ? m_children[index].get() : nullptr;
}

KeyValues* KeyValues::GetFirstSubKey() {
	return m_children.empty() ? nullptr : m_children.front().get();
}

const KeyValues* KeyValues::GetFirstSubKey() const {
	return m_children.empty() ? nullptr : m_children.front().get();
}

KeyValues* KeyValues::GetNextKey() {
	if (m_parent == nullptr) {
		return nullptr;
	}
	bool found = false;
	for (auto& sibling : m_parent->m_children) {
		if (found) {
			return sibling.get();
		}
		if (sibling.get() == this) {
			found = true;
		}
	}
	return nullptr;
}

const KeyValues* KeyValues::GetNextKey() const {
	return const_cast<KeyValues*>(this)->GetNextKey();
}

KeyValues* KeyValues::GetFirstTrueSubKey() {
	for (auto& child : m_children) {
		if (child->IsBlockNode()) {
			return child.get();
		}
	}
	return nullptr;
}

const KeyValues* KeyValues::GetFirstTrueSubKey() const {
	return const_cast<KeyValues*>(this)->GetFirstTrueSubKey();
}

KeyValues* KeyValues::GetNextTrueSubKey() {
	if (m_parent == nullptr) {
		return nullptr;
	}
	bool found = false;
	for (auto& sibling : m_parent->m_children) {
		if (found && sibling->IsBlockNode()) {
			return sibling.get();
		}
		if (sibling.get() == this) {
			found = true;
		}
	}
	return nullptr;
}

const KeyValues* KeyValues::GetNextTrueSubKey() const {
	return const_cast<KeyValues*>(this)->GetNextTrueSubKey();
}

KeyValues* KeyValues::GetFirstValue() {
	for (auto& child : m_children) {
		if (child->IsValueNode()) {
			return child.get();
		}
	}
	return nullptr;
}

const KeyValues* KeyValues::GetFirstValue() const {
	return const_cast<KeyValues*>(this)->GetFirstValue();
}

KeyValues* KeyValues::GetNextValue() {
	if (m_parent == nullptr) {
		return nullptr;
	}
	bool found = false;
	for (auto& sibling : m_parent->m_children) {
		if (found && sibling->IsValueNode()) {
			return sibling.get();
		}
		if (sibling.get() == this) {
			found = true;
		}
	}
	return nullptr;
}

const KeyValues* KeyValues::GetNextValue() const {
	return const_cast<KeyValues*>(this)->GetNextValue();
}

std::vector<KeyValues*> KeyValues::GetSubKeyList() {
	std::vector<KeyValues*> out;
	out.reserve(m_children.size());
	for (auto& child : m_children) {
		out.push_back(child.get());
	}
	return out;
}

void KeyValues::Clear() {
	m_children.clear();
	m_value.clear();
	m_has_value = false;
}

} // namespace levels_ranks
