#pragma once
// levels_ranks — the console/chat command layer of the ported plugin.
//
// The original registered console names (mm_lvl, sm_lvl, ...) and chat names (!lvl, lvl, ...)
// separately: a console name only answers console input, a chat name only chat input, and a
// trigger-less chat spelling (the Pisex "say" hook) is matched against the first word of every
// chat message. This file keeps that contract on the native s2sdk API:
//
//   * console names are this plugin's own ConCommands with Release (and ClientCanExecute unless
//     the command is server-only). AddConsoleCommand may return false when the name already
//     exists: then only its listener belongs to this plugin, and OnPluginEnd detaches it with
//     RemoveCommandListener instead of RemoveCommand,
//   * a chat name is registered with its trigger stripped ("!lvl" -> "lvl"), because s2sdk strips
//     the configured trigger and dispatches the line as a command with ConCommandContext::Chat;
//     the callback answers only the Chat context, so a client console "lvl" falls through,
//   * trigger-less chat names ("lvl", "rank", the Russian spellings) are additionally matched by
//     a say/say_team Pre listener: s2sdk only chains a message starting with a trigger,
//   * server-only commands require caller == -1. -1 is the server console; every other caller
//     (including -2) is rejected and swallowed before the handler runs,
//   * all four command bindings (AddConsoleCommand/RemoveCommand/RemoveCommandListener and
//     AddCommandListener/RemoveCommandListener) are checked before anything is registered, and
//     OnPluginEnd removes exactly what was registered, so `plg reload` is safe (PITFALLS A7/C11).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace levels_ranks {

// The original's CCommand subset: whitespace separated tokens, double quotes group a token and are
// removed. Arg(0) is the registered command name (the spelling the original saw).
class CommandArgs {
public:
	CommandArgs() = default;
	explicit CommandArgs(std::string_view text) { Tokenize(text); }

	void Tokenize(std::string_view text);
	int ArgC() const { return static_cast<int>(m_args.size()); }
	const char* Arg(int index) const;
	const char* operator[](int index) const { return Arg(index); }
	const std::string& ArgString(int index) const;

private:
	std::vector<std::string> m_args;
};

// One registered spelling. `chat` selects the context; `server_only` rejects every caller except
// -1. A name may appear twice (console and chat), each with its own flag.
struct CommandSpec {
	const char* name = nullptr;
	bool chat = false;
	bool server_only = false;
	void (*handler)(void* user, int32_t caller, bool from_chat, const CommandArgs& args) = nullptr;
};

// True when the console-command pair (AddConsoleCommand + the two removals) is present.
bool CommandBindingsAvailable();
// True when the say/say_team listener pair (AddCommandListener + RemoveCommandListener) is present.
bool CommandListenerBindingsAvailable();

// Registers the specs. Safe to call once per plugin instance; a missing binding pair skips the
// affected part and logs. `user` is handed back to every handler.
void RegisterCommands(const CommandSpec* specs, std::size_t count, void* user);

// Removes every command/listener this plugin registered (idempotent).
void UnregisterCommands();

} // namespace levels_ranks
