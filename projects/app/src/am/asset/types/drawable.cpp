#include "drawable.h"

#include "hotdrawable.h"
#include "am/asset/factory.h"
#include "am/file/iterator.h"
#include "am/graphics/buffereditor.h"
#include "am/graphics/geomprimitives.h"
#include "am/graphics/meshsplitter.h"
#include "rage/grcore/effectmgr.h"
#include "am/xml/iterator.h"
#include "rage/math/math.h"
#include "rage/physics/bounds/boundprimitives.h"
#include "rage/physics/bounds/boundcomposite.h"
#include "rage/physics/bounds/boundgeometry.h"
#include "rage/physics/bounds/boundbvh.h"

bool rageam::asset::DrawableTxd::TryCompile()
{
	Dict = nullptr;

	rage::grcTextureDictionary* dict = new rage::grcTextureDictionary();
	if (!Asset->CompileToGame(dict))
	{
		AM_ERRF(L"DrawableTxd::TryCompile() -> Failed to compile TXD '%ls'",
			Asset->GetDirectoryPath().GetCStr());
		delete dict;
		return false;
	}

	Dict = rage::pgPtr(dict);
	return true;
}

void rageam::asset::MaterialTune::Param::Serialize(XmlHandle& xml) const
{
	XML_SET_ATTR(xml, Name);
	XML_SET_ATTR(xml, Type);

	switch (Type)
	{
	case Unsupported:
	case None:															break;
	case String:	xml.SetValue(std::any_cast<string>(Value));			break;
	case Bool:		xml.SetValue(std::any_cast<bool>(Value));			break;
	case Float:		xml.SetValue(std::any_cast<float>(Value));			break;
	case Vec2F:		xml.SetValue(std::any_cast<rage::Vector2>(Value));	break;
	case Vec3F:		xml.SetValue(std::any_cast<rage::Vector3>(Value));	break;
	case Vec4F:		xml.SetValue(std::any_cast<rage::Vector4>(Value));	break;
	case Color:		xml.SetColorHex(std::any_cast<u32>(Value));			break;
	}
}

void rageam::asset::MaterialTune::Param::Deserialize(const XmlHandle& xml)
{
	XML_GET_ATTR(xml, Name);
	XML_GET_ATTR(xml, Type);

	// We can't use XmlHandle::GetValue functions with std::any so use this way with macro
#define GET_VALUE(type) { type v; xml.GetValue(v); Value = std::move(v); } MACRO_END

	switch (Type)
	{
	case Unsupported:
	case None:											break;
	case Bool:		GET_VALUE(bool);					break;
	case Float:		GET_VALUE(float);					break;
	case Vec2F:		GET_VALUE(rage::Vector2);			break;
	case Vec3F:		GET_VALUE(rage::Vector3);			break;
	case Vec4F:		GET_VALUE(rage::Vector4);			break;
	case Color:		Value = xml.GetColorHex();			break;
	case String:	Value = string(xml.GetText(false)); break; // We allow empty/null strings
	}

#undef GET_VALUE
}

void rageam::asset::MaterialTune::Param::CopyValue(rage::grcInstanceVar* var)
{
	switch (Type)
	{
	case Unsupported:
	case None:													break;
	case String:	Value = string("");							break;
	case Bool:		Value = var->GetValue<bool>();				break;
	case Float:		Value = var->GetValue<float>();				break;
	case Vec2F:		Value = var->GetValue<rage::Vector2>();		break;
	case Vec3F:		Value = var->GetValue<rage::Vector3>();		break;
	case Vec4F:		Value = var->GetValue<rage::Vector4>();		break;
	case Color:		Value = 0;									break;
	}

	// Retrieve texture name
	if (var->IsTexture() && var->GetTexture())
	{
		// Missing name shouldn't go to file
		ConstString textureName = TxdAsset::UndecorateMissingTextureName(var->GetTexture());
		Value = string(textureName);
	}
}

void rageam::asset::MaterialTune::Deserialize(const XmlHandle& node)
{
	SceneTune::Deserialize(node);
	XML_GET_ATTR(node, Effect);
	XML_GET_ATTR(node, DrawBucket);

	for (const XmlHandle& xParam : XmlIterator(node, "Param"))
	{
		Param param;
		param.Deserialize(xParam);
		Params.Emplace(std::move(param));
	}
}

void rageam::asset::MaterialTune::Serialize(XmlHandle& node) const
{
	SceneTune::Serialize(node);
	XML_SET_ATTR(node, Effect);
	XML_SET_ATTR(node, DrawBucket);

	for (const Param& param : Params)
	{
		XmlHandle xParam = node.AddChild("Param");
		param.Serialize(xParam);
	}
}

rage::grcEffect* rageam::asset::MaterialTune::LookupEffect() const
{
	rage::grcEffect* effect = rage::grcEffectMgr::FindEffect(Effect);
	if (!effect)
	{
		AM_ERRF("MaterialTune::LookupEffect() -> Shader effect with name '%s.fxc' doesn't exists.", Effect.GetCStr());
		return nullptr;
	}
	return effect;
}

rage::grcEffect* rageam::asset::MaterialTune::LookupEffectOrUseDefault() const
{
	rage::grcEffect* effect = LookupEffect();
	if (!effect)
	{
		AM_WARNINGF("MaterialTune::LookupEffectOrUseDefault() -> Failed to find shader '%s'.fxc, using '%s'.fxc instead.",
			Effect.GetCStr(), DEFAULT_SHADER);

		effect = rage::grcEffectMgr::FindEffect(DEFAULT_SHADER);
	}
	return effect;
}

void rageam::asset::MaterialTune::InitFromEffect()
{
	// Initialize from effect shader template
	rage::grcEffect* effect = LookupEffectOrUseDefault();
	InitFromShader(&effect->GetInstanceDataTemplate());
}

void rageam::asset::MaterialTune::InitFromShader(const rage::grcInstanceData* shader)
{
	rage::grcEffect* effect = shader->GetEffect();

	u16 effectVarCount = effect->GetVarCount();

	Params.Destroy();
	Params.Reserve(effectVarCount);

	for (u16 i = 0; i < effectVarCount; i++)
	{
		rage::grcEffectVar* varInfo = effect->GetVar(i);
		rage::grcInstanceVar* var = shader->GetVar(i);

		Param param;
		param.Name = varInfo->GetName();
		param.Type = grcEffectVarTypeToParamType[varInfo->GetType()];
		param.CopyValue(var);

		Params.Emplace(std::move(param));
	}

	Effect = effect->GetName();
	DrawBucket = shader->GetDrawBucket();
}

void rageam::asset::MaterialTune::SetTextureNamesFromSceneMaterial(const graphics::SceneMaterial* sceneMaterial)
{
	rage::grcEffect* effect = LookupEffectOrUseDefault();

	u16 effectVarCount = effect->GetVarCount();

	AM_ASSERT(Params.GetSize() == effectVarCount,
		"MaterialTune::SetTextureNamesFromSceneMaterial() -> Param count don't match effect.");

	for (u16 i = 0; i < effectVarCount; i++)
	{
		rage::grcEffectVar* varInfo = effect->GetVar(i);
		if (!varInfo->IsTexture())
			continue;

		graphics::eMaterialTexture textureType = graphics::MT_Unknown;

		// We don't have concrete way to detect sampler type right now so just search in string
		string varName = varInfo->GetName();
		if (varName.Contains("diff", true))			textureType = graphics::MT_Diffuse;
		else if (varName.Contains("norm", true))	textureType = graphics::MT_Normal;
		else if (varName.Contains("bump", true))	textureType = graphics::MT_Normal;
		else if (varName.Contains("spec", true))	textureType = graphics::MT_Specular;

		// Attempt to match automatically tune texture slot to shader slot
		if (textureType != graphics::MT_Unknown)
		{
			Params[i].Value = string(sceneMaterial->GetTextureName(textureType));
		}
		else
		{
			AM_WARNINGF(
				"MaterialTune::SetTextureNamesFromSceneMaterial() -> Unable to resolve texture type for shader parameter '%s'",
				varInfo->GetName());
		}
	}
}

int rageam::asset::MaterialTuneGroup::IndexOf(const graphics::Scene* scene, ConstString itemName) const
{
	graphics::SceneMaterial* material = scene->GetMaterialByName(itemName);
	return material ? material->GetIndex() : -1;
}

rageam::asset::SceneTunePtr rageam::asset::MaterialTuneGroup::CreateTune() const
{
	return std::make_shared<MaterialTune>();
}

rageam::asset::SceneTunePtr rageam::asset::MaterialTuneGroup::CreateDefaultTune(graphics::Scene* scene, u16 itemIndex) const
{
	graphics::SceneMaterial* sceneMaterial = scene->GetMaterial(itemIndex);

	ConstString matchedShader = DEFAULT_SHADER;

	// Pick default shader based on texture maps available
	bool hasSpec = sceneMaterial->HasTextureInSlot(graphics::MT_Specular);
	bool hasNormal = sceneMaterial->HasTextureInSlot(graphics::MT_Normal);
	if (hasSpec && hasNormal)
		matchedShader = "normal_spec";
	else if (hasSpec)
		matchedShader = "spec";
	else if (hasNormal)
		matchedShader = "normal";

	amPtr<MaterialTune> materialTune = std::make_shared<MaterialTune>(sceneMaterial->GetName(), matchedShader);
	materialTune->InitFromEffect();
	materialTune->SetTextureNamesFromSceneMaterial(sceneMaterial);
	return materialTune;
}

ConstString rageam::asset::MaterialTuneGroup::GetItemName(graphics::Scene* scene, u16 itemIndex) const
{
	return scene->GetMaterial(itemIndex)->GetName();
}

u16 rageam::asset::MaterialTuneGroup::GetItemCount(graphics::Scene* scene) const
{
	return scene->GetMaterialCount();
}

void rageam::asset::ModelTune::Serialize(XmlHandle& node) const
{
	SceneTune::Serialize(node);
	XML_SET_CHILD_VALUE(node, UseAsBound);
	XML_SET_CHILD_VALUE(node, CreateBone);
}

void rageam::asset::ModelTune::Deserialize(const XmlHandle& node)
{
	SceneTune::Deserialize(node);
	XML_GET_CHILD_VALUE(node, UseAsBound);
	XML_GET_CHILD_VALUE(node, CreateBone);
}

int rageam::asset::ModelTuneGroup::IndexOf(const graphics::Scene* scene, ConstString itemName) const
{
	graphics::SceneNode* node = scene->GetNodeByName(itemName);
	return node ? node->GetIndex() : -1;
}

rageam::asset::SceneTunePtr rageam::asset::ModelTuneGroup::CreateTune() const
{
	return std::make_shared<ModelTune>();
}

rageam::asset::SceneTunePtr rageam::asset::ModelTuneGroup::CreateDefaultTune(graphics::Scene* scene, u16 itemIndex) const
{
	return std::make_shared<ModelTune>();
}

ConstString rageam::asset::ModelTuneGroup::GetItemName(graphics::Scene* scene, u16 itemIndex) const
{
	return scene->GetNode(itemIndex)->GetName();
}

u16 rageam::asset::ModelTuneGroup::GetItemCount(graphics::Scene* scene) const
{
	return scene->GetNodeCount();
}

void rageam::asset::LightTune::InitFromSceneLight(const graphics::SceneNode* lightNode)
{
	graphics::SceneLight* sceneLight = lightNode->GetLight();

	switch (sceneLight->GetType())
	{
	case graphics::SceneLight_Point:	Type = Point;	break;
	case graphics::SceneLight_Spot:		Type = Spot;	break;
	}

	ColorRGB = sceneLight->GetColor();
	Falloff = Type == LIGHT_TYPE_SPOT ? 8.0f : 2.5f; // Make light longer if spot light is chosen
	FalloffExponent = 32;
	ConeOuterAngle = rage::RadToDeg(sceneLight->GetOuterConeAngle());
	ConeInnerAngle = rage::RadToDeg(sceneLight->GetInnerConeAngle());
	Flags = LF_ENABLE_SHADOWS;
	TimeFlags = LIGHT_TIME_ALWAYS_MASK;
	CoronaZBias = 0.1;
	CoronaSize = 5.0f;
	Intensity = 75.0f;
	CapsuleLength = 1.0f;
	ShadowNearClip = 0.01;
	CullingPlaneNormal = { 0, 1, 0 };
	CullingPlaneOffset = 1;
	VolumeOuterColorRGB = graphics::COLOR_WHITE;
	VolumeOuterExponent = 1;
	VolumeSizeScale = 1.0f;

	Mat44V transform = lightNode->GetLocalTransform();
	Position = rage::Vec3V(transform.Pos);
	Direction = -rage::Vec3V(transform.Up);
	Tangent = rage::Vector3(transform.Right);
}

void rageam::asset::LightTune::FromLightAttr(const CLightAttr* attr)
{
	Type = LightType(attr->Type); // Enum fully matches
	Position = attr->Position;
	Direction = attr->Direction;
	Tangent = attr->Tangent;
	ColorRGB = graphics::ColorU32(attr->ColorR, attr->ColorG, attr->ColorB);
	Flashiness = attr->Flashiness;
	Flags = attr->Flags;
	TimeFlags = attr->TimeFlags;
	Intensity = attr->Intensity;
	Falloff = attr->Falloff;
	FalloffExponent = attr->FallofExponent;
	CullingPlaneNormal = attr->CullingPlaneNormal;
	CullingPlaneOffset = attr->CullingPlaneOffset;
	ShadowBlur = attr->ShadowBlur;
	ShadowNearClip = attr->ShadowNearClip;
	CoronaSize = attr->CoronaSize;
	CoronaIntensity = attr->CoronaIntensity;
	CoronaZBias = attr->CoronaZBias;
	LightHash = attr->LightHash;
	// ProjectedTexture = attr->ProjectedTexture;
	LightFadeDistance = attr->LightFadeDistance;
	ShadowFadeDistance = attr->ShadowFadeDistance;
	SpecularFadeDistance = attr->SpecularFadeDistance;
	VolumetricFadeDistance = attr->VolumetricFadeDistance;

	VolumeIntensity = attr->VolumeIntensity;
	VolumeSizeScale = attr->VolumeSizeScale;
	VolumeOuterIntensity = attr->VolumeOuterIntensity;
	VolumeOuterExponent = attr->VolumeOuterExponent;
	VolumeOuterColorRGB = graphics::ColorU32(
		attr->VolumeOuterColorR, attr->VolumeOuterColorG, attr->VolumeOuterColorB);
	ConeInnerAngle = attr->ConeInnerAngle;
	ConeOuterAngle = attr->ConeOuterAngle;

	CapsuleLength = attr->Extent.X;
}

int rageam::asset::LightTuneGroup::IndexOf(const graphics::Scene* scene, ConstString itemName) const
{
	for (u16 i = 0; i < scene->GetLightCount(); i++)
	{
		if (String::Equals(scene->GetLight(i)->GetName(), itemName))
			return i;
	}
	return -1;
}

rageam::asset::SceneTunePtr rageam::asset::LightTuneGroup::CreateTune() const
{
	return std::make_shared<LightTune>();
}

rageam::asset::SceneTunePtr rageam::asset::LightTuneGroup::CreateDefaultTune(graphics::Scene* scene, u16 itemIndex) const
{
	amPtr<LightTune> lightTune = std::make_shared<LightTune>();
	lightTune->InitFromSceneLight(scene->GetLight(itemIndex));
	return lightTune;
}

ConstString rageam::asset::LightTuneGroup::GetItemName(graphics::Scene* scene, u16 itemIndex) const
{
	return scene->GetLight(itemIndex)->GetName();
}

u16 rageam::asset::LightTuneGroup::GetItemCount(graphics::Scene* scene) const
{
	return scene->GetLightCount();
}

void rageam::asset::LightTune::Serialize(XmlHandle& node) const
{
	SceneTune::Serialize(node);

	XML_SET_CHILD_VALUE(node, Type);
	node.AddChild("Position").SetValue(Position);
	node.AddChild("Direction").SetValue(Direction);
	node.AddChild("Tangent").SetValue(Tangent);
	node.AddChild("ColorRGB").SetColorHex(ColorRGB);
	XML_SET_CHILD_VALUE(node, Flashiness);
	node.AddChild("Flags", String::FormatTemp("%X", Flags));
	node.AddChild("TimeFlags", String::FormatTemp("%X", TimeFlags));
	XML_SET_CHILD_VALUE(node, Intensity);
	XML_SET_CHILD_VALUE(node, Falloff);
	XML_SET_CHILD_VALUE(node, FalloffExponent);
	node.AddChild("CullingPlaneNormal").SetValue(CullingPlaneNormal);
	XML_SET_CHILD_VALUE(node, CullingPlaneOffset);
	XML_SET_CHILD_VALUE(node, ShadowBlur);
	XML_SET_CHILD_VALUE(node, ShadowNearClip);
	XML_SET_CHILD_VALUE(node, CoronaSize);
	XML_SET_CHILD_VALUE(node, CoronaIntensity);
	XML_SET_CHILD_VALUE(node, CoronaZBias);
	//XML_SET_CHILD_VALUE(xLight, LightHash);
	//XML_SET_CHILD_VALUE(xLight, ProjectedTexture);
	XML_SET_CHILD_VALUE(node, LightFadeDistance);
	XML_SET_CHILD_VALUE(node, ShadowFadeDistance);
	XML_SET_CHILD_VALUE(node, SpecularFadeDistance);
	XML_SET_CHILD_VALUE(node, VolumetricFadeDistance);

	XML_SET_CHILD_VALUE(node, VolumeIntensity);
	XML_SET_CHILD_VALUE(node, VolumeSizeScale);
	XML_SET_CHILD_VALUE(node, VolumeOuterIntensity);
	XML_SET_CHILD_VALUE(node, VolumeOuterExponent);
	node.AddChild("VolumeOuterColorRGB").SetColorHex(VolumeOuterColorRGB);
	XML_SET_CHILD_VALUE(node, ConeInnerAngle);
	XML_SET_CHILD_VALUE(node, ConeOuterAngle);

	XML_SET_CHILD_VALUE(node, CapsuleLength);
}

void rageam::asset::LightTune::Deserialize(const XmlHandle& node)
{
	SceneTune::Deserialize(node);

	// TODO: This code is kinda bad

	auto scanHex = [](ConstString str) -> u32
		{
			float val = 0;
			AM_ASSERT(sscanf_s(str, "%X", &val) > 0, "LightTune::Deserialize() -> Failed to parse hex value '%s'", str);
			return val;
		};

	XML_GET_CHILD_VALUE(node, Type);
	node.GetChild("Position").GetValue(Position);
	node.GetChild("Direction").GetValue(Direction);
	node.GetChild("Tangent").GetValue(Tangent);
	ColorRGB = node.GetChild("ColorRGB").GetColorHex();
	XML_GET_CHILD_VALUE(node, Flashiness);
	Flags = scanHex(node.GetChild("Flags").GetText());
	TimeFlags = scanHex(node.GetChild("TimeFlags").GetText());
	XML_GET_CHILD_VALUE(node, Intensity);
	XML_GET_CHILD_VALUE(node, Falloff);
	XML_GET_CHILD_VALUE(node, FalloffExponent);
	node.GetChild("CullingPlaneNormal").SetValue(CullingPlaneNormal);
	XML_GET_CHILD_VALUE(node, CullingPlaneOffset);
	XML_GET_CHILD_VALUE(node, ShadowBlur);
	XML_GET_CHILD_VALUE(node, ShadowNearClip);
	XML_GET_CHILD_VALUE(node, CoronaSize);
	XML_GET_CHILD_VALUE(node, CoronaIntensity);
	XML_GET_CHILD_VALUE(node, CoronaZBias);
	//XML_SET_CHILD_VALUE(xLight, LightHash);
	//XML_SET_CHILD_VALUE(xLight, ProjectedTexture);
	XML_GET_CHILD_VALUE(node, LightFadeDistance);
	XML_GET_CHILD_VALUE(node, ShadowFadeDistance);
	XML_GET_CHILD_VALUE(node, SpecularFadeDistance);
	XML_GET_CHILD_VALUE(node, VolumetricFadeDistance);

	XML_GET_CHILD_VALUE(node, VolumeIntensity);
	XML_GET_CHILD_VALUE(node, VolumeSizeScale);
	XML_GET_CHILD_VALUE(node, VolumeOuterIntensity);
	XML_GET_CHILD_VALUE(node, VolumeOuterExponent);
	VolumeOuterColorRGB = node.GetChild("VolumeOuterColorRGB").GetColorHex();
	XML_GET_CHILD_VALUE(node, ConeInnerAngle);
	XML_GET_CHILD_VALUE(node, ConeOuterAngle);

	XML_GET_CHILD_VALUE(node, CapsuleLength);
}

void rageam::asset::LodGroupTune::Serialize(XmlHandle& node) const
{
	node.AddComment("Maximum LOD render distance's (L0 to L4)");

	// Serialize threshold as vector4
	XmlHandle xLodThreshold = node.AddChild("LodThreshold");
	rage::Vector4 lodThreshold;
	lodThreshold.FromArray(LodThreshold);
	xLodThreshold.SetValue(lodThreshold);

	Models.Serialize(node);
}

void rageam::asset::LodGroupTune::Deserialize(const XmlHandle& node)
{
	// Deserialize threshold from vector4
	XmlHandle xLodThreshold = node.GetChild("LodThreshold");
	rage::Vector4 lodThreshold;
	xLodThreshold.GetValue(lodThreshold);
	lodThreshold.ToArray(LodThreshold);

	// Make sure that lod distances are incrementally continuous
	for (int i = 0; i < MAX_LOD - 1; i++)
	{
		float current = LodThreshold[i];
		float next = LodThreshold[i + 1];

		if (next >= current)
			return;

		AM_WARNINGF("LOD %i distance threshold (%g) is less than previos LOD %i (%g), clamping value...",
			i + 1, next, i, current);

		LodThreshold[i + 1] = current;
	}

	Models.Deserialize(node);
}

void rageam::asset::DrawableTune::Serialize(XmlHandle& node) const
{
	node.AddChild("SceneFile", String::ToUtf8Temp(SceneFileName));

	XmlHandle xLodGroup = node.AddChild("LodGroup");

	Lods.Serialize(xLodGroup);
	Materials.Serialize(node);
	Lights.Serialize(node);
}

void rageam::asset::DrawableTune::Deserialize(const XmlHandle& node)
{
	ConstString fileName;
	node.GetChildText("SceneFile", &fileName, false);
	SceneFileName = String::Utf8ToWideTemp(fileName);

	XmlHandle xLodGroup = node.GetChild("LodGroup");

	Lods.Deserialize(xLodGroup);
	Materials.Deserialize(node);
	Lights.Deserialize(node);
}

rage::grmModel* rageam::asset::DrawableAssetMap::GetModelFromScene(rage::rmcDrawable* drawable, u16 sceneNodeIndex) const
{
	const ModelHandle& handle = SceneNodeToModel[sceneNodeIndex];
	if (handle.Index == u16(-1))
		return nullptr;
	return drawable->GetLodGroup().GetLod(handle.Lod)->GetModel(handle.Index);
}

rage::crBoneData* rageam::asset::DrawableAssetMap::GetBoneFromScene(const rage::rmcDrawable* drawable, u16 sceneNodeIndex) const
{
	u16 boneIndex = SceneNodeToBone[sceneNodeIndex];
	if (boneIndex == u16(-1))
		return nullptr;
	return drawable->GetSkeletonData()->GetBone(boneIndex);
}

rage::phBound* rageam::asset::DrawableAssetMap::GetBoundFromScene(const gtaDrawable* drawable, u16 sceneNodeIndex) const
{
	/*u16 boundIndex = SceneNodeToBound[sceneNodeIndex];
	if (boundIndex == u16(-1))
		return nullptr;
	return ((rage::phBoundComposite*)drawable->GetBound().Get())->GetBound(boundIndex).Get();*/
	// TODO: ...
	return nullptr;
}

CLightAttr* rageam::asset::DrawableAssetMap::GetLightFromScene(gtaDrawable* drawable, u16 sceneNodeIndex) const
{
	u16 lightAttrIndex = SceneNodeToLightAttr[sceneNodeIndex];
	if (lightAttrIndex == u16(-1))
		return nullptr;
	return drawable->GetLight(lightAttrIndex);
}

bool rageam::asset::DrawableAsset::CacheEffects()
{
	m_EffectCache.Destroy();

	MaterialTuneGroup& matGroup = m_DrawableTune.Materials;
	for (const auto& material : matGroup)
	{
		u32 effectHash = Hash(material->Effect);

		// Effect was already cached
		if (m_EffectCache.ContainsAt(effectHash))
			continue;

		rage::grcEffect* effect = rage::grcEffectMgr::FindEffectByHashKey(effectHash);
		if (!effect)
		{
			AM_ERRF("CollectMaterialInfo() -> Shader effect with name '%s.fxc' doesn't exists.", material->Effect.GetCStr());
			return false;
		}

		m_EffectCache.ConstructAt(effectHash, effect);
	}
	return true;
}

void rageam::asset::DrawableAsset::PrepareForConversion()
{
	u16 nodeCount = m_Scene->GetNodeCount();

	CacheEffects();
	m_NodeToModel.Resize(nodeCount);
	m_NodeToBone.Resize(nodeCount);
	m_EmbedDict = nullptr;

	CompiledDrawableMap = std::make_unique<DrawableAssetMap>();
	CompiledDrawableMap->SceneNodeToModel.Resize(nodeCount);
	CompiledDrawableMap->SceneNodeToBound.Resize(nodeCount);
	CompiledDrawableMap->SceneNodeToBone.Resize(nodeCount);
	CompiledDrawableMap->SceneMaterialToShader.Resize(m_Scene->GetMaterialCount());
	CompiledDrawableMap->SceneNodeToLightAttr.Resize(nodeCount);
	// Default everything to -1
	for (auto& handle : CompiledDrawableMap->SceneNodeToModel)	handle = { u16(-1), u16(-1) };
	for (auto& i : CompiledDrawableMap->SceneNodeToBound)		i = {};
	for (u16& i : CompiledDrawableMap->SceneNodeToBone)			i = u16(-1);
	for (u16& i : CompiledDrawableMap->SceneMaterialToShader)	i = u16(-1);
	for (u16& i : CompiledDrawableMap->SceneNodeToLightAttr)	i = u16(-1);
}

void rageam::asset::DrawableAsset::CleanUpConversion()
{
	m_EffectCache.Destroy();
	m_NodeToModel.Destroy();
	m_NodeToBone.Destroy();
	m_EmbedDict = nullptr;
}

rageam::List<rageam::asset::DrawableAsset::SplittedGeometry> rageam::asset::DrawableAsset::ConvertSceneGeometry(
	const graphics::SceneGeometry* sceneGeometry, bool skinned) const
{
	List<SplittedGeometry> geometries;

	// Retrieve material & vertex declaration for scene geometry
	const MaterialTune& material = *m_DrawableTune.Materials.Get(sceneGeometry->GetMaterialIndex()); // Material settings from tune.xml file (asset config)
	const EffectInfo& effectInfo = m_EffectCache.GetAt(Hash(material.Effect));	// Cached effect (.fxc shader file)

	const graphics::VertexDeclaration& decl = skinned ? effectInfo.VertexDeclSkin : effectInfo.VertexDecl; // Description of vertex buffer format

	u32 totalVertexCount = sceneGeometry->GetVertexCount();
	u32 totalIndexCount = sceneGeometry->GetIndexCount();

	// AM_DEBUGF("DrawableAsset::ConvertSceneGeometry -> %u vertices; %u indices", totalVertexCount, totalIndexCount);

	// Pack scene geometry attributes into single vertex buffer
	graphics::VertexBufferEditor sceneVertexBuffer(decl);
	sceneVertexBuffer.Init(totalVertexCount);
	sceneVertexBuffer.SetFromGeometry(sceneGeometry);
	if (!sceneVertexBuffer.IsSet(graphics::COLOR, 0))
	{
		sceneVertexBuffer.SetColorSingle(0, graphics::COLOR_WHITE);
		AM_WARNINGF(
			"DrawableAsset::ConvertSceneGeometry() -> Geometry '%u' of mesh '%s' has no vertex color! Using WHITE as default.",
			sceneGeometry->GetIndex(), sceneGeometry->GetParentMesh()->GetParentNode()->GetName());
	}

	// Remap blend indices to skeleton for skinning, they are ignored by VertexBufferEditor::SetFromGeometry
	if (skinned)
	{
		amUniquePtr<rage::Vector4[]> remappedBlendIndices = RemapBlendIndices(sceneGeometry);
		sceneVertexBuffer.SetBlendIndices(remappedBlendIndices.get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
	}

	// Creates grmGeometry from given vertex data and adds it in geometries list
	auto addGeometry = [&decl, &geometries](pVoid vertices, pVoid indices, u32 vertexCount, u32 indexCount, const rage::spdAABB& bb)
		{
			// Create geometry & set vertex data
			rage::grmVertexData vertexData;
			vertexData.Vertices = vertices;
			vertexData.Indices = static_cast<u16*>(indices);
			vertexData.VertexCount = vertexCount;
			vertexData.IndexCount = indexCount;
			vertexData.Info = decl.GrcInfo;

			rage::pgUPtr grmGeometry = new rage::grmGeometryQB();
			grmGeometry->SetVertexData(vertexData);

			// Add resulting geometry in the list
			SplittedGeometry splittedGeometry;
			splittedGeometry.GrmGeometry = std::move(grmGeometry);
			splittedGeometry.BoundingBox = bb;
			geometries.Emplace(std::move(splittedGeometry));
		};

	graphics::SceneData indices;
	sceneGeometry->GetIndices(indices);

	// If indices are 16 bit we can just use them as is, otherwise we'll have to split geometry
	if (indices.Format == DXGI_FORMAT_R16_UINT)
	{
		rage::spdAABB bound = sceneGeometry->GetAABB();
		addGeometry(sceneVertexBuffer.GetBuffer(), indices.Buffer, totalVertexCount, totalIndexCount, bound);
	}
	else
	{
		AM_ASSERT(indices.Format == DXGI_FORMAT_R32_UINT, "Unsupported index buffer format %s", Enum::GetName(indices.Format));

		auto splitVertices = graphics::MeshSplitter::Split(
			sceneVertexBuffer.GetBuffer(), decl.Stride, (u32*)indices.Buffer, totalIndexCount);

		for (const graphics::MeshChunk& chunk : splitVertices)
		{
			// Use vertex editor to compute AABB
			graphics::VertexBufferEditor bufferEditor(decl);
			bufferEditor.Init(chunk.VertexCount, chunk.Vertices.get());

			rage::spdAABB chunkBound;
			bufferEditor.ComputeMinMax_Position(chunkBound.Min, chunkBound.Max);

			addGeometry(chunk.Vertices.get(), chunk.Indices.get(), chunk.VertexCount, chunk.IndexCount, chunkBound);
		}
	}

	return geometries;
}

rage::pgUPtr<rage::grmModel> rageam::asset::DrawableAsset::ConvertSceneModel(const graphics::SceneNode* sceneNode)
{
	ReportProgress(String::FormatTemp(L"Converting model '%hs'", sceneNode->GetName()), 0.2);

	AM_ASSERT(sceneNode->HasMesh(), "DrawableAsset::ConvertSceneModel() -> Given node has no mesh!");

	graphics::SceneMesh* sceneMesh = sceneNode->GetMesh();
	bool hasSkin = sceneMesh->HasSkin();

	rage::grmModel* grmModel = new rage::grmModel();

	AM_DEBUGF("DrawableAsset() -> Converting '%s' into grmModel, '%u' initial geometries to split",
		sceneNode->GetName(), sceneMesh->GetGeometriesCount());

	// Convert every scene geometry to grmGeometry and add them to grmModel geometries array
	u32 geometryIndex = 0; // For setting material
	for (u32 i = 0; i < sceneMesh->GetGeometriesCount(); i++)
	{
		const graphics::SceneGeometry* sceneGeometry = sceneMesh->GetGeometry(i);

		// If scene geometry was pretty high poly then it'll be split on many chunks that fit in 16 bit index buffer
		for (SplittedGeometry& splittedGeometry : ConvertSceneGeometry(sceneGeometry, hasSkin))
		{
			u16 materialIndex = sceneGeometry->GetMaterialIndex();

			grmModel->AddGeometry(splittedGeometry.GrmGeometry, splittedGeometry.BoundingBox);
			grmModel->SetMaterialIndex(geometryIndex++, materialIndex); // Link with ShaderGroup material
		}
	}

	grmModel->ComputeAABB();

	m_NodeToModel[sceneNode->GetIndex()] = grmModel;

	return grmModel;
}

amUniquePtr<rage::Vector4[]> rageam::asset::DrawableAsset::RemapBlendIndices(const graphics::SceneGeometry* sceneGeometry) const
{
	struct IndicesU32 { u8 X, Y, Z, W; };

	graphics::SceneData indicesData;
	AM_ASSERT(sceneGeometry->GetAttribute(indicesData, graphics::BLENDINDICES, 0),
		"DrawableAsset::RemapBlendIndices() -> Geometry had no blend indices data.");

	AM_ASSERT(indicesData.Format == DXGI_FORMAT_R8G8B8A8_UINT,
		"DrawableAsset::RemapBlendIndices() -> Unsupported blend indices format '%s'", Enum::GetName(indicesData.Format));

	// TODO: Cache it for node?
	// Build new bone map, how it works:
	// Blend indices point to bone indices relative to the scene, but after creating skeleton
	// bone order is totally different (we may add some extra bones or change the order)
	// So in order to solve this we map scene index (aka blend index) to skeleton bone index
	u8 sceneBoneToSkeleton[MAX_BONES];
	auto sceneNode = sceneGeometry->GetParentNode();
	for (u16 i = 0; i < sceneNode->GetBoneCount(); i++)
	{
		auto sceneNodeBone = sceneNode->GetBone(i);
		auto sceneNodeBoneIndex = sceneNodeBone->GetIndex();
		sceneBoneToSkeleton[i] = m_NodeToBone[sceneNodeBoneIndex]->GetIndex();
	}

	u32 vertexCount = sceneGeometry->GetVertexCount();

	rage::Vector4* remappedIndices = new rage::Vector4[vertexCount];
	IndicesU32* sourceIndices = indicesData.GetBufferAs<IndicesU32>();
	for (u32 i = 0; i < vertexCount; i++)
	{
		IndicesU32 indices = sourceIndices[i];

		// Remap indices from scene to skeleton
		indices.X = sceneBoneToSkeleton[indices.X];
		indices.Y = sceneBoneToSkeleton[indices.Y];
		indices.Z = sceneBoneToSkeleton[indices.Z];
		indices.W = sceneBoneToSkeleton[indices.W];

		// Convert to float[4]
		constexpr float maxBones = 255.0f;
		constexpr float epsilon = 0.0015f;
		remappedIndices[i] =
		{ // Add small margin to ensure that casting won't cause rounding down
				(float)indices.X / maxBones + epsilon,
				(float)indices.Y / maxBones + epsilon,
				(float)indices.Z / maxBones + epsilon,
				(float)indices.W / maxBones + epsilon,
		};
	}

	return amUniquePtr<rage::Vector4[]>(remappedIndices);
}

bool rageam::asset::DrawableAsset::GenerateSkeleton()
{
	u16 sceneNodeCount = m_Scene->GetNodeCount();

	// Compute bone count, we can't resize skeleton so it must be precomputed
	u16 boneCount = 0;

	for (u16 i = 0; i < m_Scene->GetNodeCount(); i++)
	{
		if (CanBecameBone(m_Scene->GetNode(i)))
			boneCount++;
	}

	// Empty skeleton, skip
	if (boneCount == 0)
		return true;

	bool needRootBone = NeedAdditionalRootBone();
	if (needRootBone)
		boneCount += 1; // 'Insert' root bone

	// TODO: Find out why shader allows up to 255 but after 128 skeleton explodes
	if (boneCount > 128)
	{
		AM_ERRF("DrawableAsset::GenerateSkeleton() -> Too much bones in skeleton (%u), maximum allowed 128.", boneCount);
		return false;
	}

	rage::pgPtr skeletonData = rage::pgPtr(new rage::crSkeletonData());
	skeletonData->Init(boneCount);

	// Setup root bone
	if (needRootBone)
	{
		char rootName[64];
		sprintf_s(rootName, 64, "%s_root", m_Scene->GetName());
		skeletonData->GetBone(0)->SetName(rootName);

		// We have to add it to ensure valid indexing
		CompiledDrawableMap->BoneToSceneNode.Add(u16(-1));
	}

	// Setup bone for every scene node
	u16 boneStartIndex = needRootBone ? 1 : 0;
	for (u16 nodeIndex = 0, boneIndex = boneStartIndex; nodeIndex < sceneNodeCount; nodeIndex++)
	{
		graphics::SceneNode* sceneNode = m_Scene->GetNode(nodeIndex);
		if (!CanBecameBone(sceneNode))
			continue;

		rage::crBoneData* bone = skeletonData->GetBone(boneIndex);
		m_NodeToBone[nodeIndex] = bone;

		// Self index
		bone->SetIndex(boneIndex);

		// TODO: Allow tag override from tune file
		bone->SetName(sceneNode->GetName());

		// Apply transformation
		rage::Vec3V trans = sceneNode->GetTranslation();
		rage::Vec3V scale = sceneNode->GetScale();
		rage::QuatV rot = sceneNode->GetRotation();
		bone->SetTransform(&trans, &scale, &rot);

		// Map graphics::SceneNode -> crBoneData
		CompiledDrawableMap->SceneNodeToBone[nodeIndex] = boneIndex;
		CompiledDrawableMap->BoneToSceneNode.Add(nodeIndex);

		boneIndex++;
	}

	// Setup parent & sibling indices
	// We can't do it in first loop because index of next bone is unknown
	// TODO: Solution - We can store previous bone and link the same way as in GL
	for (u16 nodeIndex = 0; nodeIndex < sceneNodeCount; nodeIndex++)
	{
		rage::crBoneData* bone = m_NodeToBone[nodeIndex];
		if (!bone)
			continue;

		graphics::SceneNode* sceneNode = m_Scene->GetNode(nodeIndex);

		s32 parentBoneIndex = GetNodeBoneIndex(sceneNode->GetParent());
		s32 siblingBoneIndex = -1;
		// For next sibling bone we'll have to search until we find a node that is bone,
		// such scenario may occur when:
		// Node 1: Is Bone
		// Node 2: Is Not Bone
		// Node 3: Is Bone
		// In skeleton Node 3 will appear after Node 1 but as 'NextSibling' in scene it won't
		graphics::SceneNode* nextSublingBoneNode = sceneNode->GetNextSibling();
		while (nextSublingBoneNode)
		{
			s32 nextSiblingBoneIndex = GetNodeBoneIndex(nextSublingBoneNode);
			if (nextSiblingBoneIndex != -1)
			{
				siblingBoneIndex = nextSiblingBoneIndex;
				break;
			}
			nextSublingBoneNode = nextSublingBoneNode->GetNextSibling();
		}

		// We have to link root bone with first child manually
		if (needRootBone && parentBoneIndex == -1)
			parentBoneIndex = 0; // Bones with parent index -1 are getting new root parent at index 0

		bone->SetParentIndex(parentBoneIndex);
		bone->SetNextIndex(siblingBoneIndex);
	}

	skeletonData->Finalize();
	m_Drawable->SetSkeletonData(skeletonData);
	return true;
}

void rageam::asset::DrawableAsset::LinkModelsToSkeleton()
{
	// No skeleton was generated
	if (!m_Drawable->GetSkeletonData())
		return;

	for (u16 i = 0; i < m_Scene->GetNodeCount(); i++)
	{
		rage::crBoneData* bone = m_NodeToBone[i];
		if (!bone) // Node was excluded from skeleton
			continue;

		// Link model with bone
		graphics::SceneNode* sceneNode = m_Scene->GetNode(i);
		rage::grmModel* grmModel = m_NodeToModel[sceneNode->GetIndex()];
		if (!grmModel) // Model will be null for scene nodes without mesh (aka dummies)
			continue;

		if (sceneNode->HasSkin()) // Weighted skinned model
		{
			grmModel->SetIsSkinned(true, sceneNode->GetBoneCount());
		}
		else // Skinned without bone weights, works like just transform
		{
			grmModel->SetBoneIndex(bone->GetIndex());
		}
	}
}

bool rageam::asset::DrawableAsset::NeedAdditionalRootBone() const
{
	graphics::SceneNode* node = m_Scene->GetFirstNode();

	u16 numRootBones = 0;
	while (node)
	{
		if (!IsCollisionNode(node)) // Collision nodes are ignored in drawable and skeleton
		{
			if (node->HasTransform())
				return true;

			numRootBones++;
			if (numRootBones > 1)
				return true;
		}

		node = node->GetNextSibling();
	}

	return false;
}

bool rageam::asset::DrawableAsset::CanBecameBone(const graphics::SceneNode* sceneNode) const
{
	bool forceBone = m_DrawableTune.Lods.Models.Get(sceneNode->GetIndex())->CreateBone;
	if (forceBone)
		return true;

	if (IsCollisionNode(sceneNode))
		return false;

	if (sceneNode->HasLight())
		return false;

	if (sceneNode->HasSkin())
		return true;

	if (sceneNode->HasTransform())
		return true;

	// If any child node is a bone, this node becomes a bone too
	if (sceneNode->HasTransformedChild())
		return true;

	return false;
}

s32 rageam::asset::DrawableAsset::GetNodeBoneIndex(const graphics::SceneNode* sceneNode) const
{
	if (!sceneNode)
		return -1;

	rage::crBoneData* bone = m_NodeToBone[sceneNode->GetIndex()];
	if (!bone)
		return -1;

	return bone->GetIndex();
}

void rageam::asset::DrawableAsset::SetupLodModels()
{
	// TODO: 2 type lod support - hierarchy and separate model

	rage::rmcLodGroup& lodGroup = m_Drawable->GetLodGroup();
	rage::rmcLod* lod = lodGroup.GetLod(rage::LOD_HIGH);
	rage::grmModels& lodModels = lod->GetModels();
	for (u32 i = 0; i < m_Scene->GetNodeCount(); i++)
	{
		graphics::SceneNode* sceneNode = m_Scene->GetNode(i);

		if (!sceneNode->HasMesh())
			continue;

		if (IsCollisionNode(sceneNode))
			continue;

		// Map rage::grmModel -> graphics::SceneNode
		u16 modelIndex = lodModels.GetSize();
		CompiledDrawableMap->SceneNodeToModel[sceneNode->GetIndex()] = { 0, modelIndex }; // TODO: Specify lod

		lodModels.Emplace(ConvertSceneModel(sceneNode));
	}

	// TODO: Set high lod distance for now so buildings don't disappear quickly
	lodGroup.SetLodThreshold(rage::LOD_HIGH, 200.0f);
}

void rageam::asset::DrawableAsset::CalculateLodExtents() const
{
	m_Drawable->GetLodGroup().CalculateExtents();
}

bool rageam::asset::DrawableAsset::CreateMaterials()
{
	rage::grmShaderGroup* shaderGroup = m_Drawable->GetShaderGroup();

	// For each material tune param:
	// - Try find parameter metadata (grcEffectVar) in grcEffect
	// - Simply copy value for regular types, for texture we have to resolve it from string
	for (const amPtr<MaterialTune>& materialTune : m_DrawableTune.Materials)
	{
		const EffectInfo& effectInfo = m_EffectCache.GetAt(Hash(materialTune->Effect));

		rage::grcEffect* effect = effectInfo.Effect;
		rage::grmShader* shader = new rage::grmShader(effect);
		shader->SetDrawBucket(materialTune->DrawBucket);
		for (MaterialTune::Param& param : materialTune->Params)
		{
			u16 varIndex;
			rage::grcEffectVar* varInfo = effect->GetVar(param.Name, &varIndex);

			// Invalid variable name or shader was changed, skipping...
			if (!varInfo)
			{
				AM_WARNINGF("Unable to find variable '%s' in shader effect '%s'.fxc",
					param.Name.GetCStr(), materialTune->Effect.GetCStr());
				continue;
			}

			rage::grcInstanceVar* var = shader->GetVar(varIndex);

			// Locate texture
			if (varInfo->IsTexture())
			{
				string textureName = param.GetValue<string>();
				if (String::IsNullOrEmpty(textureName))
				{
					AM_WARNINGF("Texture is not specified for '%s' in material '%s'",
						param.Name.GetCStr(), materialTune->Name.GetCStr());

					var->SetTexture(TxdAsset::GetNoneTexture());
				}
				else if (!ResolveAndSetTexture(var, textureName))
				{
					AM_ERRF("Texture '%s' is not found in any known dictionary in material '%s'.",
						textureName.GetCStr(), materialTune->Name.GetCStr());
					return false;
				}
			}
			else // Other simple types
			{
				// We have to cast each type manually because there seems to be no way
				// to get value pointer from std::any
				switch (param.Type)
				{
				case MaterialTune::eParamType::Unsupported:
				case MaterialTune::eParamType::None:
				case MaterialTune::eParamType::String:
				case MaterialTune::eParamType::Color:														break;

				case MaterialTune::eParamType::Bool:		var->SetValue(param.GetValue<bool>());			break;
				case MaterialTune::eParamType::Float:		var->SetValue(param.GetValue<float>());			break;
				case MaterialTune::eParamType::Vec2F:		var->SetValue(param.GetValue<rage::Vector2>());	break;
				case MaterialTune::eParamType::Vec3F:		var->SetValue(param.GetValue<rage::Vector3>());	break;
				case MaterialTune::eParamType::Vec4F:		var->SetValue(param.GetValue<rage::Vector4>());	break;
				}
			}
		}

		// Map graphics::SceneMaterial -> rage::grmShader
		u16 shaderIndex = shaderGroup->GetShaderCount();
		u16 materialIndex = m_Scene->GetMaterialByName(materialTune->Name)->GetIndex();
		CompiledDrawableMap->SceneMaterialToShader[materialIndex] = shaderIndex;
		CompiledDrawableMap->ShaderToSceneMaterial.Add(materialIndex);

		shaderGroup->AddShader(shader);
	}
	return true;
}

bool rageam::asset::DrawableAsset::ResolveAndSetTexture(rage::grcInstanceVar* var, ConstString textureName)
{
	if (tl_SkipTextures)
	{
		// 'None' texture
		if (String::Equals(textureName, TxdAsset::MISSING_TEXTURE_NAME))
		{
			var->SetTexture(TxdAsset::GetNoneTexture());
			return true;
		}

		rage::grcTexture* missingTexture = m_EmbedDict->Find(textureName);
		if (!missingTexture)
		{
			// Create new missing texture...
			missingTexture = m_EmbedDict->Insert(textureName, TxdAsset::CreateMissingTexture(textureName));
		}

		var->SetTexture(missingTexture);
		return true;
	}

	// 'None' texture
	if (String::Equals(textureName, TxdAsset::MISSING_TEXTURE_NAME))
	{
		var->SetTexture(TxdAsset::GetNoneTexture());
		return false;
	}

	// Try to resolve first in embed dictionary (it has the highest priority)
	if (m_EmbedDict)
	{
		rage::grcTexture* embedTexture = m_EmbedDict->Find(textureName);
		if (embedTexture)
		{
			var->SetTexture(embedTexture);
			return true;
		}
	}

	// Texture was not in embed dictionary, try to find it in workspace
	for (u16 i = 0; i < WorkspaceTXD->GetTexDictCount(); i++)
	{
		const TxdAssetPtr& sharedTxdAsset = WorkspaceTXD->GetTexDict(i);

		if (!sharedTxdAsset->ContainsTextureWithName(textureName))
			continue;

		// Texture is used in this txd, first look if we compiled it before
		DrawableTxd* cachedSharedTxd = SharedTXDs.TryGetAt(sharedTxdAsset->GetHashKey());
		if (!cachedSharedTxd)
		{
			// TXD was not in cache, compile and add it there
			DrawableTxd sharedTxd(sharedTxdAsset);
			if (!sharedTxd.TryCompile()) // TODO: Ideally we should compile only single texture and use missing, unless requested
			{
				AM_ERRF(L"DrawableAsset::ResolveAndSetTexture() -> Failed to compile dictionary with referenced texture '%ls'.",
					sharedTxdAsset->GetAssetName());
				break;
			}

			cachedSharedTxd = &SharedTXDs.Emplace(std::move(sharedTxd));
		}

		rage::grcTexture* sharedTexture = cachedSharedTxd->Dict->Find(textureName);
		if (sharedTexture)
		{
			var->SetTexture(sharedTexture);
			return true;
		}
	}

	// Texture was not found or failed to compile, mark it as missing...
	var->SetTexture(TxdAsset::CreateMissingTexture(textureName));
	return false;
}

void rageam::asset::DrawableAsset::SetMissingTexture(rage::grcInstanceVar* var, ConstString textureName) const
{
	rage::grcTexture* checker = TxdAsset::CreateMissingTexture(textureName);
	var->SetTexture(checker);
}

bool rageam::asset::DrawableAsset::CompileAndSetEmbedDict()
{
	// For simplicity reasons, we always create dictionary for 'missing' texture
	if (tl_SkipTextures)
	{
		m_EmbedDict = new rage::grcTextureDictionary();
		m_Drawable->GetShaderGroup()->SetEmbedTextureDict(rage::pgPtr(m_EmbedDict));
		return true;
	}

	// Don't create empty embed dictionary 
	if (m_EmbedDictTune->GetTextureTuneCount() == 0)
		return true;

	rage::grcTextureDictionary* embedDict = new rage::grcTextureDictionary();
	if (!m_EmbedDictTune->CompileToGame(embedDict))
	{
		AM_ERRF("DrawableAsset::CompileToGame() -> Failed to compile embeed texture dictionary.");
		delete embedDict;
		return false;
	}

	m_EmbedDict = embedDict;
	m_Drawable->GetShaderGroup()->SetEmbedTextureDict(rage::pgPtr(embedDict));

	return true;
}

rageam::List<rageam::graphics::Primitive> rageam::asset::DrawableAsset::GetPrimitivesFromNode(const graphics::SceneNode* node) const
{
	graphics::SceneMesh* mesh = node->GetMesh();
	if (!mesh)
	{
		AM_ERRF("DrawableAsset::GetPrimitiveFromNode() -> Node '%s' don't have mesh!", node->GetName());
		return {};
	}

	List<graphics::Primitive> primitives;
	for (u16 i = 0; i < mesh->GetGeometriesCount(); i++)
	{
		graphics::SceneGeometry* geom = mesh->GetGeometry(i);

		u32 indexCount = geom->GetIndexCount();
		u32 vertexCount = geom->GetVertexCount();
		// Make sure that we're dealing with polygon topology
		if (vertexCount < 3 || indexCount % 3 != 0)
		{
			AM_ERRF("DrawableAsset::GetPrimitivesFromNode() -> Geometry #%i in model '%s' don't have triangle topology!",
				i, node->GetName());
			continue;
		}

		graphics::SceneData indices;
		graphics::SceneData vertices;
		geom->GetIndices(indices);
		geom->GetAttribute(vertices, graphics::POSITION, 0);

		// We can split it of course but such high dense collision with 65K+ indices is not good
		if (indices.Format != DXGI_FORMAT_R16_UINT)
		{
			AM_WARNINGF("DrawableAsset::GetPrimitivesFromNode() -> Geometry #%i in model '%s' has too much indices (%u) - maximum allowed 65 535, skipping...",
				i, node->GetName(), indexCount);
			continue;
		}

		graphics::Primitive primitive;
		MatchPrimitive(geom->GetAABB(), vertices.GetBufferAs<Vec3S>(), vertexCount, indices.GetBufferAs<u16>(), indexCount, primitive);
		primitives.Add(primitive);
	}
	return primitives;
}

rageam::List<rageam::asset::DrawableAsset::CreatedBoundInfo> rageam::asset::DrawableAsset::CreateBoundsFromNode(graphics::SceneNode* node) const
{
	List<CreatedBoundInfo> bounds;
	for (graphics::Primitive& primitive : GetPrimitivesFromNode(node))
	{
		rage::phBound* newBound = nullptr;

		switch(primitive.Type)
		{
		case graphics::PrimitiveMesh:
			newBound = new rage::phBoundGeometry(
				primitive.AABB, primitive.Mesh.Points, primitive.Mesh.Indices, primitive.Mesh.PointCount, primitive.Mesh.IndexCount);
			break;

		case graphics::PrimitiveBox:
			newBound = new rage::phBoundBox(primitive.AABB);
			break;

		case graphics::PrimitiveSphere:
			newBound = new rage::phBoundSphere(primitive.Sphere.Center, primitive.Sphere.Radius.Get());
			break;

		case graphics::PrimitiveCylinder:
			newBound = new rage::phBoundCylinder(primitive.Cylinder.Center, primitive.Cylinder.Radius.Get(), primitive.Cylinder.HalfHeight.Get());
			break;

		case graphics::PrimitiveCapsule:
			newBound = new rage::phBoundCapsule(primitive.Capsule.Center, primitive.Capsule.Radius.Get(), primitive.Capsule.HalfHeight.Get());
			break;

		case graphics::PrimitiveInvalid:
			break;
		}

		if (newBound)
		{
			CreatedBoundInfo createdBound;
			createdBound.Node = node;
			createdBound.Bound = rage::phBoundPtr(newBound);
			createdBound.Primitive = primitive;
			bounds.Emplace(std::move(createdBound));
		}
	}
	return bounds;
}

rageam::asset::DrawableAsset::CreatedBoundInfo rageam::asset::DrawableAsset::CreateBvhFromNode(graphics::SceneNode* node) const
{
	SmallList<rage::phPrimitive> bvhPrimitives;
	SmallList<Vec3S> bvhVertices;

	for (graphics::SceneNode* childNode : node->GetAllChildrenRecurse())
	{
		if (!childNode->HasMesh())
			continue;

		// BVH don't have any matrix stored for polygons, we have to transform every primitive vertices before adding them to BVH
		const rage::Mat44V& worldTransform = childNode->GetWorldTransform();

		// Adds new vertex to BVH and returns index + transforms it to world space
		auto addVertex = [&](const Vec3V& v)
			{
				// TODO: We can solve this by creating another BVH? And return list from this function
				AM_ASSERT(bvhVertices.GetSize() < UINT16_MAX, "DrawableAsset::CreateBvhFromNode() -> Too much vertices in BVH!");

				int index = bvhVertices.GetSize();
				bvhVertices.Add(v.Transform(worldTransform));
				return index;
			};

		for (graphics::Primitive& primitive : GetPrimitivesFromNode(childNode))
		{
			switch (primitive.Type)
			{
			case graphics::PrimitiveMesh:
			{
				auto& mesh = primitive.Mesh;
				for (int i = 0; i < mesh.IndexCount; i += 3)
				{
					rage::phPolygon poly;
					poly.SetVertexIndex(0, addVertex(mesh.Points[mesh.Indices[i + 0]]));
					poly.SetVertexIndex(1, addVertex(mesh.Points[mesh.Indices[i + 1]]));
					poly.SetVertexIndex(2, addVertex(mesh.Points[mesh.Indices[i + 2]]));
					bvhPrimitives.Add(poly.GetPrimitive());
				}
				break;
			}
			case graphics::PrimitiveBox:
			{
				rage::phPrimBox box;
				box.SetVertexIndex(0, addVertex(primitive.Box.Points[0]));
				box.SetVertexIndex(1, addVertex(primitive.Box.Points[1]));
				box.SetVertexIndex(2, addVertex(primitive.Box.Points[2]));
				box.SetVertexIndex(3, addVertex(primitive.Box.Points[3]));
				bvhPrimitives.Add(box.GetPrimitive());
				break;
			}
			case graphics::PrimitiveSphere:
			{
				rage::phPrimSphere sphere;
				sphere.SetCenter(addVertex(primitive.Sphere.Center));
				sphere.SetRadius(primitive.Sphere.Radius.Get());
				bvhPrimitives.Add(sphere.GetPrimitive());
				break;
			}
			case graphics::PrimitiveCylinder:
			{
				rage::phPrimCylinder cylinder;
				Vec3V extent = primitive.Cylinder.Direction * primitive.Cylinder.HalfHeight;
				cylinder.SetEndIndex0(addVertex(primitive.Cylinder.Center + extent));
				cylinder.SetEndIndex1(addVertex(primitive.Cylinder.Center - extent));
				cylinder.SetRadius(primitive.Cylinder.Radius.Get());
				bvhPrimitives.Add(cylinder.GetPrimitive());
				break;
			}
			case graphics::PrimitiveCapsule:
			{
				rage::phPrimCapsule capsule;
				Vec3V extent = primitive.Capsule.Direction * primitive.Capsule.HalfHeight;
				capsule.SetEndIndex0(addVertex(primitive.Capsule.Center + extent));
				capsule.SetEndIndex1(addVertex(primitive.Capsule.Center - extent));
				capsule.SetRadius(primitive.Capsule.Radius.Get());
				bvhPrimitives.Add(capsule.GetPrimitive());
				break;
			}
			default:
				break;
			}
		}
	}

	if (!bvhPrimitives.Any())
		return {};

	// TODO: AABB is computed in BVH constructor from created primitives, not optimal! We should use AABBs from scene nodes
	rage::phBoundBVH* bvh = new rage::phBoundBVH(bvhVertices, bvhPrimitives);

	CreatedBoundInfo createdBoundInfo;
	createdBoundInfo.Node = node;
	createdBoundInfo.Bound = rage::phBoundPtr(bvh);
	createdBoundInfo.Primitive.Type = graphics::PrimitiveInvalid;
	return createdBoundInfo;
}

rageam::asset::DrawableAsset::ColType rageam::asset::DrawableAsset::GetNodeColType(const graphics::SceneNode* sceneNode) const
{
	if (IsBvhIdentifierNode(sceneNode))
		return ColBvhRoot;

	// All nodes in .COL / .BVH hierarchy are implicitly bounds
	bool hasCol = false;
	bool hasBvh = false;
	while (sceneNode)
	{
		if (IsBvhIdentifierNode(sceneNode))
			hasBvh = true;

		if (IsColIdentifierNode(sceneNode))
			hasCol = true;

		sceneNode = sceneNode->GetParent();
	}

	// Nodes under .BVH have are BVH nodes! This has higher priority than .COL
	if (hasBvh)
		return ColBvh;

	if (hasCol)
		return ColRegular;

	return ColNone;
}

bool rageam::asset::DrawableAsset::IsColIdentifierNode(const graphics::SceneNode* sceneNode) const
{
	ImmutableString modelName = sceneNode->GetName();

	// Explicit bound identifier
	if (modelName.EndsWith(COL_MODEL_EXT, true))
		return true;

	// Support for https://github.com/Weisl/Collider-Tools
	if (modelName.StartsWith("UBX_") || // Box
		modelName.StartsWith("USP_") || // Sphere 
		modelName.StartsWith("UCP_") || // Capsule
		modelName.StartsWith("UCX_"))	// Convex
		return true;

	return false;
}

bool rageam::asset::DrawableAsset::IsBvhIdentifierNode(const graphics::SceneNode* sceneNode) const
{
	ImmutableString nodeName = sceneNode->GetName();
	return nodeName.EndsWith(COL_BVH_EXT, true);
}

void rageam::asset::DrawableAsset::CreateBound() const
{
	// TODO:
	// - How do we handle CG offset, do we use pivot?
	// - Materials

	List<CreatedBoundInfo> createdBounds;
	for (u16 i = 0; i < m_Scene->GetNodeCount(); i++)
	{
		graphics::SceneNode* node = m_Scene->GetNode(i);

		ColType colType = GetNodeColType(node);
		if (colType == ColNone)
			continue;

		// We skip bvh nodes because it is easier to work them later by traversing bvh root children
		if (colType == ColBvh)
			continue;

		// Regular collision will be later added to a composite
		if (colType == ColRegular)
		{
			List<CreatedBoundInfo> createdNodeBounds = CreateBoundsFromNode(node);
			for (CreatedBoundInfo& createdNodeBound : createdNodeBounds)
				createdBounds.Emplace(std::move(createdNodeBound));
			continue;
		}

		// BVH collider doesn't hold bounds as separate entities, it uses primitives instead
		// Because of this we have to handle construction differently from composite
		if (colType == ColBvhRoot)
		{
			CreatedBoundInfo createdBvh = CreateBvhFromNode(node);
			if (createdBvh.Bound)
				createdBounds.Emplace(std::move(createdBvh));
			continue;
		}
	}

	// No bounds were created...
	if (!createdBounds.Any())
		return;

	// TODO: Flags
	// We've got single bound. Composite is only required if we have flags or non-identity transform
	if (createdBounds.GetSize() == 1)
	{
		CreatedBoundInfo& boundInfo = createdBounds[0];

		// However we can set bound offset if we know that it's not rotated or scaled
		const rage::Mat44V& transform = boundInfo.Node->GetWorldTransform();
		bool isOnlyOffset = 
			Vec3V(transform.Right).AlmostEqual(rage::VEC_RIGHT) &&
			Vec3V(transform.Front).AlmostEqual(rage::VEC_FRONT) &&
			Vec3V(transform.Up).AlmostEqual(rage::VEC_UP);

		// Cylinder & Capsule are Y axis up, we must check if UP axis is aligned with Y
		graphics::Primitive& primitive = boundInfo.Primitive;
		if (isOnlyOffset && (primitive.Type == graphics::PrimitiveCylinder || primitive.Type == graphics::PrimitiveCapsule))
		{
			float dot = primitive.Cylinder.Direction.Dot(rage::PH_DEFAULT_ORIENTATION).Get();
			if (!rage::AlmostEquals(fabs(dot), 1.0f))
				isOnlyOffset = false;
		}

		if (isOnlyOffset)
		{
			rage::phBoundPtr bound = boundInfo.Bound;
			bound->SetCentroidOffset(transform.Pos);
			m_Drawable->SetBound(bound);
			return;
		}
	}

	// Otherwise we need to create a composite
	auto composite = new rage::phBoundComposite();
	composite->Init(createdBounds.GetSize());
	for (int i = 0; i < createdBounds.GetSize(); i++)
	{
		CreatedBoundInfo& createdBoundInfo = createdBounds[i];
		graphics::SceneNode* node = createdBoundInfo.Node;

		rage::Mat44V transform = node->GetWorldTransform();

		// Cylinder & Capsule primitives are special cases and we have to align UP axis manually
		graphics::Primitive& primitive = createdBoundInfo.Primitive;
		if (primitive.Type == graphics::PrimitiveCylinder || primitive.Type == graphics::PrimitiveCapsule)
		{
			Mat44V orientation = Mat44V::FromNormalPos(rage::VEC_ORIGIN, primitive.Cylinder.Direction);
			transform = orientation * transform;
		}

		// Scale causes weird issues on geometry bound
		rage::Vec3V scale;
		transform.Decompose(nullptr, &scale, nullptr);
		if (!scale.AlmostEqual(rage::S_ONE))
		{
			AM_WARNINGF(
				"DrawableAsset::CreateBound() -> Element '%s' (index %u) has non-identity (1, 1, 1) scale (%f, %f, %f), collision may work unstable",
				node->GetName(), i, scale.X(), scale.Y(), scale.Z());
		}

		composite->SetBound(i, createdBoundInfo.Bound);
		// TODO: Flags
		composite->SetIncludeFlags(i, rage::CF_ALL);
		composite->SetTypeFlags(i, 
			rage::CF_MAP_TYPE_WEAPON | 
			rage::CF_MAP_TYPE_MOVER | 
			rage::CF_MAP_TYPE_HORSE | 
			rage::CF_MAP_TYPE_COVER | 
			rage::CF_MAP_TYPE_VEHICLE);
		// Matrix must be set after the bound because it access the bound AABB
		composite->SetMatrix(i, transform);
	}
	composite->CalculateExtents();
	composite->AllocateAndBuildBvhStructure();
	m_Drawable->SetBound(rage::phBoundPtr(composite));
}

void rageam::asset::DrawableAsset::PoseModelBoundsFromScene() const
{
	if (!m_Scene->HasTransform())
		return;

	for (u16 i = 0; i < m_Scene->GetNodeCount(); i++)
	{
		graphics::SceneNode* sceneNode = m_Scene->GetNode(i);
		if (!sceneNode->HasTransform())
			continue;

		rage::grmModel* model = m_NodeToModel[i];
		if (!model) // Dummy with no mesh or was excluded (eg .COL nodes)
			continue;

		model->TransformAABB(sceneNode->GetWorldTransform());
	}
}

void rageam::asset::DrawableAsset::GeneratePaletteTexture()
{
	// TODO: ...

	//m_EmbedDict->Refresh();
}

void rageam::asset::DrawableAsset::CreateLights()
{
	for (const auto& lightTune : m_DrawableTune.Lights)
	{
		u16 lightIndex = m_Drawable->GetLightCount();

		// Create light attribute
		CLightAttr& light = m_Drawable->AddLight();
		// light.SetMatrix(lightNode->GetLocalTransform());

		// Convert general properties
		light.Type = eLightType(lightTune->Type);
		light.Position = lightTune->Position;
		light.Direction = lightTune->Direction;
		light.Tangent = lightTune->Tangent;
		light.ColorR = lightTune->ColorRGB.R;
		light.ColorG = lightTune->ColorRGB.G;
		light.ColorB = lightTune->ColorRGB.B;
		light.Flashiness = lightTune->Flashiness;
		light.Intensity = lightTune->Intensity;
		light.Flags = lightTune->Flags;
		light.TimeFlags = lightTune->TimeFlags;
		light.Falloff = lightTune->Falloff;
		light.FallofExponent = lightTune->FalloffExponent;
		light.CullingPlaneNormal = lightTune->CullingPlaneNormal;
		light.CullingPlaneOffset = lightTune->CullingPlaneOffset;
		light.ShadowBlur = lightTune->ShadowBlur;
		light.ShadowNearClip = lightTune->ShadowNearClip;
		light.CoronaSize = lightTune->CoronaSize;
		light.CoronaIntensity = lightTune->CoronaIntensity;
		light.CoronaZBias = lightTune->CoronaZBias;
		light.LightHash = lightTune->LightHash;
		// light.ProjectedTexture = Hash(lightTune->ProjectedTexture);
		light.LightFadeDistance = lightTune->LightFadeDistance;
		light.ShadowFadeDistance = lightTune->ShadowFadeDistance;
		light.SpecularFadeDistance = lightTune->SpecularFadeDistance;
		light.VolumetricFadeDistance = lightTune->VolumetricFadeDistance;
		// Spot
		light.VolumeIntensity = lightTune->VolumeIntensity;
		light.VolumeSizeScale = lightTune->VolumeSizeScale;
		light.VolumeOuterIntensity = lightTune->VolumeOuterIntensity;
		light.VolumeOuterExponent = lightTune->VolumeOuterExponent;
		light.VolumeOuterColorR = lightTune->VolumeOuterColorRGB.R;
		light.VolumeOuterColorG = lightTune->VolumeOuterColorRGB.G;
		light.VolumeOuterColorB = lightTune->VolumeOuterColorRGB.B;
		light.ConeInnerAngle = lightTune->ConeInnerAngle;
		light.ConeOuterAngle = lightTune->ConeOuterAngle;
		// Capsule
		light.Extent.X = lightTune->CapsuleLength;

		graphics::SceneNode* lightNode = m_Scene->GetNodeByName(lightTune->Name);
		u16 lightNodeIndex = lightNode->GetIndex();

		// Locate first parent bone and link it with light
		light.BoneTag = 0;
		graphics::SceneNode* boneNode = lightNode;
		while (boneNode)
		{
			rage::crBoneData* bone = m_NodeToBone[boneNode->GetIndex()];
			if (bone)
			{
				light.BoneTag = bone->GetBoneTag();
				break;
			}
			boneNode = boneNode->GetParent();
		}

		// Map graphics::SceneLight -> CLightAttr
		CompiledDrawableMap->SceneNodeToLightAttr[lightNodeIndex] = lightIndex;
		CompiledDrawableMap->LightAttrToSceneNode.Add(lightNodeIndex);
	}

	AM_DEBUGF("DrawableAsset::CreateLights() -> Created %u lights.", m_Drawable->GetLightCount());
}

bool rageam::asset::DrawableAsset::ValidateScene() const
{
	return true;
}

bool rageam::asset::DrawableAsset::TryCompileToGame()
{
	ReportProgress(L"Validating scene", 0.0);
	if (!ValidateScene())
		return false;

	ReportProgress(L"Compiling embed dictionary", 0.0);
	if (!CompileAndSetEmbedDict())
	{
		AM_ERRF("DrawableAsset::TryCompileToGame() -> Conversion canceled, failed to convert embed dictionary.");
		return false;
	}

	// We must generate skeleton first because we'll have to remap skinning blend indices
	AM_DEBUGF("DrawableAsset() -> Creating skeleton");
	ReportProgress(L"Creating skeleton", 0.1);
	if (!GenerateSkeleton())
		return false;

	AM_DEBUGF("DrawableAsset() -> Setting up lod models");
	ReportProgress(L"Creating LODs", 0.2);
	SetupLodModels();

	// First lod must have at least one model! That's requirement of the game because it accesses it on creating entity
	if (!m_Drawable->GetLodGroup().GetLod(0)->GetModels().Any())
	{
		AM_ERRF("DrawableAsset::TryCompileToGame() -> There must be a least one model in LOD0!");
		return false;
	}

	AM_DEBUGF("DrawableAsset() -> Linking models to skeleton");
	ReportProgress(L"Linking models to skeleton", 0.3);
	LinkModelsToSkeleton();

	AM_DEBUGF("DrawableAsset() -> Creating materials");
	ReportProgress(L"Generating materials", 0.4);
	if (!CreateMaterials())
		return false;

	AM_DEBUGF("DrawableAsset() -> Creating collision bounds");
	ReportProgress(L"Creating collision bounds", 0.5);
	CreateBound();

	AM_DEBUGF("DrawableAsset() -> Creating lights");
	ReportProgress(L"Creating lights", 0.6);
	CreateLights();

	AM_DEBUGF("DrawableAsset() -> Posing bounds from scene");
	ReportProgress(L"Posing bound", 0.7);
	PoseModelBoundsFromScene();

	ReportProgress(L"Calculating lod extents", 0.8);
	CalculateLodExtents();

	m_Drawable->GetLodGroup().ComputeBucketMask(m_Drawable->GetShaderGroup());
	for (u16 i = 0; i < m_Drawable->GetShaderGroup()->GetShaderCount(); i++)
	{
		m_Drawable->ComputeTessellationForShader(i);
	}

	file::Path debugName;
	debugName = file::PathConverter::WideToUtf8(GetAssetName());
	// R* keeps .#dr, let's keep .idr then...
	// debugName = debugName.GetFileNameWithoutExtension();
	m_Drawable->SetName(debugName);

	return true;
}

void rageam::asset::DrawableAsset::RefreshEmbedDict()
{
	m_EmbedDictPath = GetDirectoryPath() / EMBED_DICT_NAME;
	m_EmbedDictPath += ASSET_ITD_EXT;

	if (!IsFileExists(m_EmbedDictPath))
	{
		CreateDirectoryW(m_EmbedDictPath, NULL);
	}

	// Order is important here because otherwise old embed dict would overwrite new one
	// because otherwise old one will be destructed after new one is created
	m_EmbedDictTune = nullptr;
	m_EmbedDictTune = AssetFactory::LoadFromPath<TxdAsset>(m_EmbedDictPath);

	GeneratePaletteTexture();
}

//void rageam::asset::DrawableAsset::CreateMaterialTuneGroupFromScene()
//{
//	bool needDefaultMaterial = m_Scene->NeedDefaultMaterial();
//	if (needDefaultMaterial)
//	{
//		MaterialTune defaultMaterial("default", u16(-1));
//		defaultMaterial.InitFromEffect();
//		m_DrawableTune.Materials.Items.Emplace(std::move(defaultMaterial));
//	}
//
//	for (u16 i = 0; i < m_Scene->GetMaterialCount(); i++)
//	{
//		graphics::SceneMaterial* sceneMaterial = m_Scene->GetMaterial(i);
//
//		MaterialTune material(sceneMaterial->GetName(), i);
//		material.InitFromEffect();
//		material.SetTextureNamesFromSceneMaterial(sceneMaterial);
//		m_DrawableTune.Materials.Items.Emplace(std::move(material));
//	}
//}

void rageam::asset::DrawableAsset::RefreshTXDWorkspace()
{
	// We need workspace only for TXDs
	WorkspaceTXD = Workspace::FromPath(GetWorkspacePath(), WF_LoadTx);
}

bool rageam::asset::DrawableAsset::TryToFindFirstSceneFile(file::WPath& outPath) const
{
	file::Iterator iterator(GetDirectoryPath() / L"*.*");
	file::FindData data;

	// Take first supported model file
	while (iterator.Next())
	{
		iterator.GetCurrent(data);
		file::WPath extension = data.Path.GetExtension();

		if (graphics::SceneFactory::IsSupportedFormat(extension))
		{
			outPath = data.Path;
			return true;
		}
	}
	return true;
}

bool rageam::asset::DrawableAsset::RefreshSceneFile()
{
	file::WPath scenePath = GetScenePath();

	bool needModelRescan = false;
	if (String::IsNullOrEmpty(m_DrawableTune.SceneFileName))
	{
		needModelRescan = true;
		AM_DEBUGF("DrawableAsset::RefreshModel() -> Scanning model for the first time...");
	}
	else if (!IsFileExists(scenePath))
	{
		needModelRescan = true;
		AM_DEBUGF("DrawableAsset::RefreshModel() -> Model file was not found! Trying to find any...");
	}

	// Model file is most likely fine (well it exists at least...)
	if (!needModelRescan)
		return true;

	if (!TryToFindFirstSceneFile(scenePath))
	{
		AM_ERRF("DrawableAsset::RefreshModel() -> Failed to locate any 3D scene file.");
		return false;
	}

	m_DrawableTune.SceneFileName = scenePath.GetFileName();

	return true;
}

rageam::asset::DrawableAsset::DrawableAsset(const file::WPath& path) : GameRscAsset(path)
{
	RefreshEmbedDict();
}

rageam::asset::DrawableAsset::DrawableAsset(const DrawableAsset& other)
	: GameRscAsset(other), m_DrawableTune(other.m_DrawableTune)
{
	m_EmbedDictPath = other.m_EmbedDictPath;
	m_EmbedDictTune = other.m_EmbedDictTune;
	m_SceneFileTime = other.m_SceneFileTime;
	m_Scene = other.m_Scene;
}

bool rageam::asset::DrawableAsset::CompileToGame(gtaDrawable* ppOutGameFormat)
{
	Refresh();

	file::WPath scenePath = GetScenePath();

	// Scene is not loaded or outdated, attempt to load it
	u64 sceneModifyTime = GetFileModifyTime(scenePath);
	if (!m_Scene || sceneModifyTime != m_SceneFileTime)
	{
		m_SceneFileTime = sceneModifyTime;
		m_Scene = graphics::SceneFactory::LoadFrom(scenePath);
		if (!m_Scene)
		{
			AM_ERRF("DrawableAsset::CompileToGame() -> Failed to load scene file...");
			return false;
		}
	}

	m_Drawable = ppOutGameFormat;
	ReportProgress(L"Preparing assets", 0.0);
	PrepareForConversion();
	bool result = TryCompileToGame();
	ReportProgress(L"Cleaning up", 0.99);
	CleanUpConversion();
	m_Drawable = nullptr;
	return result;
}

void rageam::asset::DrawableAsset::ParseFromGame(gtaDrawable* object)
{
	// Drawable was compiled before, just map drawable and restore values
	if (CompiledDrawableMap)
	{
		// Materials
		{
			auto shaderGroup = object->GetShaderGroup();
			auto& matTunes = m_DrawableTune.Materials;
			for (u16 i = 0; i < matTunes.GetCount(); i++)
			{
				// rage::grmShader index directly map to MaterialTune
				matTunes.Get(i)->InitFromShader(shaderGroup->GetShader(i));
			}
		}

		// Lod Group
		{

		}

		// Lights
		{
			auto& lightTunes = m_DrawableTune.Lights;
			for (u16 i = 0; i < lightTunes.GetCount(); i++)
			{
				// CLightAttr index directly map to LightTune
				lightTunes.Get(i)->FromLightAttr(object->GetLight(i));
			}
		}
	}
	else
	{
		// TODO:
		// Main issue we have to figure out here is how do we handle converting mesh to obj/fbx,
		// basically every asset kind will require such options (for example output texture format for YTD, with DDS as default)
		AM_UNREACHABLE("DrawableAsset::ParseFromGame() -> Decompiling without source is not yet supported!");
	}
}

void rageam::asset::DrawableAsset::Refresh()
{
	if (!RefreshSceneFile())
		return;

	graphics::SceneLoadOptions sceneLoadOptions = {};
	sceneLoadOptions.SkipMeshData = true; // We need only metadata to refresh

	m_Scene = graphics::SceneFactory::LoadFrom(GetScenePath(), &sceneLoadOptions);
	if (!m_Scene)
	{
		AM_ERRF("DrawableAsset::Refresh() -> Failed to load scene medata.");
		return;
	}

	RefreshTunesFromScene();

	// Since it only contains metadata, we don't need it anymore
	m_Scene = nullptr;
}

void rageam::asset::DrawableAsset::RefreshTunesFromScene()
{
	RefreshTXDWorkspace();
	RefreshEmbedDict();

	m_DrawableTune.Materials.Refresh(m_Scene.get());
	m_DrawableTune.Lights.Refresh(m_Scene.get());
	m_DrawableTune.Lods.Models.Refresh(m_Scene.get());
}

void rageam::asset::DrawableAsset::Serialize(XmlHandle& node) const
{
	m_DrawableTune.Serialize(node);
}

void rageam::asset::DrawableAsset::Deserialize(const XmlHandle& node)
{
	m_DrawableTune.Deserialize(node);
}
