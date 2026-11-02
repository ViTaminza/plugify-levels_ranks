#include "lr_commands.hpp"

#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lr_phrases.hpp"

namespace levels_ranks {
namespace {

struct RegisteredName {
	std::string name;
	bool owned = false;    // AddConsoleCommand created it: RemoveCommand is the matching detach
	bool registered = false;
};

std::vector<CommandSpec> g_specs;
std::vector<RegisteredName> g_names;
bool g_say_registered = false;
bool g_say_team_registered = false;
void* g_user = nullptr;

std::string ToLower(std::string_view text) {
	std::string out(text);
	std::transform(out.begin(), out.end(), out.begin(), [](char c) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	});
	return out;
}

bool IsChatTrigger(char c) {
	return c == '!' || c == '/';
}

// s2sdk strips the trigger before dispatching a chat command, so "!lvl" is registered as "lvl"
// (that is also what pisexcompat did). Console names are registered unchanged.
std::string RegistrationName(const CommandSpec& spec) {
	if (!spec.chat || spec.name == nullptr || spec.name[0] == '\0') {
		return spec.name != nullptr ? std::string(spec.name) : std::string();
	}
	const std::string_view name(spec.name);
	return IsChatTrigger(name.front()) ? std::string(name.substr(1)) : std::string(name);
}

const CommandSpec* FindSpec(std::string_view name, bool from_chat) {
	const std::string lowered = ToLower(name);
	for (auto it = g_specs.rbegin(); it != g_specs.rend(); ++it) {
		if (it->name == nullptr || it->chat != from_chat) {
			continue;
		}
		if (ToLower(RegistrationName(*it)) == lowered) {
			return &(*it);
		}
	}
	return nullptr;
}

const CommandSpec* FindBareChatSpec(std::string_view word) {
	const std::string lowered = ToLower(word);
	for (auto it = g_specs.rbegin(); it != g_specs.rend(); ++it) {
		if (it->name == nullptr || !it->chat || it->name[0] == '\0' || IsChatTrigger(it->name[0])) {
			continue;
		}
		if (ToLower(it->name) == lowered) {
			return &(*it);
		}
	}
	return nullptr;
}

CommandArgs BuildArgs(const CommandSpec& spec, const plg::vector<plg::string>& args) {
	CommandArgs parsed;
	std::string content(spec.name != nullptr ? spec.name : "");
	for (std::size_t i = 1; i < args.size(); ++i) {
		if (!content.empty()) {
			content.push_back(' ');
		}
		content.append(args[i].data(), args[i].size());
	}
	parsed.Tokenize(content);
	return parsed;
}

CommandArgs BuildArgs(const CommandSpec& spec, const std::vector<std::string>& tokens) {
	CommandArgs parsed;
	std::string content(spec.name != nullptr ? spec.name : "");
	for (std::size_t i = 1; i < tokens.size(); ++i) {
		content.push_back(' ');
		content += tokens[i];
	}
	parsed.Tokenize(content);
	return parsed;
}

void RunSpec(const CommandSpec& spec, int32_t caller, bool from_chat, const CommandArgs& args) {
	if (spec.handler == nullptr) {
		return;
	}
	spec.handler(g_user, caller, from_chat, args);
}

s2sdk::ResultType CommandTrampoline(int32_t caller, s2sdk::ConCommandContext context,
									const plg::vector<plg::string>& args) {
	if (args.empty() || args[0].empty()) {
		return s2sdk::ResultType::Continue;
	}
	const bool from_chat = context == s2sdk::ConCommandContext::Chat;
	try {
		const std::string_view name(args[0].data(), args[0].size());
		const CommandSpec* spec = FindSpec(name, from_chat);
		if (spec == nullptr) {
			// A chat message that does not belong to any registered chat name is left alone.
			return s2sdk::ResultType::Continue;
		}
		if (spec->server_only && caller != -1) {
			// The server console is exactly caller -1; -2 or any client slot never bypasses it.
			return s2sdk::ResultType::Handled;
		}
		const CommandArgs parsed = BuildArgs(*spec, args);
		RunSpec(*spec, caller, from_chat, parsed);
		// Pisex swallowed every chat line whose first word matched a registered chat name; a
		// console command keeps its old "let the engine continue" behaviour.
		return from_chat ? s2sdk::ResultType::Handled : s2sdk::ResultType::Continue;
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: command callback threw: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: command callback threw an unknown exception");
	}
	return from_chat ? s2sdk::ResultType::Handled : s2sdk::ResultType::Continue;
}

// s2sdk delivers every "say"/"say_team" line here as (say, first-token, rest...) - or as one
// quoted message. A first token that is a registered trigger-less chat name is dispatched like a
// chat command and removed from the chat; lines starting with a trigger belong to s2sdk's own
// chat-command path (no double dispatch).
s2sdk::ResultType BareChatTrampoline(int32_t caller, s2sdk::ConCommandContext /*context*/,
									 const plg::vector<plg::string>& args) {
	try {
		if (args.size() < 2) {
			return s2sdk::ResultType::Continue;
		}
		std::vector<std::string> tokens;
		if (args.size() == 2) {
			CommandArgs message(std::string_view(args[1].data(), args[1].size()));
			for (int i = 0; i < message.ArgC(); ++i) {
				tokens.emplace_back(message.Arg(i));
			}
		} else {
			tokens.emplace_back(args[1].data(), args[1].size());
			for (std::size_t i = 2; i < args.size(); ++i) {
				tokens.emplace_back(args[i].data(), args[i].size());
			}
		}
		if (tokens.empty() || tokens[0].empty() || IsChatTrigger(tokens[0][0])) {
			return s2sdk::ResultType::Continue;
		}
		const CommandSpec* spec = FindBareChatSpec(tokens[0]);
		if (spec == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		if (spec->server_only && caller != -1) {
			return s2sdk::ResultType::Handled;
		}
		const CommandArgs parsed = BuildArgs(*spec, tokens);
		RunSpec(*spec, caller, /*from_chat=*/true, parsed);
		return s2sdk::ResultType::Handled;
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: bare chat command threw: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: bare chat command threw an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

void RegisterName(const std::string& name, bool server_only) {
	for (const RegisteredName& entry : g_names) {
		if (entry.name == name) {
			return; // one s2sdk registration per name
		}
	}
	int64_t flags = static_cast<int64_t>(s2sdk::ConVarFlag::Release);
	if (!server_only) {
		flags |= static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute);
	}
	RegisteredName entry;
	entry.name = name;
	try {
		entry.owned = s2sdk::AddConsoleCommand(plg::string(name), plg::string("levels_ranks"),
											   static_cast<s2sdk::ConVarFlag>(flags),
											   &CommandTrampoline, s2sdk::HookMode::Pre);
		entry.registered = true;
	} catch (...) {
		LogError("levels_ranks: could not register the command '" + name + "'");
		entry.registered = false;
	}
	g_names.push_back(std::move(entry));
}

} // namespace

void CommandArgs::Tokenize(std::string_view text) {
	m_args.clear();
	std::size_t i = 0;
	while (i < text.size()) {
		while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
			++i;
		}
		if (i >= text.size()) {
			break;
		}
		std::string token;
		if (text[i] == '"') {
			++i;
			while (i < text.size() && text[i] != '"') {
				token.push_back(text[i++]);
			}
			if (i < text.size()) {
				++i; // closing quote
			}
		} else {
			while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
				token.push_back(text[i++]);
			}
		}
		m_args.push_back(std::move(token));
	}
}

const char* CommandArgs::Arg(int index) const {
	if (index < 0 || static_cast<std::size_t>(index) >= m_args.size()) {
		return nullptr;
	}
	return m_args[static_cast<std::size_t>(index)].c_str();
}

const std::string& CommandArgs::ArgString(int index) const {
	static const std::string empty;
	if (index < 0 || static_cast<std::size_t>(index) >= m_args.size()) {
		return empty;
	}
	return m_args[static_cast<std::size_t>(index)];
}

bool CommandBindingsAvailable() {
	return __s2sdk_AddConsoleCommand != nullptr && __s2sdk_RemoveCommand != nullptr &&
		   __s2sdk_RemoveCommandListener != nullptr;
}

bool CommandListenerBindingsAvailable() {
	return __s2sdk_AddCommandListener != nullptr && __s2sdk_RemoveCommandListener != nullptr;
}

void RegisterCommands(const CommandSpec* specs, std::size_t count, void* user) {
	if (!g_names.empty()) {
		return; // already registered; OnPluginEnd must remove them first
	}
	g_specs.assign(specs, specs + count);
	g_user = user;
	if (!CommandBindingsAvailable()) {
		LogError("levels_ranks: the s2sdk command bindings are incomplete (add/remove/remove-listener) "
				 "- the commands are disabled");
		g_specs.clear();
		return;
	}

	// One s2sdk registration per name; the context (console/chat) is chosen by the callback.
	// A chat name is registered with its trigger stripped ("!lvl" -> "lvl").
	for (const CommandSpec& spec : g_specs) {
		if (spec.name == nullptr || *spec.name == '\0') {
			continue;
		}
		bool has_console = false;
		bool all_server_only = true;
		for (const CommandSpec& other : g_specs) {
			if (other.name == nullptr || other.chat || ToLower(other.name) != ToLower(spec.name)) {
				continue;
			}
			has_console = true;
			if (!other.server_only) {
				all_server_only = false;
			}
		}
		RegisterName(RegistrationName(spec), has_console && all_server_only);
	}

	// Trigger-less chat names need the Pisex say hook; a name starting with a trigger is reached
	// through s2sdk's own chat-command path.
	bool need_say = false;
	for (const CommandSpec& spec : g_specs) {
		if (spec.chat && spec.name != nullptr && spec.name[0] != '\0' && !IsChatTrigger(spec.name[0])) {
			need_say = true;
			break;
		}
	}
	if (!need_say) {
		return;
	}
	if (__s2sdk_AddCommandListener == nullptr || __s2sdk_RemoveCommandListener == nullptr) {
		LogError("levels_ranks: the s2sdk command-listener bindings are incomplete - the "
				 "trigger-less chat commands are disabled");
		return;
	}
	g_say_registered = false;
	try {
		g_say_registered =
			s2sdk::AddCommandListener(plg::string("say"), &BareChatTrampoline, s2sdk::HookMode::Pre);
	} catch (...) {
		g_say_registered = false;
	}
	g_say_team_registered = false;
	try {
		g_say_team_registered = s2sdk::AddCommandListener(plg::string("say_team"), &BareChatTrampoline,
														 s2sdk::HookMode::Pre);
	} catch (...) {
		g_say_team_registered = false;
	}
	if (!g_say_registered || !g_say_team_registered) {
		if (g_say_registered && __s2sdk_RemoveCommandListener != nullptr) {
			try {
				s2sdk::RemoveCommandListener(plg::string("say"), &BareChatTrampoline,
											 s2sdk::HookMode::Pre);
			} catch (...) {
			}
			g_say_registered = false;
		}
		if (g_say_team_registered && __s2sdk_RemoveCommandListener != nullptr) {
			try {
				s2sdk::RemoveCommandListener(plg::string("say_team"), &BareChatTrampoline,
											 s2sdk::HookMode::Pre);
			} catch (...) {
			}
			g_say_team_registered = false;
		}
		LogError("levels_ranks: could not hook say/say_team - the trigger-less chat commands are "
				 "disabled");
	}
}

void UnregisterCommands() {
	for (const RegisteredName& entry : g_names) {
		if (!entry.registered) {
			continue;
		}
		try {
			if (entry.owned) {
				s2sdk::RemoveCommand(plg::string(entry.name), &CommandTrampoline);
			} else if (__s2sdk_RemoveCommandListener != nullptr) {
				s2sdk::RemoveCommandListener(plg::string(entry.name), &CommandTrampoline,
											 s2sdk::HookMode::Pre);
			}
		} catch (...) {
		}
	}
	g_names.clear();
	if (__s2sdk_RemoveCommandListener != nullptr) {
		if (g_say_registered) {
			try {
				s2sdk::RemoveCommandListener(plg::string("say"), &BareChatTrampoline,
											 s2sdk::HookMode::Pre);
			} catch (...) {
			}
		}
		if (g_say_team_registered) {
			try {
				s2sdk::RemoveCommandListener(plg::string("say_team"), &BareChatTrampoline,
											 s2sdk::HookMode::Pre);
			} catch (...) {
			}
		}
	}
	g_say_registered = false;
	g_say_team_registered = false;
	g_specs.clear();
	g_user = nullptr;
}

} // namespace levels_ranks
