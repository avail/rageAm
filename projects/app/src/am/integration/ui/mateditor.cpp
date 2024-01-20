
#ifdef AM_INTEGRATED

#include "mateditor.h"
#include "modelscene.h"
#include "widgets.h"
#include "am/integration/hooks/streaming.h"
#include "am/string/splitter.h"
#include "am/ui/font_icons/icons_am.h"
#include "am/xml/doc.h"
#include "am/xml/iterator.h"
#include "am/string/fuzzysearch.h"
#include "am/ui/imglue.h"
#include "am/ui/slwidgets.h"

void rageam::integration::ShaderUIConfig::Load()
{
	CleanUp();

	try
	{
		XmlDoc xDoc;
		xDoc.LoadFromFile(GetFilePath());

		XmlHandle xShaderConfig = xDoc.Root();

		// Vars
		XmlHandle xUIVars = xShaderConfig.GetChild("UIVars", true);
		Vars.InitAndAllocate(xUIVars.GetChildCount("Item"), false);
		for (const XmlHandle& item : XmlIterator(xUIVars, "Item"))
		{
			// Parse main properties
			ConstString varName;
			ConstString overrideName;
			ConstString description;
			item.GetAttribute("Name", varName);
			item.GetChildText("Name", &overrideName);
			item.GetChildText("Description", &description);

			UIVar uiVar;
			uiVar.NameHash = Hash(varName);
			uiVar.OverrideName = overrideName;
			uiVar.Description = description;

			// Check if user added more than one config for one UI var
			if (Vars.ContainsAt(uiVar.NameHash))
			{
				AM_ERRF("ShaderConfig::Load() -> Variable '%s' already was added! Check config for duplicates.", varName);
				CleanUp();
				return;
			}

			// Parse widget
			XmlHandle xWidget = item.GetChild("Widget");
			if (!xWidget.IsNull())
			{
				xWidget.GetAttribute("Type", uiVar.WidgetType);

				switch (uiVar.WidgetType)
				{
				case SliderFloat:
				case SliderFloatLogarithmic:
					xWidget.GetChild("Min").GetValue(uiVar.WidgetParams.SliderFloat.Min);
					xWidget.GetChild("Max").GetValue(uiVar.WidgetParams.SliderFloat.Max);
					break;

				case ToggleFloat:
					xWidget.GetChild("Enabled").GetValue(uiVar.WidgetParams.ToggleFloat.Enabled);
					xWidget.GetChild("Disabled").GetValue(uiVar.WidgetParams.ToggleFloat.Disabled);
					break;

				default:
					break;
				}
			}
			Vars.EmplaceAt(uiVar.NameHash, std::move(uiVar));
		}
	}
	catch (const XmlException& ex)
	{
		ex.Print();
		AM_ERRF("ShaderConfig::Load() -> Failed to load ShaderUI.xml");
		CleanUp();
	}
}

void rageam::integration::ShaderUIConfig::CleanUp()
{
	Vars.Destroy();
}

rageam::integration::ShaderUIConfig::UIVar* rageam::integration::ShaderUIConfig::GetUIVarFor(const rage::grcEffectVar* varInfo) const
{
	UIVar* uiVar;

	// Try to retrieve from name / display name hashes
	uiVar = Vars.TryGetAt(varInfo->GetNameHash());
	if (uiVar) return uiVar;
	uiVar = Vars.TryGetAt(varInfo->GetDisplayNameHash());
	return uiVar;
}

void rageam::integration::MaterialEditor::InitializePresetSearch()
{
	auto findShaderPreset = gmAddress::Scan("48 89 5C 24 08 4C 8B 05").ToFunc<rage::grcInstanceData * (u32)>();

	rage::fiStreamPtr preloadList = rage::fiStream::Open("common:/shaders/db/preload.list");
	if (!preloadList)
		return;
	char fileNameBuffer[64];
	while (preloadList->ReadLine(fileNameBuffer, sizeof fileNameBuffer))
	{
		u32 fileNameHash = rage::atStringHash(fileNameBuffer);

		rage::grcInstanceData* instanceData = findShaderPreset(fileNameHash);
		if (!instanceData)
		{
			AM_WARNINGF("MaterialEditor::InitializePresetSearch() -> Unable to find preset '%s'", fileNameBuffer);
			continue;
		}

		rage::grcEffect* effect = instanceData->GetEffect();
		// Check if shader can be used for drawables
		if (effect->LookupTechnique(TECHNIQUE_DRAW) == INVALID_FX_HANDLE)
			continue;

		ShaderPreset shaderPreset;
		shaderPreset.InstanceData = instanceData;
		shaderPreset.FileName = fileNameBuffer;
		shaderPreset.FileNameHash = fileNameHash;

		// Trim '.sps'
		string name = fileNameBuffer;
		name = name.Substring(0, name.IndexOf<'.'>());
		shaderPreset.Name = std::move(name);

		ComputePresetFilterTagAndTokens(shaderPreset);

		m_ShaderPresets.Emplace(std::move(shaderPreset));
	}
	m_PresetSearchIndices.Reserve(m_ShaderPresets.GetSize());
}

void rageam::integration::MaterialEditor::ComputePresetFilterTagAndTokens(ShaderPreset& preset) const
{
	preset.TokenCount = 0;

	const string& name = preset.Name;

	u32 c = 0; // Categories
	u32 m = 0; // Maps

	// Tokenize 'normal_spec_decal' on 'normal 'spec' 'decal' and assign tags
	size_t tokenOffset, tokenLength;
	ConstString token;
	StringSplitter<'_'> splitter(name);
	while (splitter.GetNext(token, &tokenOffset, &tokenLength))
	{
		u32 tokenHash = Hash(token);

		if (preset.TokenCount >= SHADER_MAX_TOKENS)
		{
			AM_WARNINGF("MaterialEditor::GetShaderTagAndTokens() -> Too much tokens in '%s'", name.GetCStr());
		}
		else
		{
			preset.Tokens[preset.TokenCount++] = std::string_view(name + tokenOffset, tokenLength);
		}

		switch (tokenHash)
		{
			// Categories
		case Hash("vehicle"):		c |= ST_Vehicle;	break;
		case Hash("ped"):			c |= ST_Ped;		break;
		case Hash("weapon"):		c |= ST_Weapon;		break;
		case Hash("terrain"):		c |= ST_Terrain;	break;
		case Hash("glass"):			c |= ST_Glass;		break;
		case Hash("decal"):			c |= ST_Decal;		break;
		case Hash("cloth"):			c |= ST_Cloth;		break;
		case Hash("cutout"):		c |= ST_Cutout;		break;
		case Hash("emissivenight"):
		case Hash("emissive"):		c |= ST_Emissive;	break;
		case Hash("pxm"):
		case Hash("parallax"):		c |= ST_Parallax;	break;
			// Maps
		case Hash("detail"):
		case Hash("detail2"):		m |= SM_Detail;		break;
		case Hash("spec"):
		case Hash("specmap"):		m |= SM_Specular;	break;
		case Hash("normal"):		m |= SM_Normal;		break;
		case Hash("enveff"):
		case Hash("reflect"):		m |= SM_Cubemap;	break;
		case Hash("tnt"):			m |= SM_Tint;		break;
		}
	}

	if (c == 0)
		c |= ST_Misc; // Not in any of main categories, throw into misc

	preset.Tag.Categories = c;
	preset.Tag.Maps = m;
}

rage::grmShaderGroup* rageam::integration::MaterialEditor::GetShaderGroup() const
{
	return m_Context->Drawable->GetShaderGroup();
}

rage::grmShader* rageam::integration::MaterialEditor::GetSelectedMaterial() const
{
	return GetShaderGroup()->GetShader(m_SelectedMaterialIndex);
}

ImTextureID rageam::integration::MaterialEditor::GetTexID(const rage::grcTexture* tex) const
{
	if (asset::TxdAsset::IsMissingTexture(tex))
		return ui::GetUI()->GetIcon("no_tex_ui")->GetID();
	return ImTextureID(tex->GetTextureView());
}

ImU32 rageam::integration::MaterialEditor::GetTexLabelCol(const rage::grcTexture* tex) const
{
	if (!asset::TxdAsset::IsMissingTexture(tex))
		return ImGui::GetColorU32(ImGuiCol_Text);

	static constexpr float animTime = 2.0f;
	float phase = static_cast<float>(fmod(ImGui::GetTime(), 4.0f));

	// Loop and normalize
	if (phase > animTime * 0.5f)
		phase = animTime - phase;
	phase *= 2;
	phase /= animTime;

	static constexpr ImVec4 COL1 = { 1.0f, 0.0f, 1.0f, 1.0f }; // Pink
	static constexpr ImVec4 COL2 = { 0.6f, 0.0f, 0.6f, 1.0f }; // Dark Pink

	return ImGui::ColorConvertFloat4ToU32(ImLerp(COL1, COL2, phase));
}

void rageam::integration::MaterialEditor::ScrollingLabel(const ImVec2& pos, const ImRect& bb, const rage::grcTexture* texture) const
{
	ConstString label = texture->GetName();
	ConstString labelEnd = ImGui::FindRenderedTextEnd(label); // Support missing texture names ##
	ImGui::ScrollingLabel(pos, bb, label, labelEnd, GetTexLabelCol(texture), m_TexturePickerOpenTime);
}

rage::grcTexture* rageam::integration::MaterialEditor::TexturePicker_Grid(bool groupByDict, float iconScale)
{
	ImGuiStyle& style = ImGui::GetStyle();
	auto textureItem = [&](ConstString idStr, const rage::grcTexture* texture) -> bool
		{
			ImGuiWindow* window = ImGui::GetCurrentWindow();
			ImGuiID id = ImGui::GetID(idStr);

			float iconSizeMax = ImGui::GetFrameHeight() * 4.0f * iconScale;

			int iconWidth, iconHeight;
			graphics::ImageFitInRect(texture->GetWidth(), texture->GetHeight(), iconSizeMax, iconWidth, iconHeight);

			// Hide label when icons are very tiny
			bool showLabel = iconScale > 0.7;

			float labelHeight = showLabel ? ImGui::GetFrameHeight() : 0;

			ImVec2 iconSize(iconWidth, iconHeight);
			ImVec2 size(
				iconSizeMax + style.FramePadding.x * 2,
				iconSizeMax + style.FramePadding.y * 2 + labelHeight); // Frame for label text

			ImVec2 min = window->DC.CursorPos;
			ImVec2 max = min + size;
			ImRect bb(min, max);

			ImGui::ItemSize(size);
			if (!ImGui::ItemAdd(bb, id))
				return false;

			// Hit test
			bool hovered;
			bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, nullptr);

			// Rendering
			{
				ImVec2 iconMin = min + style.FramePadding;
				ImVec2 iconMax = iconMin + iconSize;

				ImU32 borderColor = ImGui::GetColorU32(
					pressed ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Border);

				// Border
				window->DrawList->AddRect(min, max, borderColor, 2);
				// Icon
				window->DrawList->AddImage(GetTexID(texture), iconMin, iconMax);

				// Label
				if (showLabel)
				{
					ImVec2 textPos( // Below icon
						min.x + style.FramePadding.x,
						max.y - ImGui::GetFrameHeight());

					ScrollingLabel(textPos, bb, texture);
				}
			}
			return pressed;
		};

	rage::grcTexture* pickedTexture = nullptr;
	u16 totalTextures = 0;
	for (u16 i = 0; i < m_TextureSearchEntries.GetSize(); i++)
	{
		const TextureSearch& search = m_TextureSearchEntries[i];

		bool dictOpen = true;
		if (groupByDict)
			dictOpen = ImGui::CollapsingHeader(search.DictName, ImGuiTreeNodeFlags_DefaultOpen);

		if (dictOpen)
		{
			// For item wrapping
			float maxItemX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

			for (u16 k = 0; k < search.Textures.GetSize(); k++)
			{
				rage::grcTexture* texture = search.Dict->GetValueAt(search.Textures[k]);

				// Try to place on current line
				if (groupByDict ? k != 0 : totalTextures != 0)
				{
					const ImRect& prevItemRect = GImGui->LastItemData.Rect;

					float nextItemX = prevItemRect.Max.x + style.ItemSpacing.x + prevItemRect.GetSize().x;
					if (nextItemX < maxItemX)
						ImGui::SameLine();
				}

				ConstString idStr = ImGui::FormatTemp("###GRID_TEX_%u_%u", i, k);
				if (textureItem(idStr, texture))
					pickedTexture = texture;

				// Show texture name when hovered tile
				ImGui::ToolTip(texture->GetName());

				totalTextures++;
			}
		}
	}

	return pickedTexture;
}

rage::grcTexture* rageam::integration::MaterialEditor::TexturePicker_List(float iconScale)
{
	rage::grcTexture* pickedTexture = nullptr;

	static bool navChangedPrevFrame = false;

	ImGui::PushStyleColor(ImGuiCol_NavHighlight, 0);

	// Draw dictionaries + textures 
	u16 dictCount = m_TextureSearchEntries.GetSize();
	bool navUpdated = false;
	for (u16 i = 0; i < m_TextureSearchEntries.GetSize(); i++)
	{
		const TextureSearch& search = m_TextureSearchEntries[i];

		bool dictSelected = m_DictionaryIndex == i;

		// We can't use default navigation system because we always have focus on text box
		ImGuiDir moveDir = ImGuiDir_None;
		if (SlGui::IsKeyDownDelayed(ImGuiKey_UpArrow))		moveDir = ImGuiDir_Up;
		if (SlGui::IsKeyDownDelayed(ImGuiKey_DownArrow))	moveDir = ImGuiDir_Down;
		if (SlGui::IsKeyDownDelayed(ImGuiKey_LeftArrow))	moveDir = ImGuiDir_Left;
		if (SlGui::IsKeyDownDelayed(ImGuiKey_RightArrow))	moveDir = ImGuiDir_Right;

		// Dict opening/closing using nav buttons
		if (dictSelected)
		{
			if (m_DictionaryIndex == i && moveDir == ImGuiDir_Left)
			{
				ImGui::TreeNodeSetOpened(search.DictName, false); ImGui::TreePop();
			}

			if (m_DictionaryIndex == i && moveDir == ImGuiDir_Right)
			{
				ImGui::TreeNodeSetOpened(search.DictName, true); ImGui::TreePop();
			}
		}

		u16 dictTexCount = search.Textures.GetSize();

		// Draw dict & textures
		bool toggled;
		bool isDictOpen = false;
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.65f);
		bool treeOpen = SlGui::GraphTreeNode(search.DictName, dictSelected, toggled,
			SlGuiTreeNodeFlags_DefaultOpen | SlGuiTreeNodeFlags_AlwaysShowArrows);
		if (toggled)
		{
			// Node was selected, reset texture
			m_TextureIndex = 0;
		}
		ImGui::PopStyleVar();

		// We navigated to dict last frame, scroll to it
		if (m_TextureIndex == 0 && dictSelected && navChangedPrevFrame)
		{
			navChangedPrevFrame = false;
			ImGui::ScrollToItem();
		}

		if (treeOpen)
		{
			isDictOpen = true;
			for (u16 k = 0; k < dictTexCount; k++)
			{
				u16 texIndex = search.Textures[k];
				rage::grcTexture* texture = search.Dict->GetValueAt(texIndex);

				// Node
				ConstString displayName = ImGui::FormatTemp("###%u;%u", i, k);
				bool texSelected = dictSelected && m_TextureIndex == k;
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 6.0f * iconScale));
				SlGui::GraphTreeNode(displayName, texSelected, toggled, SlGuiTreeNodeFlags_NoChildren);
				ImGui::PopStyleVar();

				// We navigated to texture last frame, scroll to it
				if (texSelected && navChangedPrevFrame)
				{
					navChangedPrevFrame = false;
					ImGui::ScrollToItem();
				}

				const ImRect& nodeRect = GImGui->LastItemData.Rect;

				// Draw separator on bottom of node
				ImGui::GetWindowDrawList()->AddLine(nodeRect.GetBL(), nodeRect.GetBR(), ImGui::GetColorU32(ImGuiCol_Border));

				ImTextureID texId = GetTexID(texture);

				// Texture icon, we draw it first to get full available rect for text
				ImVec2 textMin = nodeRect.GetTL() + ImVec2(ImGui::GetFrameHeight(), 0);
				ImVec2 textMax;
				{
					const ImGuiStyle& style = GImGui->Style;
					const ImVec2 iconPad = ImVec2();

					float iconSizeY = nodeRect.GetHeight() - iconPad.y * 2;
					float iconSizeX = iconSizeY * 2.0f; // We have much more space horizontally, let icon span on Y Axis
					int width, height;
					ImageScaleResolution(texture->GetWidth(), texture->GetHeight(), iconSizeX, iconSizeY, width, height, graphics::ResolutionScalingMode_Fit);

					// We take coord from window because node X is intended
					ImVec2 min = nodeRect.GetTR();
					min.x -= width + iconPad.x;
					min.y += iconPad.y;
					ImVec2 max = min + ImVec2(width, height - iconPad.y);

					textMax = ImVec2(min.x, max.y);

					ImGui::GetWindowDrawList()->AddImage(texId, min, max);
				}

				// Node label
				ImVec2 textPos = textMin + ImGui::GetStyle().FramePadding;
				ImRect textRect(textPos, textMax);
				ScrollingLabel(textPos, textRect, texture);

				// Pick texture using enter or click
				if (texSelected && ImGui::IsKeyPressed(ImGuiKey_Enter) ||
					ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(nodeRect.Min, nodeRect.Max))
				{
					pickedTexture = texture;
				}

				// Draw selected texture in next to window
				if (texSelected)
				{
					constexpr float previewSize = 96.0f;

					int width, height;
					graphics::ImageFitInRect(texture->GetWidth(), texture->GetHeight(), previewSize, width, height);;

					ImVec2 min = ImGui::GetCurrentWindow()->ParentWindow->OuterRectClipped.GetTL() - ImVec2(width, 0);
					ImVec2 max = min + ImVec2(width, height);

					ImDrawList* drawList = ImGui::GetForegroundDrawList();
					drawList->AddImage(texId, min, max);
					drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_ButtonActive));
				}
			}

			ImGui::TreePop();
		}

		if (dictSelected)
			m_DictionaryIndex = i;

		// Up/Down nav
		// This all is a bit awkward but there's no other way to keep input focused + navigation working
		if (moveDir != ImGuiDir_None && dictSelected && !navUpdated)
		{
			// We have to keep this flag because otherwise if we select bottom dict nav will be updated twice
			navUpdated = true;

			navChangedPrevFrame = true;

			auto getLastDictTexIndex = [&]
				{
					return m_TextureSearchEntries[m_DictionaryIndex].Dict->GetSize() - 1;
				};

			u16 maxNavDictIndex = dictCount - 1;
			u16 maxNavTexIndex = dictTexCount - 1;

			// Case 0: Dictionary isn't open,
			// we move down to next dictionary and up to last texture of previous dictionary
			if (!isDictOpen)
			{
				if (m_DictionaryIndex < maxNavDictIndex && moveDir == ImGuiDir_Down)
				{
					m_DictionaryIndex++;
				}
				else if (m_DictionaryIndex > 0 && moveDir == ImGuiDir_Up)
				{
					m_DictionaryIndex--;
					m_TextureIndex = getLastDictTexIndex();
				}
			}
			// Case 1: Dictionary is open,
			// we move up to either previous texture or to last texture of previous dictionary,
			// down to either next texture or first texture of next dictionary
			else
			{
				if (moveDir == ImGuiDir_Down)
				{
					if (m_TextureIndex < maxNavTexIndex)
					{
						m_TextureIndex++;
					}
					else if (m_DictionaryIndex < maxNavDictIndex)
					{
						m_DictionaryIndex++;
						m_TextureIndex = 0;
					}
				}

				if (moveDir == ImGuiDir_Up)
				{
					if (m_TextureIndex > 0) m_TextureIndex--;
					else if (m_DictionaryIndex > 0)
					{
						m_DictionaryIndex--;
						m_TextureIndex = getLastDictTexIndex();
					}
				}
			}
		}
	}

	ImGui::PopStyleColor(); // NavHighlight

	// Keep search text box selected
	ImGui::NavMoveRequestCancel();

	return pickedTexture;
}

rage::grcTexture* rageam::integration::MaterialEditor::TexturePicker(ConstString idStr, const rage::grcTexture* currentTexture)
{
	ImGuiID id = ImGui::GetID(idStr);

	// Draw current texture preview
	{
		constexpr float previewIconSize = 32.0f;

		if (currentTexture)
		{
			pVoid textureView = currentTexture->GetTextureView();
			int width, height;
			graphics::ImageFitInRect(
				currentTexture->GetWidth(), currentTexture->GetHeight(), previewIconSize, width, height);

			ImGui::Image(ImTextureID(textureView), ImVec2(width, height));
		}
		else
		{
			ImGui::Dummy(ImVec2(previewIconSize, previewIconSize));
		}

		// Icon border
		const ImRect& iconRect = GImGui->LastItemData.Rect;
		ImGui::GetWindowDrawList()->AddRect(iconRect.Min, iconRect.Max, ImGui::GetColorU32(ImGuiCol_Border), 2);

		ImGui::SameLine();
	}

	// Open picker
	static constexpr ConstString PICKER_POPUP = "TEXTURE_PICKER_POPUP";
	static u32 s_PickTexID = 0;
	ConstString currentTextureName = currentTexture ? currentTexture->GetName() : "-";
	float buttonWidth = ImGui::GetContentRegionAvail().x;
	if (ImGui::Button(ImGui::FormatTemp("%s###%s", currentTextureName, idStr), ImVec2(buttonWidth, 0)))
	{
		// Reset search
		m_DictionaryIndex = 0;
		m_TextureIndex = 0;
		m_TextureSearchText[0] = '\0';
		DoTextureSearch();

		// We need ID check or otherwise we gonna draw picker for all textures in shader
		s_PickTexID = id;

		ImGui::OpenPopup(PICKER_POPUP);
		m_TexturePickerOpenTime = ImGui::GetTime();
	}

	// Not picking anything, skip
	if (s_PickTexID != id)
		return nullptr;

	static constexpr float ICON_SIZE_MIN = 0.5f;
	static constexpr float ICON_SIZE_MAX = 3.0f;
	static float s_IconSize = 0.25f;
	static bool s_Grid = false;
	static bool s_GridGroupByDict = true;

	auto doTextureSearch = [&]
		{
			m_DictionaryIndex = 0;
			m_TextureIndex = 0;
			DoTextureSearch();
		};

	// Some TXD was modified, current search is invalid now and we have to redo it
	if (m_Context->HotFlags & AssetHotFlags_TxdModified)
		doTextureSearch();

	// Draw picker
	rage::grcTexture* pickedTexture = nullptr;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1, 1));
	ImGui::SetNextWindowPos(GImGui->LastItemData.Rect.GetBL()); // Position right under button
	ImGui::SetNextWindowSize(ImVec2(s_Grid ? 450 : 240, 400));
	if (ImGui::BeginPopup(PICKER_POPUP))
	{
		ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 4.0f);
		ImGui::SliderFloat("Icon Size", &s_IconSize, 0.0f, 1.0f, "", ImGuiSliderFlags_NoRoundToFormat);
		ImGui::SameLine(0, ImGui::GetStyle().FramePadding.x);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
		ImGui::Checkbox("Grid", &s_Grid);
		if (s_Grid)
		{
			ImGui::SameLine();
			ImGui::Checkbox("Group", &s_GridGroupByDict);
		}
		float iconScale = rage::Remap(s_IconSize, 0.0f, 1.0f, ICON_SIZE_MIN, ICON_SIZE_MAX);

		ImGui::HelpMarker(
			"There are two ways to search:\n"
			"Default - Searches in both dictionary and texture name\n"
			"Extended - Searches in a specific texture dictionary.\n"
			"\tSearch must be in the following format 'Dict/Texture'");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		if (ImGui::InputText("###SEARCH", m_TextureSearchText, IM_ARRAYSIZE(m_TextureSearchText)))
		{
			doTextureSearch();
		}
		ImGui::InputTextPlaceholder(m_TextureSearchText, "Search...", true);
		if (ImGui::IsWindowAppearing())
		{
			ImGui::ActivateItemByID(ImGui::GetItemID());
		}
		ImGui::PopStyleVar(); // ItemSpacing

		ImGui::BeginChild("TEXTURE_PICKER_SCROLL");
		if (s_Grid)	pickedTexture = TexturePicker_Grid(s_GridGroupByDict, iconScale);
		else		pickedTexture = TexturePicker_List(iconScale);
		ImGui::EndChild();

		if (pickedTexture != nullptr)
			ImGui::CloseCurrentPopup();

		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(1); // ItemSpacing

	// Texture was picked, reset ID
	if (!ImGui::IsPopupOpen(PICKER_POPUP))
	{
		s_PickTexID = 0;
	}

	return pickedTexture;
}

void rageam::integration::MaterialEditor::DoTextureSearch()
{
	m_TextureSearchEntries.Clear();

	ImmutableString search = m_TextureSearchText;

	// Search string is in format:
	// DictName/TextureName
	int separatorIndex = search.IndexOf('/');

	// Dictionary name is specified always, texture only if '/' present
	char dictSearchBuffer[64];
	char texSearchBuffer[64]{};
	String::Copy(dictSearchBuffer, sizeof dictSearchBuffer, search, separatorIndex);
	if (separatorIndex != -1)
		String::Copy(texSearchBuffer, sizeof texSearchBuffer, search + separatorIndex + 1);

	ImmutableString dictSearch = dictSearchBuffer;
	// We use whole search string in non-full mode
	ImmutableString texSearch = separatorIndex == -1 ? dictSearchBuffer : texSearchBuffer;

	bool hasDictSearch = dictSearch[0] != '\0';
	bool hasTexSearch = texSearch[0] != '\0';

	// We skip search only in full '/' mode where both dict&tex are specified,
	// otherwise we use given search only for texture
	bool isFullSearch = separatorIndex != -1;

	// Performs search in dict and adds matching textures to results
	auto doSearch = [&](ImmutableString dictName, rage::grcTextureDictionary* dict)
		{
			bool dictNameMatches = true;
			if (hasDictSearch) dictNameMatches = dictName.StartsWith(dictSearch, true);

			// In full search we require dict name to match
			if (isFullSearch && !dictNameMatches)
				return;

			TextureSearch textureSearch;

			// Dictionary matched, search for textures
			for (u16 i = 0; i < dict->GetSize(); i++)
			{
				ImmutableString texName = dict->GetValueAt(i)->GetName();

				bool texNameMatched = true;
				if (hasTexSearch) texNameMatched = texName.StartsWith(texSearch, true);

				if (isFullSearch)
				{
					if (!texNameMatched)
						continue;
				}
				else
				{
					if (!dictNameMatches && !texNameMatched)
						continue;
				}

				textureSearch.Textures.Add(i);
			}

			// No textures found, skip
			if (textureSearch.Textures.GetSize() == 0)
				return;

			textureSearch.Dict = dict;
			String::Copy(textureSearch.DictName, sizeof textureSearch.DictName, dictName);

			m_TextureSearchEntries.Emplace(std::move(textureSearch));
		};

	// Add embed dict, if present
	auto embedDict = m_Context->Drawable->GetShaderGroup()->GetEmbedTextureDictionary().Get();
	if (embedDict)
		doSearch("Embed", embedDict);

	// Add workspace TXDs
	for (asset::DrawableTxd& txd : *m_Context->TXDs)
	{
		// We draw embed dictionary separately with our custom name
		if (txd.IsEmbed)
			continue;

		file::WPath txdAssetName = txd.Asset->GetAssetName();
		// Convert to ansi + remove '.itd' extension
		file::Path txdName = file::PathConverter::WideToUtf8(txdAssetName);
		txdName = txdName.GetFileNameWithoutExtension();

		doSearch(txdName.GetCStr(), txd.Dict.Get());
	}
}

void rageam::integration::MaterialEditor::HandleMaterialValueChange(u16 varIndex, rage::grcInstanceVar* var, const rage::grcEffectVar* varInfo)
{
	StoreMaterialValue(m_SelectedMaterialIndex, varIndex);

	// Toggle tessellation
	if (varInfo->GetNameHash() == rage::atStringHash("useTessellation"))
	{
		m_Context->Drawable->ComputeTessellationForShader(m_SelectedMaterialIndex);
	}
}

void rageam::integration::MaterialEditor::HandleShaderChange()
{
	// Try to retrieve variables from previous shaders
	rage::grmShaderGroup* shaderGroup = GetShaderGroup();
	for (u16 i = 0; i < shaderGroup->GetShaderCount(); i++)
	{
		rage::grmShader* shader = shaderGroup->GetShader(i);
		for (u16 k = 0; k < shader->GetVarCount(); k++)
		{
			RestoreMaterialValue(i, k);
		}
	}
}

void rageam::integration::MaterialEditor::DoFuzzySearch()
{
	// Timer searchTimer = Timer::StartNew();

	m_PresetSearchIndices.Clear();

	// Tokenize search and store in array
	List<string> searchTokens;
	{
		ConstString tokenTemp;
		StringSplitter<' ', '_', '-', ',', ';'> splitter(m_PresetSearchText);
		splitter.SetTrimSpaces(true);
		while (splitter.GetNext(tokenTemp))
		{
			string token = tokenTemp;
			token = token.ToLowercase(); // All presets are lower case

			searchTokens.Emplace(std::move(token));
		}
	}

	List<float> presetDistances;
	presetDistances.Reserve(m_ShaderPresets.GetSize());

	for (u16 i = 0; i < m_ShaderPresets.GetSize(); i++)
	{
		ShaderPreset& preset = m_ShaderPresets[i];

		float totalDistance = 0.0f; // More = worse match

		// We introduce 'super kill' system where more good matches increases changes of preset coming first
		// Otherwise we may get case when 'vehicle_mesh' comes before 'vehicle_paint1_enveff'
		// with search 'vehicle, paint'
		float matchMultiplier = 1.0f;

		// Compare each preset token against search token
		for (u32 k = 0; k < preset.TokenCount; k++)
		{
			std::string_view& presetTokenView = preset.Tokens[k];
			// Copy token to local buffer, fuzzy compare doesn't work with string view
			char presetToken[128]{};
			presetTokenView.copy(presetToken, presetTokenView.length());

			// Find distance of best matching token
			float distance = FLT_MAX;
			for (string& searchToken : searchTokens)
			{
				// First do straight search, then do fuzzy
				ImmutableString lhs = presetToken;
				if (lhs.StartsWith(searchToken))
					distance = 0.0f; //
				else
					distance = MIN(distance, static_cast<float>(LevenshteinDistance(presetToken, searchToken)));
			}

			if (distance <= 2.0f) matchMultiplier += 0.5f;
			if (distance == 0.0f) matchMultiplier += 1.25f;

			distance /= matchMultiplier;

			// Normalize by total token count to prevent bias towards presets with less tokens
			totalDistance += distance /= static_cast<float>(preset.TokenCount);
		}

		// No match
		if (totalDistance > 3.0f)
			continue;

		// AM_DEBUGF("%s (D:%g)", preset.Name.GetCStr(), totalDistance);

		// Find where current search result goes in leader board
		u16 insertIndex = 0;
		for (; insertIndex < presetDistances.GetSize(); insertIndex++)
		{
			if (totalDistance <= presetDistances[insertIndex])
			{
				break;
			}
		}

		presetDistances.Insert(insertIndex, totalDistance);
		m_PresetSearchIndices.Insert(insertIndex, i);
	}

	// searchTimer.Stop();
	// AM_DEBUGF(":MaterialEditor::DrawShaderList() -> Search took %gs", searchTimer.GetElapsedMicroseconds() / 1000000.0);
}

void rageam::integration::MaterialEditor::DrawShaderSearchListItem(u16 index)
{
	ImVec2 buttonSize(ImGui::GetContentRegionAvail().x, 0);
	graphics::ColorU32 buttonColor = ImGui::GetColorU32(ImGuiCol_Button);
	buttonColor.A = 25;

	ShaderPreset& preset = m_ShaderPresets[index];

	// Only presets that include all selected categories
	if (m_PresetSearchCategories != 0 && (preset.Tag.Categories | m_PresetSearchCategories) != preset.Tag.Categories)
		return;

	// Only presets that include all selected maps
	if (m_PresetSearchMaps != 0 && (preset.Tag.Maps | m_PresetSearchMaps) != preset.Tag.Maps)
		return;

	// TODO: Show selected shader

	ImGui::PushFont(ImFont_Small);
	ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f)); // Align left
	if (ImGui::ButtonEx(preset.Name, buttonSize))
	{
		// Clone material from preset
		rage::grmShaderGroup* shaderGroup = m_Context->Drawable->GetShaderGroup();
		rage::grmShader* material = shaderGroup->GetShader(m_SelectedMaterialIndex);
		material->CloneFrom(*preset.InstanceData);
		HandleShaderChange();
	}
	ImGui::PopStyleVar();	// ButtonTextAlign
	ImGui::PopStyleColor(); // Button
	ImGui::PopFont();
}

void rageam::integration::MaterialEditor::DrawShaderSearchList()
{
	// Search box
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	if (ImGui::InputText("###SEARCH", m_PresetSearchText, sizeof m_PresetSearchText))
		DoFuzzySearch();
	ImGui::InputTextPlaceholder(m_PresetSearchText, "Search...");

	bool hasSearch = m_PresetSearchText[0] != '\0';

	// Reserve space for status bar
	float height = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight();

	// Shaders
	if (ImGui::BeginChild("SHADER_LIST", ImVec2(0, height)))
	{
		//SlGui::ShadeItem(SlGuiCol_Bg);

		// Display either search result or all presets
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
		if (hasSearch)
		{
			for (u16 i : m_PresetSearchIndices)
				DrawShaderSearchListItem(i);
		}
		else
		{
			for (u16 i = 0; i < m_ShaderPresets.GetSize(); i++)
				DrawShaderSearchListItem(i);
		}
		ImGui::PopStyleVar(); // ItemSpacing
	}
	ImGui::EndChild();

	// Count on bottom of window
	u16 itemCount = hasSearch ? m_PresetSearchIndices.GetSize() : m_ShaderPresets.GetSize();
	ImGui::Dummy(ImVec2(4, 4)); ImGui::SameLine(0, 0); // Window has no padding, we have to add it manually
	ImGui::Text("%u Item(s)", itemCount);
}

void rageam::integration::MaterialEditor::DrawShaderSearchFilters()
{
#define CATEGORY_FLAG(name) ImGui::CheckboxFlags(#name, &m_PresetSearchCategories, ST_ ##name);
#define MAP_FLAG(name) ImGui::CheckboxFlags(#name, &m_PresetSearchMaps, SM_ ##name);

	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1, 1));

	// ShaderCategories
	SlGui::CategoryText("Categories");
	ImGui::PushFont(ImFont_Small);
	if (ImGui::BeginTable("SHADER_CATEGORIES_TABLE", 2))
	{
		ImGui::TableNextColumn(); CATEGORY_FLAG(Vehicle);	ImGui::TableNextColumn(); CATEGORY_FLAG(Ped);
		ImGui::TableNextColumn(); CATEGORY_FLAG(Weapon);	ImGui::TableNextColumn(); CATEGORY_FLAG(Terrain);
		ImGui::TableNextColumn(); CATEGORY_FLAG(Glass);		ImGui::TableNextColumn(); CATEGORY_FLAG(Decal);
		ImGui::TableNextColumn(); CATEGORY_FLAG(Cloth);		ImGui::TableNextColumn(); CATEGORY_FLAG(Cutout);
		ImGui::TableNextColumn(); CATEGORY_FLAG(Emissive);	ImGui::TableNextColumn(); CATEGORY_FLAG(Parallax);
		ImGui::TableNextColumn(); CATEGORY_FLAG(Misc);
		ImGui::EndTable();
	}
	ImGui::PopFont();

	// ShaderMaps
	SlGui::CategoryText("Maps");
	ImGui::PushFont(ImFont_Small);
	if (ImGui::BeginTable("SHADER_MAPS_TABLE", 1))
	{
		ImGui::TableNextColumn(); MAP_FLAG(Detail);
		ImGui::TableNextColumn(); MAP_FLAG(Specular);
		ImGui::TableNextColumn(); MAP_FLAG(Normal);
		ImGui::TableNextColumn(); MAP_FLAG(Cubemap);
		ImGui::TableNextColumn(); MAP_FLAG(Tint);
		ImGui::EndTable();
	}
	ImGui::PopFont();

	ImGui::PopStyleVar(1); // CellPadding

#undef CATEGORY_FLAG
#undef MAP_FLAG
}

void rageam::integration::MaterialEditor::DrawShaderSearch()
{
	if (ImGui::BeginTable("SHADER_LIST_TABLE", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
	{
		ImGui::TableNextRow();

		// Column: List
		if (ImGui::TableNextColumn())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
			DrawShaderSearchList();
			ImGui::PopStyleVar();
		}

		// Column: Filters
		if (ImGui::TableNextColumn())
		{
			//SlGui::BeginPadded("SHADER_SEARCH_FILTERS_PADDING", ImVec2(4, 4));
			DrawShaderSearchFilters();
			//SlGui::EndPadded();
		}

		ImGui::EndTable();
	}
}

void rageam::integration::MaterialEditor::DrawMaterialList()
{
	// TODO: Sphere Ball Preview & Preview resizing (ItemDragged?)

	if (!ImGui::BeginChild("MaterialEditor_List"))
	{
		ImGui::EndChild();
		return;
	}
	//SlGui::ShadeItem(SlGuiCol_Bg);

	asset::DrawableTune& drawableTune = m_Context->DrawableAsset->GetDrawableTune();

	auto materialEntry = [&](u16 i, bool orphans)
		{
			// Shader group directly map to material tune group
			asset::MaterialTune& materialTune = *drawableTune.Materials.Get(i);

			if (materialTune.NoLongerNeeded)
				return;

			// We draw all orphan (unused) materials later, grouped in the bottom of the list
			if (materialTune.IsRemoved != orphans)
				return;

			ConstString materialName = materialTune.Name;

			ConstString nodeName = ImGui::FormatTemp("%s###%s_%i", materialName, materialName, i);

			if (orphans) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.8f);
			bool selected = m_SelectedMaterialIndex == i;
			bool toggled;
			SlGui::GraphTreeNode(nodeName, selected, toggled, SlGuiTreeNodeFlags_NoChildren);
			if (selected && m_SelectedMaterialIndex != i)
			{
				m_SelectedMaterialIndex = i;
			}
			if (orphans) ImGui::PopStyleVar(); // Alpha

			// Remove orphan button
			if (orphans)
			{
				ImGui::SameLine(); // We draw node on the same line

				// Align remove button to the right
				float buttonWidth = ImGui::CalcTextSize(ICON_AM_CANCEL).x + ImGui::GetStyle().FramePadding.x * 2.0f;
				ImGuiWindow* window = ImGui::GetCurrentWindow();
				window->DC.CursorPos.x = window->WorkRect.Max.x - buttonWidth;

				if (SlGui::IconButton(ICON_AM_CANCEL))
				{
					// We use toggling to allow user to bring material back if clicked accidentally
					materialTune.NoLongerNeeded = true; // TODO: Need undo here

					if (m_SelectedMaterialIndex == i)
						m_SelectedMaterialIndex = 0;
				}
			}
		};

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	{
		rage::grmShaderGroup* shaderGroup = m_Context->Drawable->GetShaderGroup();

		bool hasUnusedMaterials = false;
		// First we display all used materials (non orphans)
		for (u16 i = 0; i < shaderGroup->GetShaderCount(); i++)
		{
			materialEntry(i, false);

			asset::MaterialTune& materialTune = *drawableTune.Materials.Get(i);
			if (materialTune.IsRemoved && !materialTune.NoLongerNeeded)
				hasUnusedMaterials = true;
		}

		if(hasUnusedMaterials)
		{
			// Unused text + remove all button
			{
				ImGui::Dummy(ImVec2(2, 0)); ImGui::SameLine(); // Padding before text
				SlGui::CategoryText("Unused");

				//ImGui::PushStyleColor(ImGuiCol_Button, 0);
				//if (ImGui::Button("Remove all", ImVec2(-1, 0)))
				//{
				//	for (asset::MaterialTune& materialTune : drawableTune.MaterialGroup.Items)
				//	{
				//		if (materialTune.IsOrphan) materialTune.NoLongerNeeded = true;
				//	}
				//}
				//ImGui::PopStyleColor();
			}

			// And now all unused materials
			for (u16 i = 0; i < shaderGroup->GetShaderCount(); i++)
				materialEntry(i, true);
		}
	}
	ImGui::PopStyleVar(); // Item_Spacing
	ImGui::EndChild();    // MaterialEditor_List
}

void rageam::integration::MaterialEditor::DrawMaterialVariables()
{
	//if (!SlGui::BeginPadded("MaterialEditor_Properties_Padding", ImVec2(6, 6)))
	//{
	//	SlGui::EndPadded();
	//	return;
	//}

	rage::grmShader* material = GetSelectedMaterial();

	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 2));
	bool propertiesTable = ImGui::BeginTable("MaterialEditor_Properties", 2, ImGuiTableFlags_SizingFixedFit);
	ImGui::PopStyleVar(); // ItemInnerSpacing
	if (!propertiesTable)
		return;

	ImGui::TableSetupColumn("MaterialEditor_Properties_Name", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("MaterialEditor_Properties_Value", ImGuiTableColumnFlags_WidthStretch);

	rage::grcEffect* effect = material->GetEffect();
	for (u16 i = 0; i < material->GetVarCount(); i++)
	{
		ImGui::TableNextRow();

		// Material (shader in rage terms) holds variable values without any metadata,
		// we have to query effect variable to get name and type
		rage::grcInstanceVar* var = material->GetVarByIndex(i);
		rage::grcEffectVar* varInfo = effect->GetVarByIndex(i);

		ShaderUIConfig::UIVar* uiVar = m_UIConfig.GetUIVarFor(varInfo);

		// Column: Variable Name
		if (ImGui::TableNextColumn())
		{
			ConstString name = varInfo->GetName();

			if (uiVar)
			{
				// Retrieve override name from UI config
				if (!String::IsNullOrEmpty(uiVar->OverrideName))
				{
					name = uiVar->OverrideName;
				}

				// Add description from UI config
				if (!String::IsNullOrEmpty(uiVar->Description))
				{
					ImGui::HelpMarker(uiVar->Description);
					ImGui::SameLine(0, 2);
				}
			}

			ImGui::Text("%s", name);
		}

		// Column: Variable value
		if (ImGui::TableNextColumn())
		{
			char inputID[256];
			sprintf_s(inputID, sizeof inputID, "###%s_%i", varInfo->GetName(), i);

			// Stretch input field
			ImGui::SetNextItemWidth(-1);

			// Keep track if variable value was actually changed
			bool edited = false;

			// Default widget is always drawn for texture variable, it can't be overriden
			bool useDefaultWidget =
				var->IsTexture() ||
				uiVar == nullptr ||
				uiVar->WidgetType == ShaderUIConfig::Default;

			if (useDefaultWidget)
			{
				switch (varInfo->GetType())
				{
				case rage::EFFECT_VALUE_INT:
				case rage::EFFECT_VALUE_INT1:		edited = ImGui::InputInt(inputID, var->GetValuePtr<int>());						break;
				case rage::EFFECT_VALUE_INT2:		edited = ImGui::InputInt2(inputID, var->GetValuePtr<int>());					break;
				case rage::EFFECT_VALUE_INT3:		edited = ImGui::InputInt3(inputID, var->GetValuePtr<int>());					break;
				case rage::EFFECT_VALUE_FLOAT:		edited = ImGui::DragFloat(inputID, var->GetValuePtr<float>(), 0.1f);			break;
				case rage::EFFECT_VALUE_VECTOR2:	edited = ImGui::DragFloat2(inputID, var->GetValuePtr<float>(), 0.1f);			break;
				case rage::EFFECT_VALUE_VECTOR3:	edited = ImGui::DragFloat3(inputID, var->GetValuePtr<float>(), 0.1f);			break;
				case rage::EFFECT_VALUE_VECTOR4:	edited = edited = ImGui::DragFloat4(inputID, var->GetValuePtr<float>(), 0.1f);	break;
				case rage::EFFECT_VALUE_BOOL:		edited = SlGui::Checkbox(inputID, var->GetValuePtr<bool>());					break;

				case rage::EFFECT_VALUE_TEXTURE:
				{
					rage::grcTexture* newTexture = TexturePicker(inputID, var->GetValuePtr<rage::grcTexture>());
					if (newTexture)
					{
						var->SetTexture(newTexture);
						edited = true;
					}
				}
				break;

				// Unsupported types for now:
				// - EFFECT_VALUE_MATRIX34
				// - EFFECT_VALUE_MATRIX44
				// - EFFECT_VALUE_STRING
				default:
					ImGui::Dummy(ImVec2(0, 0));
					break;
				}
			}
			else // Widget override
			{
				auto& sliderFloatParams = uiVar->WidgetParams.SliderFloat;
				auto& toggleFloatParams = uiVar->WidgetParams.ToggleFloat;
				switch (uiVar->WidgetType)
				{
				case ShaderUIConfig::SliderFloat:
				{
					edited = ImGui::SliderFloat(inputID, var->GetValuePtr<float>(), sliderFloatParams.Min, sliderFloatParams.Max);
					break;
				}
				case ShaderUIConfig::SliderFloatLogarithmic:
				{
					edited = ImGui::SliderFloat(
						inputID, var->GetValuePtr<float>(), sliderFloatParams.Min, sliderFloatParams.Max, "%.3f",
						ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
					break;
				}
				case ShaderUIConfig::ToggleFloat:
				{
					bool isChecked = rage::AlmostEquals(var->GetValue<float>(), toggleFloatParams.Enabled);
					if (SlGui::Checkbox(inputID, &isChecked))
					{
						var->SetValue<float>(isChecked ? toggleFloatParams.Enabled : toggleFloatParams.Disabled);
						edited = true;
					}
					break;
				}

				default:
					ImGui::TextColored(ImVec4(1, 0, 0, 1), "Widget %s is not implemented", Enum::GetName(uiVar->WidgetType));
					break;
				}
			}

			if (edited)
				HandleMaterialValueChange(i, var, varInfo);
		}
	}
	ImGui::EndTable();  // MaterialEditor_Properties
	//SlGui::EndPadded(); // MaterialEditor_Properties_Padding
}

void rageam::integration::MaterialEditor::DrawMaterialOptions() const
{
	//if (!SlGui::BeginPadded("MaterialEditor_Options_Padding", ImVec2(6, 6)))
	//{
	//	SlGui::EndPadded();
	//	return;
	//}

	rage::grmShader* material = GetSelectedMaterial();

	// Draw Bucket
	static constexpr ConstString s_BucketNames[] =
	{
		"0 - Diffuse",
		"1 - Alpha",
		"2 - Decal",
		"3 - Cutout, Shadows",
		"4 - No Splash",
		"5 - No Water",
		"6 - Water",
		"7 - Lens Distortion",
	};

	ImGui::Text("Draw Bucket");
	ImGui::SameLine();
	ImGui::HelpMarker(
		"Change only if necessary. Bucket will be automatically picked from shader preset (.sps)\n"
		"Bucket descriptions:\n"
		"0 - Solid objects (no alpha)\n"
		"1 - Alpha without shadows, commonly used on glass\n"
		"2 - Decals with alpha, no shadows\n"
		"3 - Cutout with alpha + shadows, used on fences.\n"
		"4 - Used only on single preset 'vehicle_nosplash.sps'.\n"
		"5 - Used only on single preset 'vehicle_nowater.sps'.\n"
		"6 - Water shaders\n"
		"7 - Lens distortion (rendered last to apply effect on all objects in scene) 'glass_displacement.sps'");

	int drawBucket = material->GetDrawBucket();
	bool drawBucketEdited = false;
	if (ImGui::AddSubButtons("DRAW_BUCKET_ADD_SUB", drawBucket, 0, 7, false))
		drawBucketEdited = true;
	ImGui::SameLine(0, 1);
	if (ImGui::Combo("###DRAW_BUCKET", &drawBucket, s_BucketNames, IM_ARRAYSIZE(s_BucketNames)))
		drawBucketEdited = true;

	if (drawBucketEdited)
	{
		material->SetDrawBucket(drawBucket);
		m_Context->Drawable->ComputeBucketMask();
	}

	// Render Flags
	SlGui::CategoryText("Render Flags");
	rage::grcRenderMask& renderMask = material->GetDrawBucketMask();
	u32 renderFlags = renderMask.GetRenderFlags();
	if (widgets::EnumFlags<rage::grcRenderFlags>("RENDER_FLAGS", "LF", &renderFlags))
	{
		ImGui::CheckboxFlags("Visibility", &renderFlags, rage::RF_VISIBILITY);
		ImGui::CheckboxFlags("Shadows", &renderFlags, rage::RF_SHADOWS);
		ImGui::CheckboxFlags("Reflections", &renderFlags, rage::RF_REFLECTIONS);
		ImGui::CheckboxFlags("Mirror", &renderFlags, rage::RF_MIRROR);
	}
	renderMask.SetRenderFlags(renderFlags);

	//SlGui::EndPadded();
}

void rageam::integration::MaterialEditor::StoreMaterialValue(u16 materialIndex, u16 varIndex)
{
	rage::grmShader* material = GetShaderGroup()->GetShader(materialIndex);
	rage::grcInstanceVar* var = material->GetVarByIndex(varIndex);
	rage::grcEffectVar* varInfo = material->GetEffect()->GetVarByIndex(varIndex);

	VarBlob& blob = m_MaterialValues[materialIndex].InsertAt(varInfo->GetNameHash(), VarBlob());
	if (var->IsTexture())
	{
		blob.Texture = var->GetTexture();
	}
	else
	{
		// grcInstanceVar uses 16 byte blocks to store values
		u32 dataSize = 16 * var->GetValueCount();
		memcpy(blob.Data, var->GetValuePtr<char>(), dataSize);
	}
}

void rageam::integration::MaterialEditor::RestoreMaterialValue(u16 materialIndex, u16 varIndex)
{
	rage::grmShader* material = GetShaderGroup()->GetShader(materialIndex);
	rage::grcInstanceVar* var = material->GetVarByIndex(varIndex);
	rage::grcEffectVar* varInfo = material->GetEffect()->GetVarByIndex(varIndex);

	VarBlob* blob = m_MaterialValues[materialIndex].TryGetAt(varInfo->GetNameHash());
	if (blob) // We have saved state for this value
	{
		if (var->IsTexture())
		{
			var->SetTexture(blob->Texture);
		}
		else
		{
			u32 dataSize = 16 * var->GetValueCount(); // Same as before
			memcpy(var->GetValuePtr<char>(), blob->Data, dataSize);
		}
	}
}

rageam::integration::MaterialEditor::MaterialEditor(ModelSceneContext* sceneContext)
{
	m_UIConfig.Load();
	m_UIConfigWatcher.SetEntry(file::PathConverter::WideToUtf8(m_UIConfig.GetFilePath()));

	m_Context = sceneContext;
}

void rageam::integration::MaterialEditor::Render()
{
	if (!IsOpen || !m_Context->Drawable)
		return;

	// Load presets
	if (!m_ShaderPresets.Any())
	{
		InitializePresetSearch();
	}

	bool isOpen = IsOpen;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	bool window = ImGui::Begin(ICON_AM_BALL" Material Editor", &isOpen/*, ImGuiWindowFlags_MenuBar*/);
	ImGui::PopStyleVar(1); // WindowPadding
	IsOpen = isOpen;

	if (window)
	{
		// Reload UI config if file was changed
		if (m_UIConfigWatcher.GetChangeOccuredAndReset())
		{
			m_UIConfig.Load();
		}

		//if (ImGui::BeginMenuBar())
		//{
		//	static bool s_ShowShaders = true;
		//	if (SlGui::ToggleButton("Show Shaders", s_ShowShaders))
		//	{

		//	}

		//	ImGui::EndMenuBar();
		//}

		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
		if (ImGui::BeginTable("MaterialEditor_SplitView", 3, ImGuiTableFlags_Resizable))
		{
			float defaultListWidth = ImGui::GetTextLineHeight() * 10;
			ImGui::TableSetupColumn("MaterialEditor_ListAndPreview", ImGuiTableColumnFlags_WidthFixed, defaultListWidth);
			ImGui::TableSetupColumn("MaterialEditor_Properties", ImGuiTableColumnFlags_WidthStretch, 0);
			ImGui::TableSetupColumn("MaterialEditor_Shaders");

			// Column: Preview & Mat List
			if (ImGui::TableNextColumn())
			{
				DrawMaterialList();
			}

			// Column: Mat Properties
			if (ImGui::TableNextColumn())
			{
				//SlGui::BeginPadded("MATERIAL_PROPERTIES_PADDING", ImVec2(4, 4));
				ImGui::Text("Shader: %s.fxc", GetSelectedMaterial()->GetEffect()->GetName());
				ImGui::SameLine();
				ImGui::HelpMarker(
					"Shader (.fxc) name, not preset (.sps)!\n"
					"Preset only contains shader and settings (for example draw bucket index).");
				if (ImGui::BeginTabBar("SHADERS_TAB_BAR"))
				{
					if (ImGui::BeginTabItem("Variables"))
					{
						DrawMaterialVariables();
						ImGui::EndTabItem();
					}

					if (ImGui::BeginTabItem("Options"))
					{
						DrawMaterialOptions();
						ImGui::EndTabItem();
					}

					ImGui::EndTabBar();
				}
				//SlGui::EndPadded();
			}

			// Column: Shaders
			if (ImGui::TableNextColumn())
			{
				DrawShaderSearch();
			}

			ImGui::EndTable();
		}
		ImGui::PopStyleVar(1); // CellPadding
	}
	ImGui::End();
}

void rageam::integration::MaterialEditor::Reset()
{
	m_SelectedMaterialIndex = 0;
	m_MaterialValues.Clear();

	if (!m_Context->Drawable)
		return;

	// Allocate var store for shader group
	rage::grmShaderGroup* shaderGroup = m_Context->Drawable->GetShaderGroup();
	// Store current material values
	m_MaterialValues.Resize(shaderGroup->GetShaderCount());
	for (u16 i = 0; i < shaderGroup->GetShaderCount(); i++)
	{
		rage::grmShader* shader = shaderGroup->GetShader(i);
		for (u16 k = 0; k < shader->GetVarCount(); k++)
		{
			StoreMaterialValue(i, k);
		}
	}
}

#endif