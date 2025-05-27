///----------------------------------------------------------------------------------------------------
/// Copyright (c) Raidcore.GG - All rights reserved.
///
/// Name         :  Addon.cpp
/// Description  :  Addon entry point and implementation.
/// Authors      :  K. Bieniek
///----------------------------------------------------------------------------------------------------

#include <windows.h>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <dxgi.h>

#include "imgui/imgui.h"
#include "imgui_extensions.h"

#include "nlohmann/json.hpp"
using json = nlohmann::json;

#include "nexus/Nexus.h"
#include "mumble/Mumble.h"
#include "RTAPI/RTAPI.hpp"
#include "Version.h"
#include "Remote.h"
#include "Util/src/Strings.h"
#include "Util/src/Inputs.h"

#define ADDON_NAME "MouseLookHandler"

/* proto */
namespace Addon
{
	void Load(AddonAPI* aApi);
	void Unload();
	UINT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	void PreRender();
	void RenderOptions();
	void LoadSettings();
	void SaveSettings();
}

extern "C" __declspec(dllexport) AddonDefinition* GetAddonDef()
{
	static AddonDefinition s_AddonDef{
		0x09872345,
		NEXUS_API_VERSION,
		ADDON_NAME,
		AddonVersion {
			V_MAJOR,
			V_MINOR,
			V_BUILD,
			V_REVISION
		},
		"Raidcore",
		"Automatically toggles action cam while moving.",
		Addon::Load,
		Addon::Unload,
		EAddonFlags_None,

		EUpdateProvider_GitHub,
		REMOTE_URL
	};

	return &s_AddonDef;
}

namespace Config
{
	bool       ResetToCenter      = false;
	bool       EnableWhileMoving  = true;
	bool       EnableInCombat     = false;
	bool       EnableOnMount      = false;

	bool       RedirectLMB        = false;
	EGameBinds RedirectLMB_Target = (EGameBinds)0;

	bool       RedirectRMB        = false;
	EGameBinds RedirectRMB_Target = (EGameBinds)0;
}

namespace Addon
{
	static std::mutex           s_Mutex; /* For settings. */
	static AddonAPI*            s_APIDefs      = nullptr;

	static NexusLinkData*       s_NexusLink    = nullptr;
	static Mumble::Data*        s_MumbleLink   = nullptr;
	static RTAPI::RealTimeData* s_RTAPI        = nullptr;

	static HWND                 s_WindowHandle = nullptr;

	void Load(AddonAPI* aApi)
	{
		s_APIDefs = aApi;
		ImGui::SetCurrentContext((ImGuiContext*)s_APIDefs->ImguiContext);
		ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))s_APIDefs->ImguiMalloc, (void(*)(void*, void*))s_APIDefs->ImguiFree); // on imgui 1.80+

		s_NexusLink = (NexusLinkData*)s_APIDefs->DataLink.Get("DL_NEXUS_LINK");
		s_MumbleLink = (Mumble::Data*)s_APIDefs->DataLink.Get("DL_MUMBLE_LINK");

		s_APIDefs->Renderer.Register(ERenderType_PreRender, PreRender);
		s_APIDefs->Renderer.Register(ERenderType_OptionsRender, RenderOptions);

		s_APIDefs->WndProc.Register(WndProc);

		IDXGISwapChain* swapchain = (IDXGISwapChain*)s_APIDefs->SwapChain;
		DXGI_SWAP_CHAIN_DESC desc{};
		swapchain->GetDesc(&desc);
		s_WindowHandle = desc.OutputWindow;

		LoadSettings();
	}

	void Unload()
	{
		s_APIDefs->WndProc.Deregister(WndProc);

		s_APIDefs->Renderer.Deregister(PreRender);
		s_APIDefs->Renderer.Deregister(RenderOptions);
	}

	UINT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		//                      ui is ticking           && cursor not visible
		bool cursorControlled = s_NexusLink->IsGameplay && Inputs::IsCursorHidden();

		//                      rtapi   && action cam is active    && during gameplay
		bool rtapiActionCam   = s_RTAPI && s_RTAPI->IsActionCamera && s_RTAPI->GameState == RTAPI::EGameState::Gameplay;

		if (!(cursorControlled || (rtapiActionCam)))
		{
			return 1;
		}

		if (Config::RedirectLMB)
		{
			switch (uMsg)
			{
				case WM_LBUTTONDOWN:
				{
					s_APIDefs->GameBinds.Press(Config::RedirectLMB_Target);
					return 0;
				}
				case WM_LBUTTONUP:
				{
					s_APIDefs->GameBinds.Release(Config::RedirectLMB_Target);

					/* Releases should always be passed on. */
					return 1;
				}
			}
		}

		if (Config::RedirectRMB)
		{
			switch (uMsg)
			{
				case WM_RBUTTONDOWN:
				{
					s_APIDefs->GameBinds.Press(Config::RedirectRMB_Target);
					return 0;
				}
				case WM_RBUTTONUP:
				{
					s_APIDefs->GameBinds.Release(Config::RedirectRMB_Target);

					/* Releases should always be passed on. */
					return 1;
				}
			}
		}

		return 1;
	}

	void PreRender()
	{
		static bool s_WasActive = false;
		static bool s_ResetCursor = false;

		/* Do not evaluate state changes while map is open. */
		if (s_MumbleLink->Context.IsMapOpen)
		{
			return;
		}

		bool shouldActivate = false;

		if (s_ResetCursor && !Inputs::IsCursorHidden())
		{
			RECT rect{};
			GetWindowRect(s_WindowHandle, &rect);
			SetCursorPos((rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2);
			s_ResetCursor = false;
		}

		if (Config::EnableWhileMoving && s_NexusLink->IsMoving)
		{
			shouldActivate = true;
		}
		if (Config::EnableInCombat && s_MumbleLink->Context.IsInCombat)
		{
			shouldActivate = true;
		}
		if (Config::EnableOnMount && s_MumbleLink->Context.MountIndex != Mumble::EMountIndex::None)
		{
			shouldActivate = true;
		}

		if (shouldActivate != s_WasActive)
		{
			s_APIDefs->GameBinds.InvokeAsync(EGameBinds_CameraActionMode, 0);

			if (!shouldActivate && Config::ResetToCenter)
			{
				s_ResetCursor = true;
			}
		}

		s_WasActive = shouldActivate;
	}

	void GbSelectable(EGameBinds* aTarget, const char* aLabel, EGameBinds aGameBind)
	{
		bool isBound = s_APIDefs->GameBinds.IsBound(aGameBind);

		if (!isBound)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(255, 255, 0, 255));
		}

		if (ImGui::Selectable((s_APIDefs->Localization.Translate(aLabel) + ("##" + std::to_string(aGameBind))).c_str()))
		{
			*aTarget = aGameBind;
			SaveSettings();
		}

		if (!isBound)
		{
			ImGui::PopStyleColor();
			ImGui::TooltipGeneric("Bind this Game InputBind via the Nexus options in order to be able to use it.\nIt must match the game.");
		}
	}

	std::string GameBindToString(EGameBinds aGameBind)
	{
		static std::map<EGameBinds, std::string> s_LUT =
		{
			// Movement
			{ EGameBinds_MoveForward, "((MoveForward))" },
			{ EGameBinds_MoveBackward, "((MoveBackward))" },
			{ EGameBinds_MoveLeft, "((MoveLeft))" },
			{ EGameBinds_MoveRight, "((MoveRight))" },
			{ EGameBinds_MoveTurnLeft, "((MoveTurnLeft))" },
			{ EGameBinds_MoveTurnRight, "((MoveTurnRight))" },
			{ EGameBinds_MoveDodge, "((MoveDodge))" },
			{ EGameBinds_MoveAutoRun, "((MoveAutoRun))" },
			{ EGameBinds_MoveWalk, "((MoveWalk))" },
			{ EGameBinds_MoveJump_SwimUp_FlyUp, "((MoveJump))"},
			{ EGameBinds_MoveSwimDown_FlyDown, "((MoveSwimDown))" },
			{ EGameBinds_MoveAboutFace, "((MoveAboutFace))" },

			// Skills
			{ EGameBinds_SkillWeaponSwap, "((SkillWeaponSwap))" },
			{ EGameBinds_SkillWeapon1, "((SkillWeapon1))" },
			{ EGameBinds_SkillWeapon2, "((SkillWeapon2))" },
			{ EGameBinds_SkillWeapon3, "((SkillWeapon3))" },
			{ EGameBinds_SkillWeapon4, "((SkillWeapon4))" },
			{ EGameBinds_SkillWeapon5, "((SkillWeapon5))" },
			{ EGameBinds_SkillHeal, "((SkillHeal))" },
			{ EGameBinds_SkillUtility1, "((SkillUtility1))" },
			{ EGameBinds_SkillUtility2, "((SkillUtility2))" },
			{ EGameBinds_SkillUtility3, "((SkillUtility3))" },
			{ EGameBinds_SkillElite, "((SkillElite))" },
			{ EGameBinds_SkillProfession1, "((SkillProfession1))" },
			{ EGameBinds_SkillProfession2, "((SkillProfession2))" },
			{ EGameBinds_SkillProfession3, "((SkillProfession3))" },
			{ EGameBinds_SkillProfession4, "((SkillProfession4))" },
			{ EGameBinds_SkillProfession5, "((SkillProfession5))" },
			{ EGameBinds_SkillProfession6, "((SkillProfession6))" },
			{ EGameBinds_SkillProfession7, "((SkillProfession7))" },
			{ EGameBinds_SkillSpecialAction, "((SkillSpecialAction))" },

			// Targeting
			{ EGameBinds_TargetAlert, "((TargetAlert))" },
			{ EGameBinds_TargetCall, "((TargetCall))" },
			{ EGameBinds_TargetTake, "((TargetTake))" },
			{ EGameBinds_TargetCallLocal, "((TargetCallLocal))" },
			{ EGameBinds_TargetTakeLocal, "((TargetTakeLocal))" },
			{ EGameBinds_TargetEnemyNearest, "((TargetEnemyNearest))" },
			{ EGameBinds_TargetEnemyNext, "((TargetEnemyNext))" },
			{ EGameBinds_TargetEnemyPrev, "((TargetEnemyPrev))" },
			{ EGameBinds_TargetAllyNearest, "((TargetAllyNearest))" },
			{ EGameBinds_TargetAllyNext, "((TargetAllyNext))" },
			{ EGameBinds_TargetAllyPrev, "((TargetAllyPrev))" },
			{ EGameBinds_TargetLock, "((TargetLock))" },
			{ EGameBinds_TargetSnapGroundTarget, "((TargetSnapGroundTarget))" },
			{ EGameBinds_TargetSnapGroundTargetToggle, "((TargetSnapGroundTargetToggle))" },
			{ EGameBinds_TargetAutoTargetingDisable, "((TargetAutoTargetingDisable))" },
			{ EGameBinds_TargetAutoTargetingToggle, "((TargetAutoTargetingToggle))" },
			{ EGameBinds_TargetAllyTargetingMode, "((TargetAllyTargetingMode))" },
			{ EGameBinds_TargetAllyTargetingModeToggle, "((TargetAllyTargetingModeToggle))" },

			// UI Binds
			{ EGameBinds_UiCommerce, "((UiCommerce))" }, // TradingPost
			{ EGameBinds_UiContacts, "((UiContacts))" },
			{ EGameBinds_UiGuild, "((UiGuild))" },
			{ EGameBinds_UiHero, "((UiHero))" },
			{ EGameBinds_UiInventory, "((UiInventory))" },
			{ EGameBinds_UiKennel, "((UiKennel))" }, // Pets
			{ EGameBinds_UiLogout, "((UiLogout))" },
			{ EGameBinds_UiMail, "((UiMail))" },
			{ EGameBinds_UiOptions, "((UiOptions))" },
			{ EGameBinds_UiParty, "((UiParty))" },
			{ EGameBinds_UiPvp, "((UiPvp))" },
			{ EGameBinds_UiPvpBuild, "((UiPvpBuild))" },
			{ EGameBinds_UiScoreboard, "((UiScoreboard))" },
			{ EGameBinds_UiSeasonalObjectivesShop, "((UiSeasonalObjectivesShop))" }, // Wizard's Vault
			{ EGameBinds_UiInformation, "((UiInformation))" },
			{ EGameBinds_UiChatToggle, "((UiChatToggle))" },
			{ EGameBinds_UiChatCommand, "((UiChatCommand))" },
			{ EGameBinds_UiChatFocus, "((UiChatFocus))" },
			{ EGameBinds_UiChatReply, "((UiChatReply))" },
			{ EGameBinds_UiToggle, "((UiToggle))" },
			{ EGameBinds_UiSquadBroadcastChatToggle, "((UiSquadBroadcastChatToggle))" },
			{ EGameBinds_UiSquadBroadcastChatCommand, "((UiSquadBroadcastChatCommand))" },
			{ EGameBinds_UiSquadBroadcastChatFocus, "((UiSquadBroadcastChatFocus))" },

			// Camera
			{ EGameBinds_CameraFree, "((CameraFree))" },
			{ EGameBinds_CameraZoomIn, "((CameraZoomIn))" },
			{ EGameBinds_CameraZoomOut, "((CameraZoomOut))" },
			{ EGameBinds_CameraReverse, "((CameraReverse))" },
			{ EGameBinds_CameraActionMode, "((CameraActionMode))" },
			{ EGameBinds_CameraActionModeDisable, "((CameraActionModeDisable))" },

			// Screenshots
			{ EGameBinds_ScreenshotNormal, "((ScreenshotNormal))" },
			{ EGameBinds_ScreenshotStereoscopic, "((ScreenshotStereoscopic))" },

			// Map
			{ EGameBinds_MapToggle, "((MapToggle))" },
			{ EGameBinds_MapFocusPlayer, "((MapFocusPlayer))" },
			{ EGameBinds_MapFloorDown, "((MapFloorDown))" },
			{ EGameBinds_MapFloorUp, "((MapFloorUp))" },
			{ EGameBinds_MapZoomIn, "((MapZoomIn))" },
			{ EGameBinds_MapZoomOut, "((MapZoomOut))" },

			// Mounts
			{ EGameBinds_SpumoniToggle, "((SpumoniToggle))" },
			{ EGameBinds_SpumoniMovement, "((SpumoniMovement))" },
			{ EGameBinds_SpumoniSecondaryMovement, "((SpumoniSecondaryMovement))" },
			{ EGameBinds_SpumoniMAM01, "((SpumoniMAM01))" }, // Raptor
			{ EGameBinds_SpumoniMAM02, "((SpumoniMAM02))" }, // Springer
			{ EGameBinds_SpumoniMAM03, "((SpumoniMAM03))" }, // Skimmer
			{ EGameBinds_SpumoniMAM04, "((SpumoniMAM04))" }, // Jackal
			{ EGameBinds_SpumoniMAM05, "((SpumoniMAM05))" }, // Griffon
			{ EGameBinds_SpumoniMAM06, "((SpumoniMAM06))" }, // RollerBeetle
			{ EGameBinds_SpumoniMAM07, "((SpumoniMAM07))" }, // Warclaw
			{ EGameBinds_SpumoniMAM08, "((SpumoniMAM08))" }, // Skyscale
			{ EGameBinds_SpumoniMAM09, "((SpumoniMAM09))" }, // SiegeTurtle

			// Spectator Binds
			{ EGameBinds_SpectatorNearestFixed, "((SpectatorNearestFixed))" },
			{ EGameBinds_SpectatorNearestPlayer, "((SpectatorNearestPlayer))" },
			{ EGameBinds_SpectatorPlayerRed1, "((SpectatorPlayerRed1))" },
			{ EGameBinds_SpectatorPlayerRed2, "((SpectatorPlayerRed2))" },
			{ EGameBinds_SpectatorPlayerRed3, "((SpectatorPlayerRed3))" },
			{ EGameBinds_SpectatorPlayerRed4, "((SpectatorPlayerRed4))" },
			{ EGameBinds_SpectatorPlayerRed5, "((SpectatorPlayerRed5))" },
			{ EGameBinds_SpectatorPlayerBlue1, "((SpectatorPlayerBlue1))" },
			{ EGameBinds_SpectatorPlayerBlue2, "((SpectatorPlayerBlue2))" },
			{ EGameBinds_SpectatorPlayerBlue3, "((SpectatorPlayerBlue3))" },
			{ EGameBinds_SpectatorPlayerBlue4, "((SpectatorPlayerBlue4))" },
			{ EGameBinds_SpectatorPlayerBlue5, "((SpectatorPlayerBlue5))" },
			{ EGameBinds_SpectatorFreeCamera, "((SpectatorFreeCamera))" },
			{ EGameBinds_SpectatorFreeCameraMode, "((SpectatorFreeCameraMode))" },
			{ EGameBinds_SpectatorFreeMoveForward, "((SpectatorFreeMoveForward))" },
			{ EGameBinds_SpectatorFreeMoveBackward, "((SpectatorFreeMoveBackward))" },
			{ EGameBinds_SpectatorFreeMoveLeft, "((SpectatorFreeMoveLeft))" },
			{ EGameBinds_SpectatorFreeMoveRight, "((SpectatorFreeMoveRight))" },
			{ EGameBinds_SpectatorFreeMoveUp, "((SpectatorFreeMoveUp))" },
			{ EGameBinds_SpectatorFreeMoveDown, "((SpectatorFreeMoveDown))" },

			// Squad Markers
			{ EGameBinds_SquadMarkerPlaceWorld1, "((SquadMarkerPlaceWorld1))" }, // Arrow
			{ EGameBinds_SquadMarkerPlaceWorld2, "((SquadMarkerPlaceWorld2))" }, // Circle
			{ EGameBinds_SquadMarkerPlaceWorld3, "((SquadMarkerPlaceWorld3))" }, // Heart
			{ EGameBinds_SquadMarkerPlaceWorld4, "((SquadMarkerPlaceWorld4))" }, // Square
			{ EGameBinds_SquadMarkerPlaceWorld5, "((SquadMarkerPlaceWorld5))" }, // Star
			{ EGameBinds_SquadMarkerPlaceWorld6, "((SquadMarkerPlaceWorld6))" }, // Swirl
			{ EGameBinds_SquadMarkerPlaceWorld7, "((SquadMarkerPlaceWorld7))" }, // Triangle
			{ EGameBinds_SquadMarkerPlaceWorld8, "((SquadMarkerPlaceWorld8))" }, // Cross
			{ EGameBinds_SquadMarkerClearAllWorld, "((SquadMarkerClearAllWorld))" },
			{ EGameBinds_SquadMarkerSetAgent1, "((SquadMarkerSetAgent1))" }, // Arrow
			{ EGameBinds_SquadMarkerSetAgent2, "((SquadMarkerSetAgent2))" }, // Circle
			{ EGameBinds_SquadMarkerSetAgent3, "((SquadMarkerSetAgent3))" }, // Heart
			{ EGameBinds_SquadMarkerSetAgent4, "((SquadMarkerSetAgent4))" }, // Square
			{ EGameBinds_SquadMarkerSetAgent5, "((SquadMarkerSetAgent5))" }, // Star
			{ EGameBinds_SquadMarkerSetAgent6, "((SquadMarkerSetAgent6))" }, // Swirl
			{ EGameBinds_SquadMarkerSetAgent7, "((SquadMarkerSetAgent7))" }, // Triangle
			{ EGameBinds_SquadMarkerSetAgent8, "((SquadMarkerSetAgent8))" }, // Cross
			{ EGameBinds_SquadMarkerClearAllAgent, "((SquadMarkerClearAllAgent))" },

			// Mastery Skills
			{ EGameBinds_MasteryAccess, "((MasteryAccess))" },
			{ EGameBinds_MasteryAccess01, "((MasteryAccess01))" }, // Fishing
			{ EGameBinds_MasteryAccess02, "((MasteryAccess02))" }, // Skiff
			{ EGameBinds_MasteryAccess03, "((MasteryAccess03))" }, // Jade Bot Waypoint
			{ EGameBinds_MasteryAccess04, "((MasteryAccess04))" }, // Rift Scan
			{ EGameBinds_MasteryAccess05, "((MasteryAccess05))" }, // Skyscale
			{ EGameBinds_MasteryAccess06, "((MasteryAccess06))" }, // Homestead Doorway

			// Miscellaneous Binds
			{ EGameBinds_MiscAoELoot, "((MiscAoELoot))" },
			{ EGameBinds_MiscInteract, "((MiscInteract))" },
			{ EGameBinds_MiscShowEnemies, "((MiscShowEnemies))" },
			{ EGameBinds_MiscShowAllies, "((MiscShowAllies))" },
			{ EGameBinds_MiscCombatStance, "((MiscCombatStance))" }, // Stow/Draw
			{ EGameBinds_MiscToggleLanguage, "((MiscToggleLanguage))" },
			{ EGameBinds_MiscTogglePetCombat, "((MiscTogglePetCombat))" },
			{ EGameBinds_MiscToggleFullScreen, "((MiscToggleFullScreen))" },
			{ EGameBinds_MiscToggleDecorationMode, "((MiscToggleDecorationMode))" }, // Decoration Mode

			// Toys/Novelties
			{ EGameBinds_ToyUseDefault, "((ToyUseDefault))" },
			{ EGameBinds_ToyUseSlot1, "((ToyUseSlot1))" }, // Chair
			{ EGameBinds_ToyUseSlot2, "((ToyUseSlot2))" }, // Instrument
			{ EGameBinds_ToyUseSlot3, "((ToyUseSlot3))" }, // Held Item
			{ EGameBinds_ToyUseSlot4, "((ToyUseSlot4))" }, // Toy
			{ EGameBinds_ToyUseSlot5, "((ToyUseSlot5))" }, // Tonic
			//ToyUseSlot6 unused

			// Build Templates
			{ EGameBinds_Loadout1, "((Loadout1))" },
			{ EGameBinds_Loadout2, "((Loadout2))" },
			{ EGameBinds_Loadout3, "((Loadout3))" },
			{ EGameBinds_Loadout4, "((Loadout4))" },
			{ EGameBinds_Loadout5, "((Loadout5))" },
			{ EGameBinds_Loadout6, "((Loadout6))" },
			{ EGameBinds_Loadout7, "((Loadout7))" },
			{ EGameBinds_Loadout8, "((Loadout8))" },
			{ EGameBinds_Loadout9, "((Loadout9))" },

			// Equipment Templates
			{ EGameBinds_GearLoadout1, "((GearLoadout1))" },
			{ EGameBinds_GearLoadout2, "((GearLoadout2))" },
			{ EGameBinds_GearLoadout3, "((GearLoadout3))" },
			{ EGameBinds_GearLoadout4, "((GearLoadout4))" },
			{ EGameBinds_GearLoadout5, "((GearLoadout5))" },
			{ EGameBinds_GearLoadout6, "((GearLoadout6))" },
			{ EGameBinds_GearLoadout7, "((GearLoadout7))" },
			{ EGameBinds_GearLoadout8, "((GearLoadout8))" },
			{ EGameBinds_GearLoadout9, "((GearLoadout9))" }
		};

		return s_LUT[aGameBind];
	}

	void GbSelector(const char* aIdentifier, EGameBinds* aTarget)
	{
		if (ImGui::BeginCombo(aIdentifier, s_APIDefs->Localization.Translate(GameBindToString(*aTarget).c_str())))
		{
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Movement))")))
			{
				GbSelectable(aTarget, "((MoveForward))", EGameBinds_MoveForward);
				GbSelectable(aTarget, "((MoveBackward))", EGameBinds_MoveBackward);
				GbSelectable(aTarget, "((MoveLeft))", EGameBinds_MoveLeft);
				GbSelectable(aTarget, "((MoveRight))", EGameBinds_MoveRight);
				GbSelectable(aTarget, "((MoveTurnLeft))", EGameBinds_MoveTurnLeft);
				GbSelectable(aTarget, "((MoveTurnRight))", EGameBinds_MoveTurnRight);
				GbSelectable(aTarget, "((MoveDodge))", EGameBinds_MoveDodge);
				GbSelectable(aTarget, "((MoveAutoRun))", EGameBinds_MoveAutoRun);
				GbSelectable(aTarget, "((MoveWalk))", EGameBinds_MoveWalk);
				GbSelectable(aTarget, "((MoveJump))", EGameBinds_MoveJump_SwimUp_FlyUp);
				GbSelectable(aTarget, "((MoveSwimDown))", EGameBinds_MoveSwimDown_FlyDown);
				GbSelectable(aTarget, "((MoveAboutFace))", EGameBinds_MoveAboutFace);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Skills))")))
			{
				GbSelectable(aTarget, "((SkillWeaponSwap))", EGameBinds_SkillWeaponSwap);
				GbSelectable(aTarget, "((SkillWeapon1))", EGameBinds_SkillWeapon1);
				GbSelectable(aTarget, "((SkillWeapon2))", EGameBinds_SkillWeapon2);
				GbSelectable(aTarget, "((SkillWeapon3))", EGameBinds_SkillWeapon3);
				GbSelectable(aTarget, "((SkillWeapon4))", EGameBinds_SkillWeapon4);
				GbSelectable(aTarget, "((SkillWeapon5))", EGameBinds_SkillWeapon5);
				GbSelectable(aTarget, "((SkillHeal))", EGameBinds_SkillHeal);
				GbSelectable(aTarget, "((SkillUtility1))", EGameBinds_SkillUtility1);
				GbSelectable(aTarget, "((SkillUtility2))", EGameBinds_SkillUtility2);
				GbSelectable(aTarget, "((SkillUtility3))", EGameBinds_SkillUtility3);
				GbSelectable(aTarget, "((SkillElite))", EGameBinds_SkillElite);
				GbSelectable(aTarget, "((SkillProfession1))", EGameBinds_SkillProfession1);
				GbSelectable(aTarget, "((SkillProfession2))", EGameBinds_SkillProfession2);
				GbSelectable(aTarget, "((SkillProfession3))", EGameBinds_SkillProfession3);
				GbSelectable(aTarget, "((SkillProfession4))", EGameBinds_SkillProfession4);
				GbSelectable(aTarget, "((SkillProfession5))", EGameBinds_SkillProfession5);
				GbSelectable(aTarget, "((SkillProfession6))", EGameBinds_SkillProfession6);
				GbSelectable(aTarget, "((SkillProfession7))", EGameBinds_SkillProfession7);
				GbSelectable(aTarget, "((SkillSpecialAction))", EGameBinds_SkillSpecialAction);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Targeting))")))
			{
				GbSelectable(aTarget, "((TargetAlert))", EGameBinds_TargetAlert);
				GbSelectable(aTarget, "((TargetCall))", EGameBinds_TargetCall);
				GbSelectable(aTarget, "((TargetTake))", EGameBinds_TargetTake);
				GbSelectable(aTarget, "((TargetCallLocal))", EGameBinds_TargetCallLocal);
				GbSelectable(aTarget, "((TargetTakeLocal))", EGameBinds_TargetTakeLocal);
				GbSelectable(aTarget, "((TargetEnemyNearest))", EGameBinds_TargetEnemyNearest);
				GbSelectable(aTarget, "((TargetEnemyNext))", EGameBinds_TargetEnemyNext);
				GbSelectable(aTarget, "((TargetEnemyPrev))", EGameBinds_TargetEnemyPrev);
				GbSelectable(aTarget, "((TargetAllyNearest))", EGameBinds_TargetAllyNearest);
				GbSelectable(aTarget, "((TargetAllyNext))", EGameBinds_TargetAllyNext);
				GbSelectable(aTarget, "((TargetAllyPrev))", EGameBinds_TargetAllyPrev);
				GbSelectable(aTarget, "((TargetLock))", EGameBinds_TargetLock);
				GbSelectable(aTarget, "((TargetSnapGroundTarget))", EGameBinds_TargetSnapGroundTarget);
				GbSelectable(aTarget, "((TargetSnapGroundTargetToggle))", EGameBinds_TargetSnapGroundTargetToggle);
				GbSelectable(aTarget, "((TargetAutoTargetingDisable))", EGameBinds_TargetAutoTargetingDisable);
				GbSelectable(aTarget, "((TargetAutoTargetingToggle))", EGameBinds_TargetAutoTargetingToggle);
				GbSelectable(aTarget, "((TargetAllyTargetingMode))", EGameBinds_TargetAllyTargetingMode);
				GbSelectable(aTarget, "((TargetAllyTargetingModeToggle))", EGameBinds_TargetAllyTargetingModeToggle);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((User Interface))")))
			{
				GbSelectable(aTarget, "((UiCommerce))", EGameBinds_UiCommerce);
				GbSelectable(aTarget, "((UiContacts))", EGameBinds_UiContacts);
				GbSelectable(aTarget, "((UiGuild))", EGameBinds_UiGuild);
				GbSelectable(aTarget, "((UiHero))", EGameBinds_UiHero);
				GbSelectable(aTarget, "((UiInventory))", EGameBinds_UiInventory);
				GbSelectable(aTarget, "((UiKennel))", EGameBinds_UiKennel);
				GbSelectable(aTarget, "((UiLogout))", EGameBinds_UiLogout);
				GbSelectable(aTarget, "((UiMail))", EGameBinds_UiMail);
				GbSelectable(aTarget, "((UiOptions))", EGameBinds_UiOptions);
				GbSelectable(aTarget, "((UiParty))", EGameBinds_UiParty);
				GbSelectable(aTarget, "((UiPvp))", EGameBinds_UiPvp);
				GbSelectable(aTarget, "((UiPvpBuild))", EGameBinds_UiPvpBuild);
				GbSelectable(aTarget, "((UiScoreboard))", EGameBinds_UiScoreboard);
				GbSelectable(aTarget, "((UiSeasonalObjectivesShop))", EGameBinds_UiSeasonalObjectivesShop);
				GbSelectable(aTarget, "((UiInformation))", EGameBinds_UiInformation);
				GbSelectable(aTarget, "((UiChatToggle))", EGameBinds_UiChatToggle);
				GbSelectable(aTarget, "((UiChatCommand))", EGameBinds_UiChatCommand);
				GbSelectable(aTarget, "((UiChatFocus))", EGameBinds_UiChatFocus);
				GbSelectable(aTarget, "((UiChatReply))", EGameBinds_UiChatReply);
				GbSelectable(aTarget, "((UiToggle))", EGameBinds_UiToggle);
				GbSelectable(aTarget, "((UiSquadBroadcastChatToggle))", EGameBinds_UiSquadBroadcastChatToggle);
				GbSelectable(aTarget, "((UiSquadBroadcastChatCommand))", EGameBinds_UiSquadBroadcastChatCommand);
				GbSelectable(aTarget, "((UiSquadBroadcastChatFocus))", EGameBinds_UiSquadBroadcastChatFocus);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Camera))")))
			{
				GbSelectable(aTarget, "((CameraFree))", EGameBinds_CameraFree);
				GbSelectable(aTarget, "((CameraZoomIn))", EGameBinds_CameraZoomIn);
				GbSelectable(aTarget, "((CameraZoomOut))", EGameBinds_CameraZoomOut);
				GbSelectable(aTarget, "((CameraReverse))", EGameBinds_CameraReverse);
				GbSelectable(aTarget, "((CameraActionMode))", EGameBinds_CameraActionMode);
				GbSelectable(aTarget, "((CameraActionModeDisable))", EGameBinds_CameraActionModeDisable);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Screenshot))")))
			{
				GbSelectable(aTarget, "((ScreenshotNormal))", EGameBinds_ScreenshotNormal);
				GbSelectable(aTarget, "((ScreenshotStereoscopic))", EGameBinds_ScreenshotStereoscopic);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Map))")))
			{
				GbSelectable(aTarget, "((MapToggle))", EGameBinds_MapToggle);
				GbSelectable(aTarget, "((MapFocusPlayer))", EGameBinds_MapFocusPlayer);
				GbSelectable(aTarget, "((MapFloorDown))", EGameBinds_MapFloorDown);
				GbSelectable(aTarget, "((MapFloorUp))", EGameBinds_MapFloorUp);
				GbSelectable(aTarget, "((MapZoomIn))", EGameBinds_MapZoomIn);
				GbSelectable(aTarget, "((MapZoomOut))", EGameBinds_MapZoomOut);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Mounts))")))
			{
				GbSelectable(aTarget, "((SpumoniToggle))", EGameBinds_SpumoniToggle);
				GbSelectable(aTarget, "((SpumoniMovement))", EGameBinds_SpumoniMovement);
				GbSelectable(aTarget, "((SpumoniSecondaryMovement))", EGameBinds_SpumoniSecondaryMovement);
				GbSelectable(aTarget, "((SpumoniMAM01))", EGameBinds_SpumoniMAM01);
				GbSelectable(aTarget, "((SpumoniMAM02))", EGameBinds_SpumoniMAM02);
				GbSelectable(aTarget, "((SpumoniMAM03))", EGameBinds_SpumoniMAM03);
				GbSelectable(aTarget, "((SpumoniMAM04))", EGameBinds_SpumoniMAM04);
				GbSelectable(aTarget, "((SpumoniMAM05))", EGameBinds_SpumoniMAM05);
				GbSelectable(aTarget, "((SpumoniMAM06))", EGameBinds_SpumoniMAM06);
				GbSelectable(aTarget, "((SpumoniMAM07))", EGameBinds_SpumoniMAM07);
				GbSelectable(aTarget, "((SpumoniMAM08))", EGameBinds_SpumoniMAM08);
				GbSelectable(aTarget, "((SpumoniMAM09))", EGameBinds_SpumoniMAM09);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Spectators))")))
			{
				GbSelectable(aTarget, "((SpectatorNearestFixed))", EGameBinds_SpectatorNearestFixed);
				GbSelectable(aTarget, "((SpectatorNearestPlayer))", EGameBinds_SpectatorNearestPlayer);
				GbSelectable(aTarget, "((SpectatorPlayerRed1))", EGameBinds_SpectatorPlayerRed1);
				GbSelectable(aTarget, "((SpectatorPlayerRed2))", EGameBinds_SpectatorPlayerRed2);
				GbSelectable(aTarget, "((SpectatorPlayerRed3))", EGameBinds_SpectatorPlayerRed3);
				GbSelectable(aTarget, "((SpectatorPlayerRed4))", EGameBinds_SpectatorPlayerRed4);
				GbSelectable(aTarget, "((SpectatorPlayerRed5))", EGameBinds_SpectatorPlayerRed5);
				GbSelectable(aTarget, "((SpectatorPlayerBlue1))", EGameBinds_SpectatorPlayerBlue1);
				GbSelectable(aTarget, "((SpectatorPlayerBlue2))", EGameBinds_SpectatorPlayerBlue2);
				GbSelectable(aTarget, "((SpectatorPlayerBlue3))", EGameBinds_SpectatorPlayerBlue3);
				GbSelectable(aTarget, "((SpectatorPlayerBlue4))", EGameBinds_SpectatorPlayerBlue4);
				GbSelectable(aTarget, "((SpectatorPlayerBlue5))", EGameBinds_SpectatorPlayerBlue5);
				GbSelectable(aTarget, "((SpectatorFreeCamera))", EGameBinds_SpectatorFreeCamera);
				GbSelectable(aTarget, "((SpectatorFreeCameraMode))", EGameBinds_SpectatorFreeCameraMode);
				GbSelectable(aTarget, "((SpectatorFreeMoveForward))", EGameBinds_SpectatorFreeMoveForward);
				GbSelectable(aTarget, "((SpectatorFreeMoveBackward))", EGameBinds_SpectatorFreeMoveBackward);
				GbSelectable(aTarget, "((SpectatorFreeMoveLeft))", EGameBinds_SpectatorFreeMoveLeft);
				GbSelectable(aTarget, "((SpectatorFreeMoveRight))", EGameBinds_SpectatorFreeMoveRight);
				GbSelectable(aTarget, "((SpectatorFreeMoveUp))", EGameBinds_SpectatorFreeMoveUp);
				GbSelectable(aTarget, "((SpectatorFreeMoveDown))", EGameBinds_SpectatorFreeMoveDown);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Squad))")))
			{
				GbSelectable(aTarget, "((SquadMarkerPlaceWorld1))", EGameBinds_SquadMarkerPlaceWorld1);
				GbSelectable(aTarget, "((SquadMarkerPlaceWorld2))", EGameBinds_SquadMarkerPlaceWorld2);
				GbSelectable(aTarget, "((SquadMarkerPlaceWorld3))", EGameBinds_SquadMarkerPlaceWorld3);
				GbSelectable(aTarget, "((SquadMarkerPlaceWorld4))", EGameBinds_SquadMarkerPlaceWorld4);
				GbSelectable(aTarget, "((SquadMarkerPlaceWorld5))", EGameBinds_SquadMarkerPlaceWorld5);
				GbSelectable(aTarget, "((SquadMarkerPlaceWorld6))", EGameBinds_SquadMarkerPlaceWorld6);
				GbSelectable(aTarget, "((SquadMarkerPlaceWorld7))", EGameBinds_SquadMarkerPlaceWorld7);
				GbSelectable(aTarget, "((SquadMarkerPlaceWorld8))", EGameBinds_SquadMarkerPlaceWorld8);
				GbSelectable(aTarget, "((SquadMarkerClearAllWorld))", EGameBinds_SquadMarkerClearAllWorld);
				GbSelectable(aTarget, "((SquadMarkerSetAgent1))", EGameBinds_SquadMarkerSetAgent1);
				GbSelectable(aTarget, "((SquadMarkerSetAgent2))", EGameBinds_SquadMarkerSetAgent2);
				GbSelectable(aTarget, "((SquadMarkerSetAgent3))", EGameBinds_SquadMarkerSetAgent3);
				GbSelectable(aTarget, "((SquadMarkerSetAgent4))", EGameBinds_SquadMarkerSetAgent4);
				GbSelectable(aTarget, "((SquadMarkerSetAgent5))", EGameBinds_SquadMarkerSetAgent5);
				GbSelectable(aTarget, "((SquadMarkerSetAgent6))", EGameBinds_SquadMarkerSetAgent6);
				GbSelectable(aTarget, "((SquadMarkerSetAgent7))", EGameBinds_SquadMarkerSetAgent7);
				GbSelectable(aTarget, "((SquadMarkerSetAgent8))", EGameBinds_SquadMarkerSetAgent8);
				GbSelectable(aTarget, "((SquadMarkerClearAllAgent))", EGameBinds_SquadMarkerClearAllAgent);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Mastery Skills))")))
			{
				GbSelectable(aTarget, "((MasteryAccess))", EGameBinds_MasteryAccess);
				GbSelectable(aTarget, "((MasteryAccess01))", EGameBinds_MasteryAccess01);
				GbSelectable(aTarget, "((MasteryAccess02))", EGameBinds_MasteryAccess02);
				GbSelectable(aTarget, "((MasteryAccess03))", EGameBinds_MasteryAccess03);
				GbSelectable(aTarget, "((MasteryAccess04))", EGameBinds_MasteryAccess04);
				GbSelectable(aTarget, "((MasteryAccess05))", EGameBinds_MasteryAccess05);
				GbSelectable(aTarget, "((MasteryAccess06))", EGameBinds_MasteryAccess06);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Miscellaneous))")))
			{
				GbSelectable(aTarget, "((MiscAoELoot))", EGameBinds_MiscAoELoot);
				GbSelectable(aTarget, "((MiscInteract))", EGameBinds_MiscInteract);
				GbSelectable(aTarget, "((MiscShowEnemies))", EGameBinds_MiscShowEnemies);
				GbSelectable(aTarget, "((MiscShowAllies))", EGameBinds_MiscShowAllies);
				GbSelectable(aTarget, "((MiscCombatStance))", EGameBinds_MiscCombatStance);
				GbSelectable(aTarget, "((MiscToggleLanguage))", EGameBinds_MiscToggleLanguage);
				GbSelectable(aTarget, "((MiscTogglePetCombat))", EGameBinds_MiscTogglePetCombat);
				GbSelectable(aTarget, "((MiscToggleFullScreen))", EGameBinds_MiscToggleFullScreen);
				GbSelectable(aTarget, "((MiscToggleDecorationMode))", EGameBinds_MiscToggleDecorationMode);
				GbSelectable(aTarget, "((ToyUseDefault))", EGameBinds_ToyUseDefault);
				GbSelectable(aTarget, "((ToyUseSlot1))", EGameBinds_ToyUseSlot1);
				GbSelectable(aTarget, "((ToyUseSlot2))", EGameBinds_ToyUseSlot2);
				GbSelectable(aTarget, "((ToyUseSlot3))", EGameBinds_ToyUseSlot3);
				GbSelectable(aTarget, "((ToyUseSlot4))", EGameBinds_ToyUseSlot4);
				GbSelectable(aTarget, "((ToyUseSlot5))", EGameBinds_ToyUseSlot5);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(s_APIDefs->Localization.Translate("((Templates))")))
			{
				GbSelectable(aTarget, "((Loadout1))", EGameBinds_Loadout1);
				GbSelectable(aTarget, "((Loadout2))", EGameBinds_Loadout2);
				GbSelectable(aTarget, "((Loadout3))", EGameBinds_Loadout3);
				GbSelectable(aTarget, "((Loadout4))", EGameBinds_Loadout4);
				GbSelectable(aTarget, "((Loadout5))", EGameBinds_Loadout5);
				GbSelectable(aTarget, "((Loadout6))", EGameBinds_Loadout6);
				GbSelectable(aTarget, "((Loadout7))", EGameBinds_Loadout7);
				GbSelectable(aTarget, "((Loadout8))", EGameBinds_Loadout8);
				GbSelectable(aTarget, "((Loadout9))", EGameBinds_Loadout9);
				GbSelectable(aTarget, "((GearLoadout1))", EGameBinds_GearLoadout1);
				GbSelectable(aTarget, "((GearLoadout2))", EGameBinds_GearLoadout2);
				GbSelectable(aTarget, "((GearLoadout3))", EGameBinds_GearLoadout3);
				GbSelectable(aTarget, "((GearLoadout4))", EGameBinds_GearLoadout4);
				GbSelectable(aTarget, "((GearLoadout5))", EGameBinds_GearLoadout5);
				GbSelectable(aTarget, "((GearLoadout6))", EGameBinds_GearLoadout6);
				GbSelectable(aTarget, "((GearLoadout7))", EGameBinds_GearLoadout7);
				GbSelectable(aTarget, "((GearLoadout8))", EGameBinds_GearLoadout8);
				GbSelectable(aTarget, "((GearLoadout9))", EGameBinds_GearLoadout9);
				ImGui::EndMenu();
			}
			ImGui::EndCombo();
		}
	}

	void RenderOptions()
	{
		if (!s_APIDefs->GameBinds.IsBound(EGameBinds_CameraActionMode))
		{
			ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f), "\"Toggle Action Camera\" not bound within Nexus.");
			ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f), "You can bind it from Keybinds -> Guild Wars 2. It should match your bind in game.");
		}

		if (!s_RTAPI)
		{
			ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f), "You can install RealTime API for more accurate action camera detection.");
		}

		ImGui::Text("UI/UX");
		if (ImGui::Checkbox("Reset Cursor to Center after Action Cam", &Config::ResetToCenter))
		{
			SaveSettings();
		}

		ImGui::Text("Activation");
		if (ImGui::Checkbox("Enable while moving", &Config::EnableWhileMoving))
		{
			SaveSettings();
		}

		if (ImGui::Checkbox("Enable in combat", &Config::EnableInCombat))
		{
			SaveSettings();
		}

		if (ImGui::Checkbox("Enable while mounted", &Config::EnableOnMount))
		{
			SaveSettings();
		}

		ImGui::Text("Redirect Input");
		ImGui::Checkbox("Redirect Left-Click while action cam is active", &Config::RedirectLMB);
		if (Config::RedirectLMB)
		{
			ImGui::Text("Left-Click Action:");
			ImGui::SameLine();
			GbSelector("##RedirectLMBTarget", &Config::RedirectLMB_Target);
		}
		ImGui::Checkbox("Redirect Right-Click while action cam is active", &Config::RedirectRMB);
		if (Config::RedirectRMB)
		{
			ImGui::Text("Right-Click Action:");
			ImGui::SameLine();
			GbSelector("##RedirectRMBTarget", &Config::RedirectRMB_Target);
		}
	}

	void LoadSettings()
	{
		std::filesystem::path path = s_APIDefs->Paths.GetAddonDirectory(ADDON_NAME"/settings.json");

		if (!std::filesystem::exists(s_APIDefs->Paths.GetAddonDirectory(ADDON_NAME)))
		{
			std::filesystem::create_directory(s_APIDefs->Paths.GetAddonDirectory(ADDON_NAME));
		}

		if (!std::filesystem::exists(s_APIDefs->Paths.GetAddonDirectory(ADDON_NAME"/settings.json")))
		{
			return;
		}

		json settings = json::object();

		try
		{
			std::ifstream file(path);
			settings = json::parse(file);
			file.close();
		}
		catch (json::parse_error& ex)
		{
			s_APIDefs->Log(ELogLevel_WARNING, ADDON_NAME, String::Format("Settings.json could not be parsed. Error: %s", ex.what()).c_str());
		}
		catch (...)
		{
			s_APIDefs->Log(ELogLevel_WARNING, ADDON_NAME, "Error reading settings.");
		}

		const std::lock_guard<std::mutex> lock(s_Mutex);

		if (settings.is_null())
		{
			return;
		}

		if ((!settings["LC_KEY"].is_null() && settings["LC_KEY"].get<int>() > 0) ||
			(!settings["RC_KEY"].is_null() && settings["RC_KEY"].get<int>() > 0))
		{
			/* If the old redirect was being used, show a migration notification. */
			s_APIDefs->UI.SendAlert("MouseLookHandler has reset your redirected keybinds.\nReview your settings.");
		}

		Config::ResetToCenter      = settings.value("RESET_CURSOR_CENTER",        false        );
		Config::EnableWhileMoving  = settings.value("ENABLE_WHILE_MOVING",        true         );
		Config::EnableInCombat     = settings.value("ENABLE_DURING_COMBAT",       false        );
		Config::EnableOnMount      = settings.value("ENABLE_ON_MOUNT",            false        );

		Config::RedirectLMB        = settings.value("REDIRECT_LEFTCLICK",         false        );
		Config::RedirectLMB_Target = settings.value("REDIRECT_LEFTCLICK_TARGET",  (EGameBinds)0);

		Config::RedirectRMB        = settings.value("REDIRECT_RIGHTCLICK",        false        );
		Config::RedirectRMB_Target = settings.value("REDIRECT_RIGHTCLICK_TARGET", (EGameBinds)0);
	}

	void SaveSettings()
	{
		std::filesystem::path path = s_APIDefs->Paths.GetAddonDirectory(ADDON_NAME "/settings.json");

		json settings = json::object();

		const std::lock_guard<std::mutex> lock(s_Mutex);

		settings["RESET_CURSOR_CENTER"]        = Config::ResetToCenter;
		settings["ENABLE_WHILE_MOVING"]        = Config::EnableWhileMoving;
		settings["ENABLE_DURING_COMBAT"]       = Config::EnableInCombat;
		settings["ENABLE_ON_MOUNT"]            = Config::EnableOnMount;

		settings["REDIRECT_LEFTCLICK"]         = Config::RedirectLMB;
		settings["REDIRECT_LEFTCLICK_TARGET"]  = Config::RedirectLMB_Target;

		settings["REDIRECT_RIGHTCLICK"]        = Config::RedirectRMB;
		settings["REDIRECT_RIGHTCLICK_TARGET"] = Config::RedirectRMB_Target;

		try
		{
			std::ofstream file(path);
			file << settings.dump(1, '\t') << std::endl;
			file.close();
		}
		catch (...)
		{
			s_APIDefs->Log(ELogLevel_WARNING, ADDON_NAME, "Error writing settings.");
		}
	}
}
