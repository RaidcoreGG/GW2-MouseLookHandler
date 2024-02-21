#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "nexus/Nexus.h"
#include "mumble/Mumble.h"
#include "imgui/imgui.h"

#include "Version.h"

#include "nlohmann/json.hpp"
using json = nlohmann::json;

namespace ImGui
{
	static bool Tooltip()
	{
		bool hovered = ImGui::IsItemHovered();
		if (hovered)
		{
			ImGui::BeginTooltip();
		}
		return hovered;
	}

	static void TooltipGeneric(const char* fmt, ...)
	{
		if (ImGui::Tooltip())
		{
			ImGui::Text(fmt);
			ImGui::EndTooltip();
		}
	}
}

std::map<unsigned short, std::string> ScancodeLookupTable;

const char* ConvertToUTF8(const char* multibyteStr)
{
	char* utf8Str = nullptr;

	int wideCharCount = MultiByteToWideChar(CP_ACP, 0, multibyteStr, -1, NULL, 0);
	if (wideCharCount > 0)
	{
		wchar_t* wideCharBuff = new wchar_t[wideCharCount];
		MultiByteToWideChar(CP_ACP, 0, multibyteStr, -1, wideCharBuff, wideCharCount);

		int utf8Count = WideCharToMultiByte(CP_UTF8, 0, wideCharBuff, -1, NULL, 0, NULL, NULL);
		if (utf8Count > 0)
		{
			utf8Str = new char[utf8Count];
			WideCharToMultiByte(CP_UTF8, 0, wideCharBuff, -1, utf8Str, utf8Count, NULL, NULL);
		}

		delete[] wideCharBuff;
	}

	return utf8Str;
}

std::string KeybindToString(Keybind& keybind, bool padded)
{
	if (!keybind.Key) { return "(null)"; }

	char* buff = new char[100];
	std::string str;

	if (keybind.Alt)
	{
		GetKeyNameTextA(MapVirtualKeyA(VK_MENU, MAPVK_VK_TO_VSC) << 16, buff, 100);
		str.append(buff);
		str.append(padded ? " + " : "+");
	}

	if (keybind.Ctrl)
	{
		GetKeyNameTextA(MapVirtualKeyA(VK_CONTROL, MAPVK_VK_TO_VSC) << 16, buff, 100);
		str.append(buff);
		str.append(padded ? " + " : "+");
	}

	if (keybind.Shift)
	{
		GetKeyNameTextA(MapVirtualKeyA(VK_SHIFT, MAPVK_VK_TO_VSC) << 16, buff, 100);
		str.append(buff);
		str.append(padded ? " + " : "+");
	}

	HKL hkl = GetKeyboardLayout(0);
	UINT vk = MapVirtualKeyA(keybind.Key, MAPVK_VSC_TO_VK);

	if (vk >= 65 && vk <= 90 || vk >= 48 && vk <= 57)
	{
		GetKeyNameTextA(keybind.Key << 16, buff, 100);
		str.append(buff);
	}
	else
	{
		auto it = ScancodeLookupTable.find(keybind.Key);
		if (it != ScancodeLookupTable.end())
		{
			str.append(it->second);
		}
	}

	delete[] buff;

	std::transform(str.begin(), str.end(), str.begin(), ::toupper);

	// Convert Multibyte encoding to UFT-8 bytes
	const char* multibyte_pointer = str.c_str();
	const char* utf8_bytes = ConvertToUTF8(multibyte_pointer);

	return std::string(utf8_bytes);
}

bool operator==(const Keybind& lhs, const Keybind& rhs)
{
	return	lhs.Key == rhs.Key &&
		lhs.Alt == rhs.Alt &&
		lhs.Ctrl == rhs.Ctrl &&
		lhs.Shift == rhs.Shift;
}

bool operator!=(const Keybind& lhs, const Keybind& rhs)
{
	return	!(lhs == rhs);
}

typedef struct KeystrokeMessageFlags
{
	unsigned RepeatCount : 16;
	unsigned ScanCode : 8;
	unsigned ExtendedFlag : 1;
	unsigned Reserved : 4;
	unsigned ContextCode : 1;
	unsigned PreviousKeyState : 1;
	unsigned TransitionState : 1;

	unsigned short GetScanCode()
	{
		unsigned short ret = ScanCode;

		if (ExtendedFlag)
		{
			ret |= 0xE000;
		}

		return ret;
	}
} KeyLParam;

KeystrokeMessageFlags& LParamToKMF(LPARAM& lp)
{
	return *(KeystrokeMessageFlags*)&lp;
}

LPARAM& KMFToLParam(KeystrokeMessageFlags& kmf)
{
	return *(LPARAM*)&kmf;
}

LPARAM GetLPARAM(uint32_t key, bool down, bool sys)
{
	uint64_t lp;
	lp = down ? 0 : 1; // transition state
	lp = lp << 1;
	lp += down ? 0 : 1; // previous key state
	lp = lp << 1;
	lp += 0; // context code
	lp = lp << 1;
	lp = lp << 4;
	lp = lp << 1;
	lp = lp << 8;
	lp += MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
	lp = lp << 16;
	lp += 1;

	//APIDefs->Log(ELogLevel_TRACE, std::to_string(lp).c_str());

	return lp;
}

void AddonLoad(AddonAPI* aApi);
void AddonUnload();
UINT AddonWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void AddonRender();
void AddonOptions();

void LoadSettings(std::filesystem::path aPath);
void SaveSettings(std::filesystem::path aPath);

HWND Game;
HMODULE hSelf;
AddonDefinition AddonDef{};
AddonAPI* APIDefs = nullptr;
Mumble::Data* MumbleLink = nullptr;
NexusLinkData* NexusLink = nullptr;

std::filesystem::path AddonPath{};
json Settings{};
std::filesystem::path SettingsPath{};
std::mutex Mutex;

bool isSettingKeybind = false;
bool resetCursorToCenter = false;
bool enableInCombat = false;
bool enableOnMount = false;
bool wasInCombat = false;
bool wasMoving = false;
bool wasMounted = false;
bool redirectLeftClick = false;
bool redirectRightClick = false;

bool actionCamControlled = false;

enum class ETargetBind
{
	None,
	DisableActionCam,
	LeftClick,
	RightClick
};

ETargetBind CurrentTargetBind = ETargetBind::None;
Keybind CurrentKeybind{};
Keybind disableActionCam{};
Keybind leftClickTarget{};
Keybind rightClickTarget{};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH: hSelf = hModule; break;
	case DLL_PROCESS_DETACH: break;
	case DLL_THREAD_ATTACH: break;
	case DLL_THREAD_DETACH: break;
	}
	return TRUE;
}

extern "C" __declspec(dllexport) AddonDefinition* GetAddonDef()
{
	AddonDef.Signature = 0x09872345;
	AddonDef.APIVersion = NEXUS_API_VERSION;
	AddonDef.Name = "MouseLookHandler";
	AddonDef.Version.Major = V_MAJOR;
	AddonDef.Version.Minor = V_MINOR;
	AddonDef.Version.Build = V_BUILD;
	AddonDef.Version.Revision = V_REVISION;
	AddonDef.Author = "Raidcore";
	AddonDef.Description = "Automatically toggles action cam while moving.";
	AddonDef.Load = AddonLoad;
	AddonDef.Unload = AddonUnload;
	AddonDef.Flags = EAddonFlags_None;

	/* not necessary if hosted on Raidcore, but shown anyway for the example also useful as a backup resource */
	AddonDef.Provider = EUpdateProvider_GitHub;
	AddonDef.UpdateLink = "https://github.com/RaidcoreGG/MouseLookHandler";

	return &AddonDef;
}

void AddonLoad(AddonAPI* aApi)
{
	APIDefs = aApi;
	ImGui::SetCurrentContext(APIDefs->ImguiContext);
	ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))APIDefs->ImguiMalloc, (void(*)(void*, void*))APIDefs->ImguiFree); // on imgui 1.80+

	MumbleLink = (Mumble::Data*)APIDefs->GetResource("DL_MUMBLE_LINK");
	NexusLink = (NexusLinkData*)APIDefs->GetResource("DL_NEXUS_LINK");

	APIDefs->RegisterRender(ERenderType_Render, AddonRender);
	APIDefs->RegisterRender(ERenderType_OptionsRender, AddonOptions);

	APIDefs->RegisterWndProc(AddonWndProc); // lazy way to get game handle

	for (long long i = 0; i < 255; i++)
	{
		KeyLParam key{};
		key.ScanCode = i;
		char* buff = new char[64];
		std::string str;
		GetKeyNameTextA(static_cast<LONG>(KMFToLParam(key)), buff, 64);
		str.append(buff);

		ScancodeLookupTable[key.GetScanCode()] = str;

		key.ExtendedFlag = 1;
		buff = new char[64];
		str = "";
		GetKeyNameTextA(static_cast<LONG>(KMFToLParam(key)), buff, 64);
		str.append(buff);

		ScancodeLookupTable[key.GetScanCode()] = str;

		delete[] buff;
	}

	AddonPath = APIDefs->GetAddonDirectory("MouseLookHandler");
	SettingsPath = APIDefs->GetAddonDirectory("MouseLookHandler/settings.json");
	std::filesystem::create_directory(AddonPath);
	LoadSettings(SettingsPath);
}
void AddonUnload()
{
	APIDefs->UnregisterWndProc(AddonWndProc);

	APIDefs->UnregisterRender(AddonOptions);
	APIDefs->UnregisterRender(AddonRender);

	MumbleLink = nullptr;
	NexusLink = nullptr;
}

UINT AddonWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	Game = hWnd;

	if (uMsg == WM_LBUTTONDOWN && true == redirectLeftClick && true == actionCamControlled)
	{
		if (leftClickTarget == Keybind{} || leftClickTarget.Key == 0) { return uMsg; }

		if (leftClickTarget.Alt)
		{
			PostMessage(Game, WM_SYSKEYDOWN, VK_CONTROL, GetLPARAM(VK_CONTROL, true, true));
			Sleep(5);
		}
		if (leftClickTarget.Ctrl)
		{
			PostMessage(Game, WM_KEYDOWN, VK_CONTROL, GetLPARAM(VK_CONTROL, true, false));
			Sleep(5);
		}
		if (leftClickTarget.Shift)
		{
			PostMessage(Game, WM_KEYDOWN, VK_SHIFT, GetLPARAM(VK_SHIFT, true, false));
			Sleep(5);
		}

		KeyLParam key{};
		key.TransitionState = false;
		key.ExtendedFlag = (leftClickTarget.Key & 0xE000) != 0;
		key.ScanCode = leftClickTarget.Key;

		PostMessage(Game, WM_KEYDOWN, MapVirtualKeyA(leftClickTarget.Key, MAPVK_VSC_TO_VK), KMFToLParam(key));

		return 0;
	}
	if (uMsg == WM_RBUTTONDOWN && true == redirectRightClick && true == actionCamControlled)
	{
		if (rightClickTarget == Keybind{} || rightClickTarget.Key == 0) { return uMsg; }

		if (rightClickTarget.Alt)
		{
			PostMessage(Game, WM_SYSKEYDOWN, VK_CONTROL, GetLPARAM(VK_CONTROL, true, true));
			Sleep(5);
		}
		if (rightClickTarget.Ctrl)
		{
			PostMessage(Game, WM_KEYDOWN, VK_CONTROL, GetLPARAM(VK_CONTROL, true, false));
			Sleep(5);
		}
		if (rightClickTarget.Shift)
		{
			PostMessage(Game, WM_KEYDOWN, VK_SHIFT, GetLPARAM(VK_SHIFT, true, false));
			Sleep(5);
		}

		KeyLParam key{};
		key.TransitionState = false;
		key.ExtendedFlag = (rightClickTarget.Key & 0xE000) != 0;
		key.ScanCode = rightClickTarget.Key;

		PostMessage(Game, WM_KEYDOWN, MapVirtualKeyA(rightClickTarget.Key, MAPVK_VSC_TO_VK), KMFToLParam(key));

		return 0;
	}

	if (uMsg == WM_LBUTTONUP && true == redirectLeftClick && true == actionCamControlled)
	{
		KeyLParam key{};
		key.TransitionState = true;
		key.ExtendedFlag = (leftClickTarget.Key & 0xE000) != 0;
		key.ScanCode = leftClickTarget.Key;

		PostMessage(Game, WM_KEYUP, MapVirtualKeyA(leftClickTarget.Key, MAPVK_VSC_TO_VK), KMFToLParam(key));

		if (leftClickTarget.Alt)
		{
			PostMessage(Game, WM_SYSKEYUP, VK_CONTROL, GetLPARAM(VK_CONTROL, false, true));
			Sleep(5);
		}
		if (leftClickTarget.Ctrl)
		{
			PostMessage(Game, WM_KEYUP, VK_CONTROL, GetLPARAM(VK_CONTROL, false, false));
			Sleep(5);
		}
		if (leftClickTarget.Shift)
		{
			PostMessage(Game, WM_KEYUP, VK_SHIFT, GetLPARAM(VK_SHIFT, false, false));
			Sleep(5);
		}
	}
	if (uMsg == WM_RBUTTONUP && true == redirectRightClick && true == actionCamControlled)
	{
		KeyLParam key{};
		key.TransitionState = true;
		key.ExtendedFlag = (rightClickTarget.Key & 0xE000) != 0;
		key.ScanCode = rightClickTarget.Key;

		PostMessage(Game, WM_KEYUP, MapVirtualKeyA(rightClickTarget.Key, MAPVK_VSC_TO_VK), KMFToLParam(key));

		if (rightClickTarget.Alt)
		{
			PostMessage(Game, WM_SYSKEYUP, VK_CONTROL, GetLPARAM(VK_CONTROL, false, true));
			Sleep(5);
		}
		if (rightClickTarget.Ctrl)
		{
			PostMessage(Game, WM_KEYUP, VK_CONTROL, GetLPARAM(VK_CONTROL, false, false));
			Sleep(5);
		}
		if (rightClickTarget.Shift)
		{
			PostMessage(Game, WM_KEYUP, VK_SHIFT, GetLPARAM(VK_SHIFT, false, false));
			Sleep(5);
		}
	}

	if (isSettingKeybind)
	{
		if (WM_KEYDOWN == uMsg)
		{
			if (wParam <= 255)
			{
				KeyLParam keylp = LParamToKMF(lParam);

				Keybind kb{};
				kb.Alt = GetKeyState(VK_MENU) & 0x8000;
				kb.Ctrl = GetKeyState(VK_CONTROL) & 0x8000;
				kb.Shift = GetKeyState(VK_SHIFT) & 0x8000;
				kb.Key = keylp.GetScanCode();

				// if shift, ctrl or alt set key to 0
				if (wParam == 16 || wParam == 17 || wParam == 18)
				{
					kb.Key = 0;
				}

				CurrentKeybind = kb;
			}
		}

		if (uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYUP || uMsg == WM_KEYUP)
		{
			return 0;
		}
	}

	return uMsg;
}

void AddonRender()
{
	if (nullptr == NexusLink || nullptr == MumbleLink) { return; }

	if ((true == NexusLink->IsMoving && false == wasMoving) ||
		(true == enableInCombat && true == MumbleLink->Context.IsInCombat && false == wasInCombat) ||
		(true == enableOnMount && Mumble::EMountIndex::None != MumbleLink->Context.MountIndex && false == wasMounted))
	{
		wasMoving = NexusLink->IsMoving;
		wasInCombat = MumbleLink->Context.IsInCombat;
		wasMounted = MumbleLink->Context.MountIndex != Mumble::EMountIndex::None;
		actionCamControlled = true;

		if (disableActionCam == Keybind{} || disableActionCam.Key == 0) { return; }

		if (disableActionCam.Alt)
		{
			PostMessage(Game, WM_SYSKEYDOWN, VK_CONTROL, GetLPARAM(VK_CONTROL, true, true));
			Sleep(5);
		}
		if (disableActionCam.Ctrl)
		{
			PostMessage(Game, WM_KEYDOWN, VK_CONTROL, GetLPARAM(VK_CONTROL, true, false));
			Sleep(5);
		}
		if (disableActionCam.Shift)
		{
			PostMessage(Game, WM_KEYDOWN, VK_SHIFT, GetLPARAM(VK_SHIFT, true, false));
			Sleep(5);
		}

		KeyLParam key{};
		key.TransitionState = false;
		key.ExtendedFlag = (disableActionCam.Key & 0xE000) != 0;
		key.ScanCode = disableActionCam.Key;

		PostMessage(Game, WM_KEYDOWN, MapVirtualKeyA(disableActionCam.Key, MAPVK_VSC_TO_VK), KMFToLParam(key));
	}
	else if ((false == NexusLink->IsMoving && true == wasMoving) ||
			(true == enableInCombat && false == MumbleLink->Context.IsInCombat && true == wasInCombat) ||
			(true == enableOnMount && Mumble::EMountIndex::None == MumbleLink->Context.MountIndex && true == wasMounted))
	{
		wasMoving = NexusLink->IsMoving;
		wasInCombat = MumbleLink->Context.IsInCombat;
		wasMounted = MumbleLink->Context.MountIndex != Mumble::EMountIndex::None;

		if (wasMoving || wasInCombat || wasMounted)
		{
			// still moving, etc do not send disable
			return;
		}

		actionCamControlled = false;

		KeyLParam key{};
		key.TransitionState = false;
		key.ExtendedFlag = (disableActionCam.Key & 0xE000) != 0;
		key.ScanCode = disableActionCam.Key;
		key.TransitionState = true;
		PostMessage(Game, WM_KEYUP, MapVirtualKeyA(disableActionCam.Key, MAPVK_VSC_TO_VK), KMFToLParam(key));

		if (disableActionCam.Alt)
		{
			PostMessage(Game, WM_SYSKEYUP, VK_CONTROL, GetLPARAM(VK_CONTROL, false, true));
			Sleep(5);
		}
		if (disableActionCam.Ctrl)
		{
			PostMessage(Game, WM_KEYUP, VK_CONTROL, GetLPARAM(VK_CONTROL, false, false));
			Sleep(5);
		}
		if (disableActionCam.Shift)
		{
			PostMessage(Game, WM_KEYUP, VK_SHIFT, GetLPARAM(VK_SHIFT, false, false));
		}
	}

	if (actionCamControlled && resetCursorToCenter)
	{
		RECT rect{};
		GetWindowRect(Game, &rect);

		SetCursorPos((rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2);
	}
}

void AddonOptions()
{
	ImGui::TextDisabled("MouseLookHandler");

	ImGui::Text("Disable Action Cam:");
	ImGui::SameLine();
	if (ImGui::Button(KeybindToString(disableActionCam, true).c_str()))
	{
		CurrentTargetBind = ETargetBind::DisableActionCam;
		ImGui::OpenPopup("Set Keybind: MouseLookHandler", ImGuiPopupFlags_AnyPopupLevel);
	}
	ImGui::TooltipGeneric("This should match whatever keybind you're using in-game for \"Disable Action Cam\".\nAvoid using keybinds with modifiers such as Alt, Ctrl and Shift as those will be permanently \"pressed\" while moving.\nUse a key that you don't use at all and can't easily reach.");

	if (ImGui::Checkbox("Reset Cursor to Center after action cam", &resetCursorToCenter))
	{
		SaveSettings(SettingsPath);
	}

	if (ImGui::Checkbox("Always enable in combat", &enableInCombat))
	{
		SaveSettings(SettingsPath);
	}

	if (ImGui::Checkbox("Stay enabled while standing on mount", &enableOnMount))
	{
		SaveSettings(SettingsPath);
	}

	ImGui::Checkbox("Redirect Left-Click while moving", &redirectLeftClick);
	if (redirectLeftClick)
	{
		ImGui::Text("Left-Click Action:");
		ImGui::SameLine();
		if (ImGui::Button(KeybindToString(leftClickTarget, true).c_str()))
		{
			CurrentTargetBind = ETargetBind::LeftClick;
			ImGui::OpenPopup("Set Keybind: MouseLookHandler", ImGuiPopupFlags_AnyPopupLevel);
		}
	}
	ImGui::Checkbox("Redirect Right-Click while moving", &redirectRightClick);
	if (redirectRightClick)
	{
		ImGui::Text("Right-Click Action:");
		ImGui::SameLine();
		if (ImGui::Button(KeybindToString(rightClickTarget, true).c_str()))
		{
			CurrentTargetBind = ETargetBind::RightClick;
			ImGui::OpenPopup("Set Keybind: MouseLookHandler", ImGuiPopupFlags_AnyPopupLevel);
		}
	}

	ImGui::Separator();

	if (ImGui::BeginPopupModal("Set Keybind: MouseLookHandler"))
	{
		isSettingKeybind = true;
		if (CurrentKeybind == Keybind{})
		{
			switch (CurrentTargetBind)
			{
			case ETargetBind::DisableActionCam:
				ImGui::Text(KeybindToString(disableActionCam, true).c_str());
				break;
			case ETargetBind::LeftClick:
				ImGui::Text(KeybindToString(leftClickTarget, true).c_str());
				break;
			case ETargetBind::RightClick:
				ImGui::Text(KeybindToString(rightClickTarget, true).c_str());
				break;
			}
		}
		else
		{
			ImGui::Text(KeybindToString(CurrentKeybind, true).c_str());
		}

		bool close = false;

		if (ImGui::Button("Unbind"))
		{
			switch (CurrentTargetBind)
			{
			case ETargetBind::DisableActionCam:
				disableActionCam = {};
				break;
			case ETargetBind::LeftClick:
				leftClickTarget = {};
				break;
			case ETargetBind::RightClick:
				rightClickTarget = {};
				break;
			}
			close = true;
		}

		/* i love imgui */
		ImGui::SameLine();
		ImGui::Spacing();
		ImGui::SameLine();
		ImGui::Spacing();
		ImGui::SameLine();
		ImGui::Spacing();
		ImGui::SameLine();
		/* i love imgui end*/

		if (ImGui::Button("Accept"))
		{
			switch (CurrentTargetBind)
			{
			case ETargetBind::DisableActionCam:
				disableActionCam = CurrentKeybind;
				break;
			case ETargetBind::LeftClick:
				leftClickTarget = CurrentKeybind;
				break;
			case ETargetBind::RightClick:
				rightClickTarget = CurrentKeybind;
				break;
			}
			close = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			close = true;
		}

		if (close)
		{
			CurrentKeybind = Keybind{};
			isSettingKeybind = false;
			SaveSettings(SettingsPath);
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

void LoadSettings(std::filesystem::path aPath)
{
	if (!std::filesystem::exists(aPath)) { return; }

	Mutex.lock();
	{
		try
		{
			std::ifstream file(aPath);
			Settings = json::parse(file);
			file.close();
		}
		catch (json::parse_error& ex)
		{
			APIDefs->Log(ELogLevel_WARNING, "MouseLookHandler: Settings.json could not be parsed.");
			APIDefs->Log(ELogLevel_WARNING, ex.what());
		}
	}
	Mutex.unlock();

	if (!Settings.is_null())
	{
		if (!Settings["DAC_KEY"].is_null()) { Settings["DAC_KEY"].get_to(disableActionCam.Key); }
		if (!Settings["DAC_ALT"].is_null()) { Settings["DAC_ALT"].get_to(disableActionCam.Alt); }
		if (!Settings["DAC_CTRL"].is_null()) { Settings["DAC_CTRL"].get_to(disableActionCam.Ctrl); }
		if (!Settings["DAC_SHIFT"].is_null()) { Settings["DAC_SHIFT"].get_to(disableActionCam.Shift); }

		if (!Settings["RESET_CURSOR_CENTER"].is_null()) { Settings["RESET_CURSOR_CENTER"].get_to(resetCursorToCenter); }
		if (!Settings["ENABLE_DURING_COMBAT"].is_null()) { Settings["ENABLE_DURING_COMBAT"].get_to(enableInCombat); }
		if (!Settings["ENABLE_ON_MOUNT"].is_null()) { Settings["ENABLE_ON_MOUNT"].get_to(enableOnMount); }

		if (!Settings["REDIRECT_LEFTCLICK"].is_null()) { Settings["REDIRECT_LEFTCLICK"].get_to(redirectLeftClick); }
		if (!Settings["LC_KEY"].is_null()) { Settings["LC_KEY"].get_to(leftClickTarget.Key); }
		if (!Settings["LC_ALT"].is_null()) { Settings["LC_ALT"].get_to(leftClickTarget.Alt); }
		if (!Settings["LC_CTRL"].is_null()) { Settings["LC_CTRL"].get_to(leftClickTarget.Ctrl); }
		if (!Settings["LC_SHIFT"].is_null()) { Settings["LC_SHIFT"].get_to(leftClickTarget.Shift); }

		if (!Settings["REDIRECT_RIGHTCLICK"].is_null()) { Settings["REDIRECT_RIGHTCLICK"].get_to(redirectRightClick); }
		if (!Settings["RC_KEY"].is_null()) { Settings["RC_KEY"].get_to(rightClickTarget.Key); }
		if (!Settings["RC_ALT"].is_null()) { Settings["RC_ALT"].get_to(rightClickTarget.Alt); }
		if (!Settings["RC_CTRL"].is_null()) { Settings["RC_CTRL"].get_to(rightClickTarget.Ctrl); }
		if (!Settings["RC_SHIFT"].is_null()) { Settings["RC_SHIFT"].get_to(rightClickTarget.Shift); }
	}
}
void SaveSettings(std::filesystem::path aPath)
{
	Settings["DAC_KEY"] = disableActionCam.Key;
	Settings["DAC_ALT"] = disableActionCam.Alt;
	Settings["DAC_CTRL"] = disableActionCam.Ctrl;
	Settings["DAC_SHIFT"] = disableActionCam.Shift;

	Settings["RESET_CURSOR_CENTER"] = resetCursorToCenter;
	Settings["ENABLE_DURING_COMBAT"] = enableInCombat;
	Settings["ENABLE_ON_MOUNT"] = enableOnMount;

	Settings["REDIRECT_LEFTCLICK"] = redirectLeftClick;
	Settings["LC_KEY"] = leftClickTarget.Key;
	Settings["LC_ALT"] = leftClickTarget.Alt;
	Settings["LC_CTRL"] = leftClickTarget.Ctrl;
	Settings["LC_SHIFT"] = leftClickTarget.Shift;

	Settings["REDIRECT_RIGHTCLICK"] = redirectRightClick;
	Settings["RC_KEY"] = rightClickTarget.Key;
	Settings["RC_ALT"] = rightClickTarget.Alt;
	Settings["RC_CTRL"] = rightClickTarget.Ctrl;
	Settings["RC_SHIFT"] = rightClickTarget.Shift;

	Mutex.lock();
	{
		std::ofstream file(aPath);
		file << Settings.dump(1, '\t') << std::endl;
		file.close();
	}
	Mutex.unlock();
}