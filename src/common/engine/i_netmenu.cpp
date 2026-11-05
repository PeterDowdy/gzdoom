//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//		Starting a network game from the main menu.
//
//		A network game is only ever set up at start-up (-host / -join, see i_net.cpp), so hosting or joining from
//		the menu closes the game cleanly and starts it again, in the same process, with those arguments added and
//		whatever it was started with kept (the same IWAD, the same -file list).
//
//-----------------------------------------------------------------------------

#include <functional>
#include <string>
#include <vector>
#include <set>
#include <errno.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <limits.h>
#include <spawn.h>
#include <mach-o/dyld.h>
extern char** environ;
#endif
#endif

#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "engineerrors.h"
#include "i_net.h"
#include "gamestate.h"
#include "m_argv.h"
#include "menu.h"
#include "printf.h"
#include "vm.h"

extern FString lastIWAD;

// The yes/no box the engine's own "end game" and "quit" use; the handler runs on yes.
typedef void(*hfunc)();
DMenu* CreateMessageBoxMenu(DMenu* parent, const char* message, int messagemode, bool playsound, FName action = NAME_None, hfunc handler = nullptr);
void M_ActivateMenu(DMenu* menu);
extern DMenu* CurrentMenu;

// What the menu's forms edit. The password is deliberately not saved.
CVAR(Int, mp_players, 2, CVAR_ARCHIVE)
CVAR(Int, mp_mode, 0, CVAR_ARCHIVE)			// 0 co-operative, 1 deathmatch, 2 alternate deathmatch
CVAR(Int, mp_skill, 3, CVAR_ARCHIVE)		// as -skill takes it: 1 is the easiest
CVAR(String, mp_map, "1", CVAR_ARCHIVE)		// as -warp takes it; empty: the host picks "New game" itself
CVAR(Int, mp_port, 5029, CVAR_ARCHIVE)
CVAR(String, mp_address, "", CVAR_ARCHIVE)	// the last address joined by hand: "host" or "host:port"
CVAR(String, mp_password, "", 0)

static std::vector<std::string> RelaunchArgv;

// The arguments this game was started with, without the network and game-setup switches a host or guest sets itself.
static std::vector<std::string> ArgumentsToKeep()
{
	// Switches that stand alone, and switches that take values (every argument after them that isn't a switch).
	static const std::set<std::string> flags = { "-deathmatch", "-altdeath", "-coop", "-netmenu" };
	static const std::set<std::string> valued = { "-host", "-join", "-port", "-password", "-netmode", "-warp", "-skill", "-loadgame", "+map", "+warp" };

	std::vector<std::string> kept;
	const int count = Args->NumArgs();
	for (int i = 0; i < count; ++i)
	{
		const std::string arg = Args->GetArg(i);
		if (i > 0 && flags.count(arg))
			continue;

		if (i > 0 && valued.count(arg))
		{
			while (i + 1 < count && Args->GetArg(i + 1)[0] != '-' && Args->GetArg(i + 1)[0] != '+')
				++i;
			continue;
		}

		kept.push_back(arg);
	}

	// A game started through the IWAD picker has no -iwad of its own to pass on.
	bool hasIWAD = false;
	for (auto& arg : kept)
		hasIWAD |= (arg == "-iwad");
	if (!hasIWAD && lastIWAD.IsNotEmpty())
	{
		kept.push_back("-iwad");
		kept.push_back(lastIWAD.GetChars());
	}

	return kept;
}

// Starting a network game starts the program again, which ends any game being played. In a level, ask first, and carry
// on with `proceed` if the player says yes. Returns true if it's fine to go ahead right now.
static std::function<void()> AfterConfirmation;

static bool CanStartNetGame(std::function<void()> proceed)
{
	if (netgame || multiplayer)
	{
		Printf(TEXTCOLOR_RED "Already in a network game.\n");
		return false;
	}
	if (gamestate == GS_LEVEL)
	{
		AfterConfirmation = std::move(proceed);
		DMenu* box = CreateMessageBoxMenu(CurrentMenu,
			"Hosting or joining a network game ends the game you're playing, and anything you haven't saved is lost.\n\nAre you sure?\n\nPress Y or N.",
			0, false, NAME_None, []()
			{
				auto go = std::move(AfterConfirmation);
				AfterConfirmation = nullptr;
				if (go)
					go();
			});
		M_ActivateMenu(box);
		return false;
	}
	return true;
}

// Quit, and let main() start the game again with these arguments once everything has been shut down and saved.
[[noreturn]] static void Relaunch(std::vector<std::string> argv)
{
	RelaunchArgv = std::move(argv);
	throw CExitEvent(0);
}

static bool IsNumber(const char* text)
{
	if (!*text)
		return false;
	for (; *text; ++text)
	{
		if (*text < '0' || *text > '9')
			return false;
	}
	return true;
}

[[noreturn]] static void StartHosting(int players)
{
	players = clamp(players, 2, 8);

	auto args = ArgumentsToKeep();
	args.push_back("-netmenu");
	args.push_back("-host");
	args.push_back(std::to_string(players));
	if (*mp_port > 0 && *mp_port < 65536 && *mp_port != 5029)
	{
		args.push_back("-port");
		args.push_back(std::to_string(*mp_port));
	}
	if (strlen(mp_password) > 0)
	{
		args.push_back("-password");
		args.push_back(*mp_password);
	}
	if (*mp_mode == 1)
		args.push_back("-deathmatch");
	else if (*mp_mode == 2)
		args.push_back("-altdeath");
	args.push_back("-skill");
	args.push_back(std::to_string(clamp(*mp_skill, 1, 5)));
	if (IsNumber(mp_map))
	{
		args.push_back("-warp");
		args.push_back(*mp_map);
	}

	Relaunch(std::move(args));
}

[[noreturn]] static void StartJoining(const FString& address)
{
	auto args = ArgumentsToKeep();
	args.push_back("-netmenu");
	args.push_back("-join");
	args.push_back(address.GetChars());
	if (strlen(mp_password) > 0)
	{
		args.push_back("-password");
		args.push_back(*mp_password);
	}

	Relaunch(std::move(args));
}

// mphost [players]: host a game with the menu's settings and wait for the other players.
CCMD(mphost)
{
	const int players = argv.argc() > 1 ? atoi(argv[1]) : *mp_players;
	if (CanStartNetGame([players]() { StartHosting(players); }))
		StartHosting(players);
}

// mpjoin [address]: join a game, the address being "host" or "host:port".
CCMD(mpjoin)
{
	FString address = argv.argc() > 1 ? FString(argv[1]) : FString(*mp_address);
	address.StripLeftRight();
	if (address.IsEmpty())
	{
		Printf(TEXTCOLOR_RED "No address to join.\n");
		return;
	}
	if (argv.argc() <= 1)
		mp_address = address.GetChars();

	if (CanStartNetGame([address]() { StartJoining(address); }))
		StartJoining(address);
}

void I_NetMenuReturnToMenu()
{
	// Only for games the menu started: -host and -join on the command line mean what they say.
	if (Args != nullptr && Args->CheckParm("-netmenu") && I_NetStartInterrupted() && RelaunchArgv.empty())
		RelaunchArgv = ArgumentsToKeep();
}

#ifdef _WIN32
// One argument as CommandLineToArgvW (and the C runtime) will read it back.
static std::wstring QuoteArgument(const std::wstring& arg)
{
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
		return arg;

	std::wstring out = L"\"";
	for (auto it = arg.begin();; ++it)
	{
		size_t backslashes = 0;
		for (; it != arg.end() && *it == L'\\'; ++it)
			++backslashes;

		if (it == arg.end())
		{
			out.append(backslashes * 2, L'\\');	// they come right before the closing quote
			break;
		}
		else if (*it == L'"')
		{
			out.append(backslashes * 2 + 1, L'\\');
			out.push_back(L'"');
		}
		else
		{
			out.append(backslashes, L'\\');
			out.push_back(*it);
		}
	}
	out.push_back(L'"');
	return out;
}
#endif

// Called by the platform's main() after the game has shut down. Only returns if no restart was asked for, or it failed.
// Linux execs, so it's the same process (the studio's Play keeps tracking it). Windows and macOS start the program
// again and let this one end, as the existing "restart" on Windows does.
void I_RelaunchIfRequested()
{
	if (RelaunchArgv.empty())
		return;

#ifdef _WIN32
	WCHAR path[MAX_PATH];
	if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
		return;

	std::wstring parameters;
	for (size_t i = 1; i < RelaunchArgv.size(); ++i)
	{
		if (i > 1)
			parameters += L' ';
		parameters += QuoteArgument(FString(RelaunchArgv[i].c_str()).WideString());
	}
	ShellExecuteW(nullptr, L"open", path, parameters.c_str(), nullptr, SW_SHOWNORMAL);
#else
	std::vector<char*> argv;
	for (auto& arg : RelaunchArgv)
		argv.push_back(const_cast<char*>(arg.c_str()));
	argv.push_back(nullptr);

#ifdef __APPLE__
	char exe[PATH_MAX];
	uint32_t size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &size) == 0)
	{
		argv[0] = exe;
		pid_t child;
		if (posix_spawn(&child, exe, nullptr, nullptr, argv.data(), environ) == 0)
			return;
	}
#else
	execv("/proc/self/exe", argv.data());	// the same binary, whatever it was started as
#endif
	execvp(argv[0], argv.data());
	fprintf(stderr, "Couldn't start the network game: %s\n", strerror(errno));
#endif
}

// The lobbies the menu's LAN browser last found.
static TArray<FLanGame> LanFound;

DEFINE_ACTION_FUNCTION(_LanGames, Scan)
{
	PARAM_PROLOGUE;
	PARAM_INT(port);
	PARAM_INT(waitMs);
	I_NetScanLAN(port, waitMs, LanFound);
	return 0;
}

DEFINE_ACTION_FUNCTION(_LanGames, Count)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(LanFound.Size());
}

DEFINE_ACTION_FUNCTION(_LanGames, Address)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	ACTION_RETURN_STRING(index >= 0 && (unsigned)index < LanFound.Size() ? LanFound[index].Address : FString());
}

DEFINE_ACTION_FUNCTION(_LanGames, Description)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	if (index < 0 || (unsigned)index >= LanFound.Size())
		ACTION_RETURN_STRING(FString());

	const FLanGame& game = LanFound[index];
	FString text;
	text.Format("%s  %d/%d", game.HostName.GetChars(), game.Players, game.MaxPlayers);
	if (game.Password)
		text << "  (password)";
	if (!game.Compatible)
		text << "  (other version)";
	else if (game.Players >= game.MaxPlayers)
		text << "  (full)";
	ACTION_RETURN_STRING(text);
}

DEFINE_ACTION_FUNCTION(_LanGames, Joinable)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	ACTION_RETURN_BOOL(index >= 0 && (unsigned)index < LanFound.Size() && LanFound[index].Compatible
		&& LanFound[index].Players < LanFound[index].MaxPlayers);
}
