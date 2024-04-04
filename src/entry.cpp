#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "nexus/Nexus.h"
#include "mumble/Mumble.h"
#include "imgui/imgui.h"

#include "Remote.h"
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

std::string KeybindToString(Keybind& keybind, bool padded)
{
	if (keybind == Keybind{}) { return "(null)"; }

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
bool wasDisabling = false;
bool wasMapOpen = false;
bool wasGameFocused = false;
bool redirectLeftClick = false;
bool redirectRightClick = false;

bool actionCamControlled = false;
bool overridingDisable = false;

enum class ETargetBind
{
	None,
	DisableActionCam,
	LeftClick,
	RightClick,
	OverrideDisable
};

ETargetBind CurrentTargetBind = ETargetBind::None;
Keybind CurrentKeybind{};
Keybind disableActionCam{};
Keybind leftClickTarget{};
Keybind rightClickTarget{};
Keybind overrideDisable{};

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
	AddonDef.UpdateLink = REMOTE_URL;

	return &AddonDef;
}

void AddonLoad(AddonAPI* aApi)
{
	APIDefs = aApi;
	ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
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
	APIDefs->DeregisterWndProc(AddonWndProc);

	APIDefs->DeregisterRender(AddonOptions);
	APIDefs->DeregisterRender(AddonRender);

	MumbleLink = nullptr;
	NexusLink = nullptr;
}

void SendDisableActionCam(bool down)
{
	if (down)
	{
		if (disableActionCam == Keybind{}) { return; }

		if (disableActionCam.Alt)
		{
			PostMessage(Game, WM_SYSKEYDOWN, VK_MENU, GetLPARAM(VK_MENU, true, true));
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

		actionCamControlled = true;
	}
	else
	{
		KeyLParam key{};
		key.TransitionState = true;
		key.ExtendedFlag = (disableActionCam.Key & 0xE000) != 0;
		key.ScanCode = disableActionCam.Key;
		PostMessage(Game, WM_KEYUP, MapVirtualKeyA(disableActionCam.Key, MAPVK_VSC_TO_VK), KMFToLParam(key));

		if (disableActionCam.Alt)
		{
			PostMessage(Game, WM_SYSKEYUP, VK_MENU, GetLPARAM(VK_MENU, false, true));
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

		actionCamControlled = false;
	}
}

UINT AddonWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	Game = hWnd;

	if (true == actionCamControlled && false == overridingDisable)
	{
		if (true == redirectLeftClick)
		{
			if (uMsg == WM_LBUTTONDOWN)
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
			else if (uMsg == WM_LBUTTONUP)
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
		}

		if (true == redirectRightClick)
		{
			if (uMsg == WM_RBUTTONDOWN)
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
			else if (uMsg == WM_RBUTTONUP)
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
		}
	}

	if (WM_KEYDOWN == uMsg || WM_SYSKEYDOWN == uMsg)
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
			if (wParam == 16) { kb.Shift = true; }
			if (wParam == 17) { kb.Ctrl = true; }
			if (wParam == 18) { kb.Alt = true; }
		}

		if (kb == overrideDisable)
		{
			overridingDisable = true;
			SendDisableActionCam(false);
			return 0;
		}

		if (isSettingKeybind)
		{
			CurrentKeybind = kb;
		}
	}
	else if (WM_KEYUP == uMsg || WM_SYSKEYUP == uMsg)
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
			if (wParam == 16) { kb.Shift = true; }
			if (wParam == 17) { kb.Ctrl = true; }
			if (wParam == 18) { kb.Alt = true; }
		}

		if (kb == overrideDisable)
		{
			overridingDisable = false;
			return 0;
		}
	}

	if (isSettingKeybind && (uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYUP || uMsg == WM_KEYUP))
	{
		return 0;
	}

	return uMsg;
}

void AddonRender()
{
	if (nullptr == NexusLink || nullptr == MumbleLink) { return; }

	bool isMoving = NexusLink->IsMoving;
	bool isInCombat = MumbleLink->Context.IsInCombat;
	bool isMounted = Mumble::EMountIndex::None != MumbleLink->Context.MountIndex;
	bool isMapOpen = MumbleLink->Context.IsMapOpen;
	bool isGameFocused = MumbleLink->Context.IsGameFocused;

	if (isMoving != wasMoving ||
		isInCombat != wasInCombat ||
		isMounted != wasMounted ||
		isMapOpen != wasMapOpen ||
		isGameFocused != wasGameFocused ||
		overridingDisable != wasDisabling)
	{
		bool shouldControlCamera = (isMoving || (enableInCombat && isInCombat) || (enableOnMount && isMounted)) && !isMapOpen && !overridingDisable;

		if (shouldControlCamera && !actionCamControlled)
		{
			SendDisableActionCam(true);
		}
		else if (!shouldControlCamera && actionCamControlled)
		{
			SendDisableActionCam(false);
		}
	}

	wasMoving = isMoving;
	wasInCombat = isInCombat;
	wasMounted = isMounted;
	wasDisabling = overridingDisable;
	wasMapOpen = isMapOpen;
	wasGameFocused = isGameFocused;

	if (resetCursorToCenter)
	{
		if (actionCamControlled && !overridingDisable && isGameFocused && !isMapOpen)
		{
			RECT rect{};
			GetWindowRect(Game, &rect);

			SetCursorPos((rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2);
		}
	}

	if (!isGameFocused)
	{
		overridingDisable = false;
		actionCamControlled = false;
	}
}

void AddonOptions()
{
	ImGui::TextDisabled("MouseLookHandler");

	ImGui::Text("Disable Action Cam:");
	ImGui::SameLine();
	if (ImGui::Button((KeybindToString(disableActionCam, true) + "##DisableActionCam").c_str()))
	{
		CurrentTargetBind = ETargetBind::DisableActionCam;
		ImGui::OpenPopup("Set Keybind: MouseLookHandler", ImGuiPopupFlags_AnyPopupLevel);
	}
	ImGui::TooltipGeneric("This should match whatever keybind you're using in-game for \"Disable Action Cam\".\nAvoid using keybinds with modifiers such as Alt, Ctrl and Shift as those will be permanently \"pressed\" while moving.\nUse a key that you don't use at all and can't easily reach.");

	if (ImGui::Checkbox("Reset Cursor to Center after Action Cam", &resetCursorToCenter))
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

	ImGui::Text("Hold to temporarily disable Action Cam:");
	ImGui::SameLine();
	if (ImGui::Button((KeybindToString(overrideDisable, true) + "##OverrideDisable").c_str()))
	{
		CurrentTargetBind = ETargetBind::OverrideDisable;
		ImGui::OpenPopup("Set Keybind: MouseLookHandler", ImGuiPopupFlags_AnyPopupLevel);
	}

	ImGui::Checkbox("Redirect Left-Click while moving", &redirectLeftClick);
	if (redirectLeftClick)
	{
		ImGui::Text("Left-Click Action:");
		ImGui::SameLine();
		if (ImGui::Button((KeybindToString(leftClickTarget, true) + "##LeftClickTarget").c_str()))
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
		if (ImGui::Button((KeybindToString(rightClickTarget, true) + "##RightClickTarget").c_str()))
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
			case ETargetBind::OverrideDisable:
				ImGui::Text(KeybindToString(overrideDisable, true).c_str());
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
			case ETargetBind::OverrideDisable:
				overrideDisable = {};
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
			case ETargetBind::OverrideDisable:
				overrideDisable = CurrentKeybind;
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
			APIDefs->Log(ELogLevel_WARNING, "MouseLookHandler", "Settings.json could not be parsed.");
			APIDefs->Log(ELogLevel_WARNING, "MouseLookHandler", ex.what());
		}
	}
	Mutex.unlock();

	if (!Settings.is_null())
	{
		if (!Settings["DAC_KEY"].is_null()) { Settings["DAC_KEY"].get_to(disableActionCam.Key); }
		if (!Settings["DAC_ALT"].is_null()) { Settings["DAC_ALT"].get_to(disableActionCam.Alt); }
		if (!Settings["DAC_CTRL"].is_null()) { Settings["DAC_CTRL"].get_to(disableActionCam.Ctrl); }
		if (!Settings["DAC_SHIFT"].is_null()) { Settings["DAC_SHIFT"].get_to(disableActionCam.Shift); }

		if (!Settings["OD_KEY"].is_null()) { Settings["OD_KEY"].get_to(overrideDisable.Key); }
		if (!Settings["OD_ALT"].is_null()) { Settings["OD_ALT"].get_to(overrideDisable.Alt); }
		if (!Settings["OD_CTRL"].is_null()) { Settings["OD_CTRL"].get_to(overrideDisable.Ctrl); }
		if (!Settings["OD_SHIFT"].is_null()) { Settings["OD_SHIFT"].get_to(overrideDisable.Shift); }

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

	Settings["OD_KEY"] = overrideDisable.Key;
	Settings["OD_ALT"] = overrideDisable.Alt;
	Settings["OD_CTRL"] = overrideDisable.Ctrl;
	Settings["OD_SHIFT"] = overrideDisable.Shift;

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