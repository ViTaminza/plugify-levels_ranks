#pragma once
// The pisex::KeyValues view of lr_core's ConfigView interface: the bridge between the KV1 parser
// of libs/pisexcompat and the engine-free settings model (Settings LoadSettings()).
//
// Header-only so that both plugin.cpp (the real settings.ini) and the tests (the shipped
// settings.ini, plus literal trees) drive the core through the same code path. It needs
// pisexcompat for the parser but no engine: the test binary links pisexcompat + s2sdk_bindings
// like tests/pisexcompat_tests.cpp does.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pisex/keyvalues.hpp>

#include "lr_core.hpp"

namespace levels_ranks {

class KeyValuesView final : public ConfigView {
public:
	explicit KeyValuesView(pisex::KeyValues& root) : m_root(root) {}

	bool HasSection(std::string_view section) const override {
		if (section.empty()) {
			return true;
		}
		return m_root.FindKey(std::string(section).c_str(), false) != nullptr;
	}

	std::string GetString(std::string_view section, std::string_view key, std::string_view def) const override {
		const pisex::KeyValues* node = Node(section);
		if (node == nullptr) {
			return std::string(def);
		}
		const std::string key_text(key);
		const std::string def_text(def);
		return std::string(node->GetString(key_text.c_str(), def_text.c_str()));
	}

	float GetFloat(std::string_view section, std::string_view key, float def) const override {
		const pisex::KeyValues* node = Node(section);
		return node == nullptr ? def : node->GetFloat(std::string(key).c_str(), def);
	}

	int GetInt(std::string_view section, std::string_view key, int def) const override {
		const pisex::KeyValues* node = Node(section);
		return node == nullptr ? def : node->GetInt(std::string(key).c_str(), def);
	}

	std::vector<std::pair<std::string, int>> GetIntValues(std::string_view section) const override {
		std::vector<std::pair<std::string, int>> values;
		pisex::KeyValues* node = Node(section);
		if (node == nullptr) {
			return values;
		}
		FOR_EACH_VALUE(node, entry) {
			values.emplace_back(entry->GetName(), entry->GetInt(nullptr, 0));
		}
		return values;
	}

private:
	pisex::KeyValues* Node(std::string_view section) const {
		if (section.empty()) {
			return &m_root; // "" is the root node, exactly like pKVConfig of the original
		}
		return m_root.FindKey(std::string(section).c_str(), false);
	}

	pisex::KeyValues& m_root;
};

} // namespace levels_ranks
