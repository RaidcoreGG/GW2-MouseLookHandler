///----------------------------------------------------------------------------------------------------
/// Copyright (c) Raidcore.GG - All rights reserved.
///
/// Name         :  imgui_extensions.h
/// Description  :  Additional functions for imgui.
/// Authors      :  K. Bieniek
///----------------------------------------------------------------------------------------------------

#ifndef IMGUI_EXTENSIONS_H
#define IMGUI_EXTENSIONS_H

#include "imgui/imgui.h"

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

#endif
