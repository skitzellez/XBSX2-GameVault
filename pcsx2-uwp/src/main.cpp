// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/PrecompiledHeader.h"

#include "UWPHost.h"
#include "UWPKeyboard.h"
#include "GameVaultHost.h"

#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/VMManager.h"

#include <windows.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include "imgui.h"

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
	winrt::hstring m_launchOnExit;
	bool m_initialized = false;
	std::optional<VMBootParameters> m_autoboot;
	std::string m_home; // Game Vault's folder on the games drive (see GameVaultHost::UseHome)
	bool m_fps = false;

	IFrameworkView CreateView()
	{
		return *this;
	}

	void Initialize(CoreApplicationView const& v)
	{
		v.Activated({this, &App::OnActivate});
		CoreApplication::Suspending([](auto&&, auto&&) { UWPHost::SaveSettings(); });

		namespace WGI = winrt::Windows::Gaming::Input;

		const char* error;
		if (!VMManager::PerformEarlyHardwareChecks(&error))
		{
			// Reuse QtHost's translation for the startup error title.
			Host::ReportErrorAsync(TRANSLATE_SV("QtHost", "Startup Error"), error);
			return;
		}

		m_initialized = true;

		if (winrt::try_get_activation_factory<WGI::RawGameController>())
		{
			WGI::RawGameController::RawGameControllerAdded(
				[](auto&&, const WGI::RawGameController raw_game_controller) {
					Host::RunOnCPUThread([]() { InputManager::ReloadDevices(); });
				});

			WGI::RawGameController::RawGameControllerRemoved(
				[](auto&&, const WGI::RawGameController raw_game_controller) {
					Host::RunOnCPUThread([]() { InputManager::ReloadDevices(); });
				});
		}
	}

	void OnActivate(const winrt::Windows::ApplicationModel::Core::CoreApplicationView&,
		const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs& args)
	{
		CoreWindow::GetForCurrentThread().Activate();
		if (!m_initialized)
			return;

		std::stringstream filePath;

		if (args.Kind() == Windows::ApplicationModel::Activation::ActivationKind::Protocol)
		{
			auto protocolActivatedEventArgs{
				args.as<Windows::ApplicationModel::Activation::ProtocolActivatedEventArgs>()};
			auto query = protocolActivatedEventArgs.Uri().QueryParsed();

			for (uint32_t i = 0; i < query.Size(); i++)
			{
				auto arg = query.GetAt(i);

				// Parse the command line from the protocol URI.
				if (arg.Name() == winrt::hstring(L"cmd"))
				{
					std::string argVal = winrt::to_string(arg.Value());

					// Strip the executable from the cmd argument.
					if (argVal.rfind("xbsx2.exe", 0) == 0)
					{
						argVal = argVal.substr(10, argVal.length());
					}

					std::istringstream iss(argVal);
					std::string s;
					bool first = true;

					// Keep backslashes in quoted paths.
					while (iss >> std::quoted(s, '"', (char)0))
					{
						if (!first)
							filePath << ' ';
						first = false;
						filePath << s;
					}
				}
				else if (arg.Name() == winrt::hstring(L"home"))
				{
					m_home = winrt::to_string(arg.Value());
				}
				else if (arg.Name() == winrt::hstring(L"fps"))
				{
					m_fps = arg.Value() == L"1";
				}
				else if (arg.Name() == winrt::hstring(L"launchOnExit"))
				{
					// Return to the launching frontend on exit.
					m_launchOnExit = arg.Value();
				}
			}
		}

		// Game Vault: App type "App" (Dev Home) gives only 1 GB, and games run out of memory and close. Started by
		// Game Vault with that little: straight back to it with "from=lowmem-xbsx2", which says on the TV what to
		// change.
		if (std::wstring_view(m_launchOnExit).rfind(L"gamevault:", 0) == 0 &&
			Windows::System::MemoryManager::AppMemoryUsageLimit() < (2048ull << 20))
		{
			Windows::System::Launcher::LaunchUriAsync(Windows::Foundation::Uri(L"gamevault:?from=lowmem-xbsx2"))
				.Completed([](auto&&, auto&&) { CoreApplication::Exit(); });
			return;
		}

		std::string gamePath = filePath.str();
		if (!gamePath.empty())
		{
			VMBootParameters params;
			// "disc:" instead of a file: play the disc in the console's own drive
			if (gamePath.rfind("disc:", 0) == 0)
				params.source_type = CDVD_SourceType::Disc;
			params.filename = std::move(gamePath);
			if (g_emu_thread)
				BootGame(std::move(params));
			else
				m_autoboot = std::move(params);
		}
	}

	// Game Vault's folders are set on the CPU thread before the game starts; a game already running is left alone
	void BootGame(VMBootParameters params)
	{
		Host::RunOnCPUThread([params = std::move(params), home = m_home, fps = m_fps]() mutable {
			if (VMManager::HasValidVM())
				return;
			GameVaultHost::UseHome(home, fps);
			g_emu_thread->startVM(std::move(params));
		});
	}

	void Load(hstring const&)
	{
	}

	void Uninitialize()
	{
	}

	void Run()
	{
		if (m_initialized)
			m_initialized = UWPHost::InitializeConfig();

		// Start up the CPU thread.
		if (m_initialized && EmuThread::start())
		{
			if (m_autoboot)
			{
				BootGame(std::move(m_autoboot.value()));
				m_autoboot.reset();
			}
		}
		// This doesn't return until we exit.
		for (;;)
		{
			const bool dialog_pending = UWPHost::ProcessPendingEvents();
			const bool cpu_running = g_emu_thread && g_emu_thread->isRunning();
			if (!cpu_running && !dialog_pending)
				break;
			UWPHost::ProcessUIEvents();
		}
		// Shutting down.
		UWPHost::Shutdown();

		if (!m_launchOnExit.empty())
		{
			Windows::System::Launcher::LaunchUriAsync(Windows::Foundation::Uri(m_launchOnExit)).Completed([](auto&&, auto&&) { CoreApplication::Exit(); });
		}
		else
		{
			CoreApplication::Exit();
		}
	}

	void SetWindow(CoreWindow const& window)
	{
		UWPHost::SetWindow(window);
		window.Closed([](auto&&, auto&&) { Host::RequestExitApplication(false); });
		const auto navigation = Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
		navigation.BackRequested([](auto&&, const BackRequestedEventArgs& args) { args.Handled(true); });

		window.CharacterReceived({this, &App::OnKeyInput});
		window.KeyDown({this, &App::OnKeyDown});
		window.KeyUp({this, &App::OnKeyUp});
		window.PointerMoved({this, &App::OnPointerMoved});
		window.PointerPressed({this, &App::OnPointerPressed});
		window.PointerReleased({this, &App::OnPointerReleased});
		window.PointerWheelChanged({this, &App::OnPointerWheelChanged});
		window.SizeChanged({this, &App::OnSizeChanged});
	}

	void OnKeyInput(const IInspectable&, const winrt::Windows::UI::Core::CharacterReceivedEventArgs& args)
	{
		if (args.KeyCode() == L'\b')
			return;

		if (ImGuiManager::WantsTextInput())
		{
			MTGS::RunOnGSThread([c = args.KeyCode()]() {
				if (!ImGui::GetCurrentContext())
					return;

				ImGui::GetIO().AddInputCharacter(c);
			});
		}
	}

	void OnKeyDown(const IInspectable&, const winrt::Windows::UI::Core::KeyEventArgs& args)
	{
		if (args.KeyStatus().WasKeyDown)
			return;

		const u32 key = static_cast<u32>(args.VirtualKey());
		// Ignore keys without a binding name.
		if (!UWPKeyboard::IsValidKey(key))
			return;

		Host::RunOnCPUThread([key]() {
			InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), 1.0f);
		});
	}

	void OnKeyUp(const IInspectable&, const winrt::Windows::UI::Core::KeyEventArgs& args)
	{
		const u32 key = static_cast<u32>(args.VirtualKey());
		// Ignore keys without a binding name.
		if (!UWPKeyboard::IsValidKey(key))
			return;

		Host::RunOnCPUThread([key]() {
			InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), 0.0f);
		});
	}

	void OnPointerMoved(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
	{
		if (UWPHost::IsRelativeMouseMode())
			return;

		const auto pos = args.CurrentPoint().Position();
		const float scale = UWPHost::GetDisplayScale();
		const float x = pos.X * scale;
		const float y = pos.Y * scale;

		Host::RunOnCPUThread([x, y]() {
			InputManager::UpdatePointerAbsolutePosition(0, x, y);
		});
	}

	void OnPointerButton(const winrt::Windows::UI::Core::PointerEventArgs& args, bool pressed)
	{
		using winrt::Windows::UI::Input::PointerUpdateKind;
		const PointerUpdateKind kind = args.CurrentPoint().Properties().PointerUpdateKind();

		u32 button = 0;
		switch (kind)
		{
			case PointerUpdateKind::LeftButtonPressed:
			case PointerUpdateKind::LeftButtonReleased:
				button = 0;
				break;
			case PointerUpdateKind::RightButtonPressed:
			case PointerUpdateKind::RightButtonReleased:
				button = 1;
				break;
			case PointerUpdateKind::MiddleButtonPressed:
			case PointerUpdateKind::MiddleButtonReleased:
				button = 2;
				break;
			default:
				return;
		}

		Host::RunOnCPUThread([button, pressed]() {
			InputManager::InvokeEvents(InputManager::MakePointerButtonKey(0, button), pressed ? 1.0f : 0.0f);
		});
	}

	void OnPointerPressed(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
	{
		OnPointerButton(args, true);
	}

	void OnPointerReleased(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
	{
		OnPointerButton(args, false);
	}

	void OnPointerWheelChanged(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
	{
		const float wheel_delta = static_cast<float>(args.CurrentPoint().Properties().MouseWheelDelta()) / 120.0f;
		if (wheel_delta == 0.0f)
			return;

		Host::RunOnCPUThread([wheel_delta]() {
			InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelY, wheel_delta);
		});
	}

	void OnSizeChanged(const IInspectable&, const WindowSizeChangedEventArgs&)
	{
		UWPHost::UpdateWindowSize();
	}
};

extern "C" void uwp_redirect_file_apis_startup(void);

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	// disc images on any USB drive (FAT32 / exFAT too): file calls go through the broker (UWPFileRedirect.cpp)
	uwp_redirect_file_apis_startup();
	SDL_SetMainReady();

	winrt::init_apartment();

	CoreApplication::Run(make<App>());

	winrt::uninit_apartment();
}
