/* Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 * GPL-3.0-or-later, like PCSX2 / XBSX2.
 *
 * Settings: the console deletes an app's storage when the app is removed (a player's update through the Device
 * Portal was queued, failed and took the app with it: settings and controller setup gone), so a copy is kept in the
 * publisher's shared folder (games\xbsx2\PCSX2.ini: outlives a reinstall) and on the games drive
 * (<home>\settings\XBSX2 PCSX2.ini: outlives a reset console), and put back by itself when XBSX2 starts with no
 * settings of its own.
 *
 * Game Vault keeps BIOS files, memory cards and states on the games USB drive, in "<drive>\Game Vault", and sends
 * that folder as "home" with every game. The first time, what XBSX2 had in its own storage is moved there (a file
 * already on the drive is left alone). A game started without "home" uses XBSX2's own folders again, so a stick that
 * is no longer plugged in is never looked for.
 */

#include "pcsx2/PrecompiledHeader.h"

#include "GameVaultHost.h"

#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <winrt/Windows.Storage.h>

#include <vector>

static bool s_pad_auto_mapped = false; // controller 1 got the Xbox layout at this start
static std::vector<std::string> s_moved_to_home; // files moved from XBSX2's own storage at this start
static bool s_settings_were_fresh = false;

static const char* const s_home_folders[][2] = {
	{"Bios", "bios"}, {"MemoryCards", "memory cards"}, {"Savestates", "states"}};
static const char* const s_own_folders[] = {"bios", "memcards", "sstates"};

static INISettingsInterface* BaseSettings()
{
	return static_cast<INISettingsInterface*>(Host::Internal::GetBaseSettingsLayer());
}

static std::string SettingsBackup()
{
	try
	{
		const std::string dir = winrt::to_string(winrt::Windows::Storage::ApplicationData::Current()
				.GetPublisherCacheFolder(L"games").Path()) + "\\xbsx2";
		FileSystem::CreateDirectoryPath(dir.c_str(), true);
		return Path::Combine(dir, "PCSX2.ini");
	}
	catch (...)
	{
		return {};
	}
}

static std::string HomeSettings(const std::string& home)
{
	const std::string dir = Path::Combine(home, "settings");
	FileSystem::CreateDirectoryPath(dir.c_str(), true);
	return Path::Combine(dir, "XBSX2 PCSX2.ini");
}

bool GameVaultHost::RestoreSettings(const std::string& path)
{
	if (FileSystem::FileExists(path.c_str()))
		return true;
	// a reinstall: the copy in the shared folder, if there is one
	const std::string backup = SettingsBackup();
	if (!backup.empty() && FileSystem::FileExists(backup.c_str()) && FileSystem::CopyFilePath(backup.c_str(), path.c_str(), true))
	{
		Console.WriteLn("Game Vault: settings put back from %s", backup.c_str());
		return true;
	}
	s_settings_were_fresh = true;
	return false;
}

void GameVaultHost::AutoMapController(INISettingsInterface& si)
{
	// Controller 1 with no buttons at all (the defaults are made before any controller is seen, so a fresh or reset
	// settings file has none) gets the Xbox controller's standard layout. A player's own mapping, even a partial one,
	// is left alone. Games saw no input at all: "Controller 1 has no input bindings configured".
	// The first pad is always SDL-0 now (SDLInputSource numbers pads in order on the Xbox): controller 1 mapped to
	// another pad number, from a console that called the pad player 1, follows it to SDL-0, buttons unchanged.
	const std::string cross = si.GetStringValue("Pad1", "Cross");
	if (cross.starts_with("SDL-") && !cross.starts_with("SDL-0/"))
	{
		const std::string from = cross.substr(0, cross.find('/') + 1);
		auto items = si.GetKeyValueList("Pad1");
		for (auto& [key, value] : items)
			for (size_t at; (at = value.find(from)) != std::string::npos;)
				value.replace(at, from.size(), "SDL-0/");
		si.SetKeyValueList("Pad1", items);
		auto lock = Host::GetSettingsLock();
		Error error;
		if (si.Save(&error))
			Console.WriteLn("Game Vault: controller 1 moved from %s to SDL-0/", from.c_str());
		return;
	}
	if (si.GetStringValue("Pad1", "Type", "DualShock2") != "DualShock2" || !cross.empty() ||
		!si.GetStringValue("Pad1", "Start").empty() || !si.GetStringValue("Pad1", "Up").empty())
		return;

	static constexpr const char* layout[][2] = {
		{"Up", "SDL-0/DPadUp"}, {"Right", "SDL-0/DPadRight"}, {"Down", "SDL-0/DPadDown"}, {"Left", "SDL-0/DPadLeft"},
		{"Triangle", "SDL-0/FaceNorth"}, {"Circle", "SDL-0/FaceEast"}, {"Cross", "SDL-0/FaceSouth"},
		{"Square", "SDL-0/FaceWest"}, {"Select", "SDL-0/Back"}, {"Start", "SDL-0/Start"},
		{"L1", "SDL-0/LeftShoulder"}, {"L2", "SDL-0/+LeftTrigger"}, {"R1", "SDL-0/RightShoulder"},
		{"R2", "SDL-0/+RightTrigger"}, {"L3", "SDL-0/LeftStick"}, {"R3", "SDL-0/RightStick"},
		{"LUp", "SDL-0/-LeftY"}, {"LRight", "SDL-0/+LeftX"}, {"LDown", "SDL-0/+LeftY"}, {"LLeft", "SDL-0/-LeftX"},
		{"RUp", "SDL-0/-RightY"}, {"RRight", "SDL-0/+RightX"}, {"RDown", "SDL-0/+RightY"}, {"RLeft", "SDL-0/-RightX"},
		{"Analog", "SDL-0/Guide"}, {"LargeMotor", "SDL-0/LargeMotor"}, {"SmallMotor", "SDL-0/SmallMotor"}};
	si.SetStringValue("Pad1", "Type", "DualShock2");
	for (const auto& b : layout)
		si.SetStringValue("Pad1", b[0], b[1]);
	si.SetBoolValue("InputSources", "SDL", true);
	auto lock = Host::GetSettingsLock();
	s_pad_auto_mapped = true;
	Error error;
	if (si.Save(&error))
		Console.WriteLn("Game Vault: controller 1 had no buttons, Xbox controller layout set");
	else
		Console.ErrorFmt("Game Vault: controller layout not saved: {}", error.GetDescription());
}

static void MoveFolderToHome(const std::string& from, const std::string& to)
{
	FileSystem::FindResultsArray files;
	if (!FileSystem::FindFiles(from.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS, &files))
		return;
	for (const FILESYSTEM_FIND_DATA& fd : files)
	{
		const std::string src = Path::Combine(from, fd.FileName), dst = Path::Combine(to, fd.FileName);
		if (FileSystem::FileExists(dst.c_str()))
			continue;
		FileSystem::CreateDirectoryPath(std::string(Path::GetDirectory(dst)).c_str(), true);
		if (FileSystem::CopyFilePath(src.c_str(), dst.c_str(), false))
		{
			FileSystem::DeleteFilePath(src.c_str());
			Console.WriteLn("Game Vault: moved %s to %s", src.c_str(), dst.c_str());
			s_moved_to_home.push_back(dst);
		}
		else
		{
			Console.Error("Game Vault: could not move %s to %s", src.c_str(), dst.c_str());
		}
	}
}

// for reports (written once the log on the games drive is open): how controller 1 is set up at this start
static void LogControllerSetup(INISettingsInterface* si)
{
	if (!si)
		return;
	if (s_pad_auto_mapped)
		Console.WriteLn("Game Vault: controller 1 had no buttons, Xbox controller layout set");
	for (const std::string& f : s_moved_to_home) // a BIOS among them is used from now on, before any free one
		Console.WriteLn("Game Vault: moved from XBSX2's own storage onto the drive: %s", f.c_str());
	s_moved_to_home.clear();
	int mapped = 0;
	for (const char* b : {"Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle", "Start", "Select",
			 "L1", "L2", "R1", "R2", "L3", "R3", "LUp", "LDown", "LLeft", "LRight"})
		mapped += !si->GetStringValue("Pad1", b).empty();
	Console.WriteLn("Game Vault: controller 1 is %s with %d of 20 buttons and directions mapped (Cross = %s)",
		si->GetStringValue("Pad1", "Type", "DualShock2").c_str(), mapped, si->GetStringValue("Pad1", "Cross", "none").c_str());
}

void GameVaultHost::UseHome(const std::string& home, bool fps)
{
	INISettingsInterface* const base = BaseSettings();
	// fresh settings and no copy in the shared folder (a reset console): the copy on the games drive
	if (s_settings_were_fresh && !home.empty())
	{
		s_settings_were_fresh = false;
		const std::string from = HomeSettings(home);
		if (FileSystem::FileExists(from.c_str()) && base && FileSystem::CopyFilePath(from.c_str(), base->GetFileName().c_str(), true))
		{
			auto lock = Host::GetSettingsLock();
			base->Load();
			Console.WriteLn("Game Vault: settings put back from %s", from.c_str());
		}
	}
	{
		auto lock = Host::GetSettingsLock();
		SettingsInterface* si = Host::Internal::GetBaseSettingsLayer();
		// The frame-rate display: Game Vault sends "fps=1" while it is on. Turned off again
		// only when Game Vault turned it on, so a choice made in XBSX2's own settings is left alone.
		const bool ours = si->GetBoolValue("GameVault", "FpsOn", false);
		if (fps || ours)
		{
			si->SetBoolValue("EmuCore/GS", "OsdShowFPS", fps);
			si->SetBoolValue("GameVault", "FpsOn", fps);
		}
		for (size_t i = 0; i < std::size(s_home_folders); i++)
		{
			const std::string current = si->GetStringValue("Folders", s_home_folders[i][0], s_own_folders[i]);
			if (home.empty())
			{
				if (current.find("\\Game Vault\\") != std::string::npos)
					si->SetStringValue("Folders", s_home_folders[i][0], s_own_folders[i]);
				continue;
			}
			const std::string want = Path::Combine(home, s_home_folders[i][1]);
			FileSystem::CreateDirectoryPath(want.c_str(), true);
			// only XBSX2's own storage is emptied, never another drive's Game Vault folder
			const std::string own = Path::IsAbsolute(current) ? current : Path::Combine(EmuFolders::DataRoot, current);
			if (_strnicmp(own.c_str(), EmuFolders::DataRoot.c_str(), EmuFolders::DataRoot.size()) == 0)
				MoveFolderToHome(own, want);
			si->SetStringValue("Folders", s_home_folders[i][0], want.c_str());
		}
	}
	Host::CommitBaseSettingChanges();
	VMManager::Internal::UpdateEmuFolders();
	// keep the copies up to date: every start from Game Vault saves the settings as they are now
	if (base)
	{
		const std::string& own = base->GetFileName();
		const std::string backup = SettingsBackup();
		if (!backup.empty())
			FileSystem::CopyFilePath(own.c_str(), backup.c_str(), true);
		if (!home.empty())
			FileSystem::CopyFilePath(own.c_str(), HomeSettings(home).c_str(), true);
	}
	// the log of each session goes to the drive too, where a player can pick it up for a problem report
	if (!home.empty())
	{
		const std::string logs = Path::Combine(home, "logs");
		FileSystem::CreateDirectoryPath(logs.c_str(), true);
		// a file still held open (an earlier XBSX2 that never quite closed) cannot be written: the second name then,
		// else the log silently stayed in XBSX2's own storage and the drive's showed an old session (2026-10-19)
		if (!Log::SetFileOutputLevel(LOGLEVEL_INFO, Path::Combine(logs, "PS2 (XBSX2) - last session.txt")))
		{
			const bool second = Log::SetFileOutputLevel(LOGLEVEL_INFO, Path::Combine(logs, "PS2 (XBSX2) - last session (2).txt"));
			Console.WriteLn("Game Vault: the drive's log file is in use, %s",
				second ? "this session's log is 'last session (2)'" : "and so is the second one: this log stays in XBSX2's own storage");
		}
		LogControllerSetup(base);
	}
}
