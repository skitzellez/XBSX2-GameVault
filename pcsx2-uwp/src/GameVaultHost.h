/* Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 * GPL-3.0-or-later, like PCSX2 / XBSX2.
 *
 * What XBSX2 does differently when Game Vault starts it: settings that outlive a reinstall, controller 1 set up by
 * itself, BIOS / memory cards / states / logs on the games drive. See GameVaultHost.cpp.
 */

#pragma once

#include <string>

class INISettingsInterface;

namespace GameVaultHost
{
	/// Before the settings are loaded: no PCSX2.ini of its own (a reinstall) puts back the copy in the shared folder.
	/// Returns whether there is a settings file now.
	bool RestoreSettings(const std::string& path);

	/// After the settings are loaded: controller 1 with no buttons at all gets the Xbox controller layout.
	void AutoMapController(INISettingsInterface& si);

	/// On the CPU thread, before a game from Game Vault starts: "home" is Game Vault's folder on the games drive
	/// (empty: XBSX2's own folders), "fps" whether Game Vault's frame-rate display is on.
	void UseHome(const std::string& home, bool fps);
} // namespace GameVaultHost
