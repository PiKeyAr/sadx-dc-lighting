#include "stdafx.h"
#include <d3d9.h>
#include "d3d8types.hpp"

// Mod loader
#include <SADXModLoader.h>
#include <Trampoline.h>

// Standard library
#include <string>
#include <sstream>
#include <fstream>
#include <vector>

// Local
#include "d3d.h"
#include "datapointers.h"
#include "fog.h"
#include "globals.h"

using namespace std;

// TODO: split this file; it's way too huge.
// TODO: load resources from SYSTEM folder
// TODO: figure out why occasional flickering happens

#pragma region Types

#pragma pack(push, 1)
struct ColorPair
{
	NJS_COLOR diffuse, specular;
};

struct PaletteLight
{
	Uint8 Level;
	Uint8 Act;
	Uint8 Flags;
	Uint8 Type;
	NJS_VECTOR Direction;
	float DIF;     // (0~4 ) Diffuse
	float AMB_R;   // (0~4 ) Ambient R
	float AMB_G;   // (0~4 ) Ambient G
	float AMB_B;   // (0~4 ) Ambient B
	float CO_pow;  // (0~99) Upper Color Power (higher is less visible)
	float CO_R;    // (0~4 ) Upper Color R
	float CO_G;    // (0~4 ) Upper Color G
	float CO_B;    // (0~4 ) Upper Color B
	float SP_pow;  // (0~99) Upper Specular Power (higher is less visible)
	float SP_R;    // (0~4 ) Upper Specular R
	float SP_G;    // (0~4 ) Upper Specular G
	float SP_B;    // (0~4 ) Upper Specular B
	float CO2_pow; // (0~99) Lower Color Power (higher is less visible)
	int CO2_R;     // (0~4 ) Lower Color R
	int CO2_G;     // (0~4 ) Lower Color G
	int CO2_B;     // (0~4 ) Lower Color B
	float SP2_pow; // (0~99) Lower Specular Power (higher is less visible)
	float SP2_R;   // (0~4 ) Lower Specular R
	float SP2_G;   // (0~4 ) Lower Specular G
	float SP2_B;   // (0~4 ) Lower Specular B
};

struct LanternPalette
{
	// The first set of colors
	IDirect3DTexture9* diffuse;
	// The second set of colors
	IDirect3DTexture9* specular;
};

enum RenderFlags
{
	EnvironmentMap   = 0x1,
	ConstantMaterial = 0x2,
	OffsetMaterial   = 0x4,
	RenderFlags_8    = 0x8,
	RenderFlags_10   = 0x10,
};
#pragma pack(pop)

static_assert(sizeof(ColorPair) == sizeof(NJS_COLOR) * 2, "AGAIN TRY");
static_assert(sizeof(PaletteLight) == 0x60, "AGAIN TRY");

#pragma endregion

static Trampoline* Direct3D_ParseMaterial_t      = nullptr;
static Trampoline* Direct3D_PerformLighting_t    = nullptr;
static Trampoline* Direct3D_SetWorldTransform_t  = nullptr;
static Trampoline* DrawLandTable_t               = nullptr;
static Trampoline* CharSel_LoadA_t               = nullptr;
static Trampoline* MeshNull_CreateVertexBuffer_t = nullptr;
static Trampoline* SetLevelAndAct_t              = nullptr;
static Trampoline* GoToNextLevel_t               = nullptr;
static Trampoline* IncrementAct_t                = nullptr;
static Trampoline* SetTimeOfDay_t                = nullptr;
static Trampoline* LoadLevelFiles_t              = nullptr;

static Uint32 last_level    = 0;
static Uint32 last_act      = 0;
static Sint8 last_time      = 0;
static bool use_palette     = false;
static bool is_landtable    = false;
static NJS_VECTOR light_dir = {};

static LanternPalette palettes[8] = {};

static D3DXMATRIX _ViewMatrix, _ProjectionMatrix;

//DataArray(PaletteLight, LightPaletteData, 0x00903E88, 256);
DataPointer(PaletteLight, LSPalette, 0x03ABDAF0);
DataPointer(EntityData1*, Camera_Data1, 0x03B2CBB0);
DataPointer(Uint32, LastRenderFlags, 0x03D08498);
DataPointer(D3DLIGHT8, Direct3D_CurrentLight, 0x03ABDB50);
DataPointer(D3DXMATRIX, WorldTransform, 0x03D12900);
DataPointer(NJS_COLOR, LandTableVertexColor, 0x03D08494);
DataPointer(NJS_COLOR, EntityVertexColor, 0x03D0848C);
DataPointer(Uint32, _nj_constant_or_attr, 0x03D0F9C4);
DataPointer(Uint32, _nj_constant_and_attr, 0x03D0F840);
DataPointer(Uint32, _nj_control_3d, 0x03D0F9C8);
DataArray(StageLightData, CurrentStageLights, 0x03ABD9F8, 4);

FunctionPointer(Bool, njPushMatrixEx, (void), 0x00781C80);
FastcallFunctionPointer(void, njUnitMatrix, (NJS_MATRIX* m), 0x007875F0);
FastcallFunctionPointer(void, njCalcVector, (NJS_MATRIX* m, NJS_VECTOR* vs, NJS_VECTOR* vd), 0x00784D60);
FunctionPointer(Bool, njPopMatrixEx, (void), 0x007816E0);

#pragma region Palette loading
/// <summary>
/// Get the pixel data of the specified texture.
/// </summary>
/// <param name="texture">The texture to lock.</param>
/// <returns>The locked rect.</returns>
inline D3DLOCKED_RECT GetTextureRect(IDirect3DTexture9* texture)
{
	D3DLOCKED_RECT result;
	HRESULT hresult = texture->LockRect(0, &result, nullptr, 0);
	if (FAILED(hresult))
	{
		throw exception("Failed to lock texture rect!");
	}
	return result;
}

/// <summary>
/// Creates a 1x256 image to be used as a palette.
/// </summary>
/// <param name="texture">Destination texture.</param>
inline void CreateTexture(IDirect3DTexture9** texture)
{
	using namespace d3d;
	if (FAILED(device->CreateTexture(1, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture, nullptr)))
	{
		throw exception("Failed to create palette texture!");
	}
}

/// <summary>
/// Creates a pair of texture palettes from the provided color pairs.
/// </summary>
/// <param name="palette">The destination palette structure.</param>
/// <param name="pairs">Array of 256 color pairs. (512 total)</param>
static void CreatePaletteTexturePair(LanternPalette& palette, ColorPair* pairs)
{
	using namespace d3d;
	if (device == nullptr)
		throw exception("Device is null.");

	if (palette.diffuse)
	{
		palette.diffuse->Release();
		palette.diffuse = nullptr;
	}

	if (palette.specular)
	{
		palette.specular->Release();
		palette.specular = nullptr;
	}

	if (!palette.diffuse)
		CreateTexture(&palette.diffuse);
	if (!palette.specular)
		CreateTexture(&palette.specular);

	auto diffuse_rect = GetTextureRect(palette.diffuse);
	auto diffuse_data = (NJS_COLOR*)diffuse_rect.pBits;

	auto specular_rect = GetTextureRect(palette.specular);
	auto specular_data = (NJS_COLOR*)specular_rect.pBits;

	for (size_t i = 0; i < 256; i++)
	{
		const auto& pair = pairs[i];
		auto& diffuse = diffuse_data[i];
		auto& specular = specular_data[i];

		diffuse = pair.diffuse;
		specular = pair.specular;
	}

	palette.diffuse->UnlockRect(0);
	palette.specular->UnlockRect(0);
}

/// <summary>
/// Returns a string in the format "_[0-9]", "1[A-Z]", "2[A-Z]", etc.
/// </summary>
/// <param name="level">Current level.</param>
/// <param name="act">Current act.</param>
/// <returns>A string containing the properly formatted PL level ID.</returns>
static string LanternPaletteId(Uint32 level, Uint32 act)
{
	// Special cases because sonic adventure:
	// Egg Walker specifically loads the night time Station Square palette.
	if (level == LevelIDs_EggWalker)
	{
		level = LevelIDs_StationSquare;
		act = 3;
	}
	else if (level >= LevelIDs_StationSquare && (level < LevelIDs_Past || level > LevelIDs_SandHill))
	{
		switch (level)
		{
			// Any other adventure field presumably uses time of day
			// in place of act for filenames (e.g Mystic Ruins).
			default:
				act = GetTimeOfDay();
				break;

			// Egg Carrier interior is not affected by time of day.
			case LevelIDs_EggCarrierInside:
				break;

			// Station Square, unlike other sane adventure fields,
			// uses arbitrary numbers for time of day.
			case LevelIDs_StationSquare:
				switch (GetTimeOfDay())
				{
					default:
						act = 4;
						break;
					case 1:
						act = 1;
						break;
					case 2:
						act = 3;
						break;
				}

				break;

			// Station Square Chao Garden only supports day and evening.
			case LevelIDs_SSGarden:
				act = min(1, GetTimeOfDay());
				break;

			// Egg Carrier Exterior uses Emerald Coast's palette if it's on the ocean,
			// otherwise it just uses a single palette, unaffected by time of day.
			case LevelIDs_EggCarrierOutside:
				int flag;

				switch (CurrentCharacter)
				{
					default:
						flag = EventFlags_Sonic_EggCarrierSunk;
						break;
					case Characters_Tails:
						flag = EventFlags_Tails_EggCarrierSunk;
						break;
					case Characters_Knuckles:
						flag = EventFlags_Knuckles_EggCarrierSunk;
						break;
					case Characters_Amy:
						flag = EventFlags_Amy_EggCarrierSunk;
						break;
					case Characters_Big:
						flag = EventFlags_Big_EggCarrierSunk;
						break;
					case Characters_Gamma:
						flag = EventFlags_Gamma_EggCarrierSunk;
						break;
				}

				if (EventFlagArray[flag] != 0)
				{
					level = LevelIDs_EmeraldCoast;
					act = 0;
				}
				else
				{
					act = 0;
				}
				break;
		}
	}

	stringstream result;

	if (level < 10)
	{
		result << "_" << level << act;
		return result.str();
	}

	auto n = (level - 10) / 26;

	if (n > 9)
	{
		throw exception("Level ID out of range.");
	}

	if (!n)
	{
		result << "_";
	}
	else
	{
		result << n;
	}


	auto i = (char)(level - (10 + 26 * n) + 'A');
	result << i << act;
	return result.str();
}

/// <summary>
/// Overwrites light directions for the current stage with the specified direction.
/// </summary>
/// <param name="dir">The direction with which to overwrite.</param>
static void UpdateLightDirections(const NJS_VECTOR& dir)
{
	int level = CurrentLevel;
	int act = CurrentAct;
	DoTimeOfDayLighting(&level, &act);

	int n = 0;
	for (StageLightData* i = GetStageLight(level, act, n); i != nullptr; i = GetStageLight(level, act, ++n))
	{
		i->xyz = dir;
	}
}

/// <summary>
/// Loads palette parameter data (light direction) for the specified stage/act.
/// </summary>
/// <param name="level">Current level/stage.</param>
/// <param name="act">Current act.</param>
/// <returns><c>true</c> on success.</returns>
static bool LoadLanternSource(Uint32 level, Uint32 act)
{
	bool result = true;

	stringstream name;
	name << globals::system << "SL" << LanternPaletteId(level, act) << "B.BIN";
	auto file = ifstream(name.str(), ios::binary);

	NJS_VECTOR dir;

	if (!file.is_open())
	{
		dir = { 0.0f, -1.0f, 0.0f };
		result = false;
	}
	else
	{
		Angle yaw, roll;

		file.seekg(0x5A0);
		file.read((char*)&yaw, sizeof(Angle));
		file.read((char*)&roll, sizeof(Angle));
		file.close();

		NJS_MATRIX m;
		auto _m = &m;

		njUnitMatrix(_m);
		njRotateY(_m, yaw);
		njRotateZ(_m, roll);

		// Default light direction is down, so we want to rotate relative to that.
		static const NJS_VECTOR vs = { 0.0f, -1.0f, 0.0f };
		njCalcVector(_m, (NJS_VECTOR*)&vs, &dir);
	}

	UpdateLightDirections(dir);
	return result;
}

/// <summary>
/// Loads palette data for the specified stage and act.
/// </summary>
/// <param name="level">Current level/stage.</param>
/// <param name="act">Current act.</param>
/// <returns><c>true</c> on success.</returns>
static bool LoadLanternPalette(Uint32 level, Uint32 act)
{
#if 1
	stringstream name;
	name << globals::system << "PL" << LanternPaletteId(level, act) << "B.BIN";
	auto file = ifstream(name.str(), ios::binary);
#else
	auto file = ifstream("C:\\Users\\Michael\\Desktop\\wojnis.bin", ios::binary);
#endif

	if (!file.is_open())
	{
		return false;
	}

	vector<ColorPair> colorData;

	do
	{
		ColorPair pair = {};
		file.read((char*)&pair.diffuse, sizeof(NJS_COLOR));
		file.read((char*)&pair.specular, sizeof(NJS_COLOR));
		colorData.push_back(pair);
	} while (!file.eof());

	file.close();

	for (size_t i = 0; i < 8; i++)
	{
		auto index = i * 256;
		if (index < colorData.size() && index + 256 < colorData.size())
		{
			CreatePaletteTexturePair(palettes[i], &colorData[index]);
		}
		else
		{
			auto& palette = palettes[i];

			if (palette.diffuse)
			{
				palette.diffuse->Release();
				palette.diffuse = nullptr;
			}

			if (palette.specular)
			{
				palette.specular->Release();
				palette.specular = nullptr;
			}
		}
	}

	return true;
}

/// <summary>
/// Loads the lantern palette and source files for the current level if it (or the time of day) has changed.
/// </summary>
void LoadLanternFiles()
{
	auto time = GetTimeOfDay();

	for (int i = CurrentAct; i >= 0; i--)
	{
		int level = CurrentLevel;
		int act = i;

		DoTimeOfDayLighting(&level, &act);

		if (level == last_level && act == last_act && time == last_time)
			break;

		if (!LoadLanternPalette(CurrentLevel, i))
			continue;
			
		LoadLanternSource(CurrentLevel, i);

		last_time  = time;
		last_level = level;
		last_act   = act;

		use_palette = false;
		d3d::do_effect = false;
		break;
	}
}
#pragma endregion

static void DisplayLightDirection()
{
#ifdef _DEBUG
	auto player = CharObj1Ptrs[0];
	if (player == nullptr)
		return;

	NJS_POINT3 points[2] = {
		player->Position,
		{}
	};

	int colors[2] = {};

	LineInfo info = {
		points, colors, 0, 2
	};

	points[1] = points[0];
	points[1].x += light_dir.x * 10.0f;
	colors[0] = colors[1] = 0xFFFF0000;
	DrawLineList(&info, 1, 0);

	points[1] = points[0];
	points[1].y += light_dir.y * 10.0f;
	colors[0] = colors[1] = 0xFF00FF00;
	DrawLineList(&info, 1, 0);

	points[1] = points[0];
	points[1].z += light_dir.z * 10.0f;
	colors[0] = colors[1] = 0xFF0000FF;
	DrawLineList(&info, 1, 0);

	points[1] = points[0];
	points[1].x += light_dir.x * 10.0f;
	points[1].y += light_dir.y * 10.0f;
	points[1].z += light_dir.z * 10.0f;
	colors[0] = colors[1] = 0xFFFFFF00;
	DrawLineList(&info, 1, 0);
#endif
}

static void SetLightParameters()
{
	using namespace d3d;

	if (!use_palette || effect == nullptr)
		return;

	const auto& light = Direct3D_CurrentLight;
	auto dir = -*(D3DXVECTOR3*)&light.Direction;
	auto mag = D3DXVec3Length(&dir);
	effect->SetValue("LightDirection", &dir, sizeof(D3DVECTOR));
	effect->SetFloat("LightLength", mag);
	//effect->SetVector("LightAmbient", (D3DXVECTOR4*)&light.Ambient);
	//effect->SetVector("LightDiffuse", (D3DXVECTOR4*)&light.Diffuse);
	//effect->SetVector("LightSpecular", (D3DXVECTOR4*)&light.Specular);
}

static void SetMaterialParameters(const D3DMATERIAL9& material)
{
	using namespace d3d;

	if (!use_palette || effect == nullptr)
		return;

	D3DMATERIALCOLORSOURCE colorsource;
	device->GetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, (DWORD*)&colorsource);
	effect->SetInt("DiffuseSource", colorsource);
	//device->GetRenderState(D3DRS_SPECULARMATERIALSOURCE, (DWORD*)&colorsource);
	//effect->SetInt("SpecularSource", colorsource);
	//device->GetRenderState(D3DRS_AMBIENTMATERIALSOURCE, (DWORD*)&colorsource);
	//effect->SetInt("AmbientSource", colorsource);

	//effect->SetVector("MaterialAmbient", (D3DXVECTOR4*)&material.Ambient);
	effect->SetVector("MaterialDiffuse", (D3DXVECTOR4*)&material.Diffuse);
	//effect->SetVector("MaterialSpecular", (D3DXVECTOR4*)&material.Specular);
	//effect->SetFloat("MaterialPower", material.Power);
}

static void __cdecl CorrectMaterial_r()
{
	using namespace d3d;
	D3DMATERIAL9 material; // [sp+8h] [bp-44h]@1

	device->GetMaterial(&material);
	material.Power = LSPalette.SP_pow;

	material.Ambient.r  /= 255.0f;
	material.Ambient.g  /= 255.0f;
	material.Ambient.b  /= 255.0f;
	material.Ambient.a  /= 255.0f;
	material.Diffuse.r  /= 255.0f;
	material.Diffuse.g  /= 255.0f;
	material.Diffuse.b  /= 255.0f;
	material.Diffuse.a  /= 255.0f;
	material.Specular.r /= 255.0f;
	material.Specular.g /= 255.0f;
	material.Specular.b /= 255.0f;
	material.Specular.a /= 255.0f;

	SetMaterialParameters(material);
	device->SetMaterial(&material);
}

static void __fastcall Direct3D_ParseMaterial_r(NJS_MATERIAL* material)
{
	using namespace d3d;

	auto original = (decltype(Direct3D_ParseMaterial_r)*)Direct3D_ParseMaterial_t->Target();
	original(material);

	if (effect == nullptr)
		return;

	do_effect = false;

	auto pad = ControllerPointers[0];
	if (!use_palette || pad && pad->HeldButtons & Buttons_Z)
		return;

	auto flags = material->attrflags;
	if (_nj_control_3d & NJD_CONTROL_3D_CONSTANT_ATTR)
	{
		flags = _nj_constant_or_attr | _nj_constant_and_attr & flags;
	}

	globals::light = (flags & NJD_FLAG_IGNORE_LIGHT) == 0;
	SetLightParameters();

	effect->SetBool("AlphaEnabled", (flags & NJD_FLAG_USE_ALPHA) != 0);

	bool use_texture = (flags & NJD_FLAG_USE_TEXTURE) != 0;

	if (use_texture)
	{
		IDirect3DBaseTexture9* texture;
		device->GetTexture(0, &texture);
		effect->SetTexture("BaseTexture", texture);
	}

	effect->SetBool("TextureEnabled", use_texture);
	effect->SetBool("EnvironmentMapped", (LastRenderFlags & EnvironmentMap) != 0);

	D3DMATERIAL9 mat;
	device->GetMaterial(&mat);
	SetMaterialParameters(mat);

	do_effect = true;
}

static void SetPaletteLights(int type)
{
	auto pad = ControllerPointers[0];
	if (d3d::effect == nullptr || pad && pad->HeldButtons & Buttons_Z)
	{
		use_palette = false;
		d3d::do_effect = false;
		return;
	}

	globals::light = true;
	SetLightParameters();

	// [0] 1-2 level geometry
	// [1] 3-4 objects (springs etc.)
	// [2] 5-6 player models & misc objects
	// [3] 7-8 Tails' shoes and jump ball (sometimes? probably level-specific)

	// type:
	// 0: stage
	// 2: character
	// 4: shiny thing (super sonic, gamma)
	// 6: enemy???

	Sint32 diffuse = -1;
	Sint32 specular = -1;

	switch (type)
	{
		case 0:
			diffuse = 0;
			specular = is_landtable ? 0 : 1;
			light_dir = *(NJS_VECTOR*)&Direct3D_CurrentLight.Direction;
			break;

		case 2:
			diffuse = 2;
			specular = 2;
			break;

		case 4:
			diffuse = 2;
			specular = 3;
			break;

		case 6:
			diffuse = 0;
			specular = 1;

		default:
			break;
	}

	if (diffuse < 0 || specular < 0)
	{
		use_palette = false;
		d3d::do_effect = false;
		return;
	}

	d3d::effect->SetTexture("DiffusePalette", palettes[diffuse].diffuse);
	d3d::effect->SetTexture("SpecularPalette", palettes[specular].specular);
	use_palette = true;
}

static void __cdecl Direct3D_PerformLighting_r(int type)
{
	FunctionPointer(void, original, (int), Direct3D_PerformLighting_t->Target());
	original(0);

	if (d3d::effect == nullptr)
		return;

	SetPaletteLights(type);
}

static void __cdecl DrawLandTable_r()
{
	is_landtable = true;
	SetPaletteLights(0);

	auto original = (decltype(DrawLandTable_r)*)DrawLandTable_t->Target();
	original();

	is_landtable = false;
	SetPaletteLights(0);
}

static void __cdecl Direct3D_SetWorldTransform_r()
{
	VoidFunc(original, Direct3D_SetWorldTransform_t->Target());
	original();

	using namespace d3d;

	if (!use_palette || effect == nullptr)
		return;

	device->GetTransform(D3DTS_VIEW, &_ViewMatrix);
	device->GetTransform(D3DTS_PROJECTION, &_ProjectionMatrix);

	effect->SetMatrix("ViewMatrix", &_ViewMatrix);
	effect->SetMatrix("ProjectionMatrix", &_ProjectionMatrix);
	effect->SetMatrix("WorldMatrix", &WorldTransform);

	if (Camera_Data1 != nullptr)
	{
		auto position = D3DXVECTOR4(*(D3DVECTOR*)&Camera_Data1->Position, 1);
		effect->SetVector("CameraPosition", &position);
	}

	auto wvMatrix = WorldTransform * _ViewMatrix;
	D3DXMatrixInverse(&wvMatrix, nullptr, &wvMatrix);
	D3DXMatrixTranspose(&wvMatrix, &wvMatrix);
	effect->SetMatrix("wvMatrixInvT", &wvMatrix);
}

static void __cdecl CharSel_LoadA_r()
{
	auto original = (decltype(CharSel_LoadA_r)*)CharSel_LoadA_t->Target();

	NJS_VECTOR dir = { 1.0f, -1.0f, -1.0f };

	njUnitVector(&dir);
	UpdateLightDirections(dir);

	LoadLanternPalette(LevelIDs_SkyDeck, 0);

	last_level = CurrentLevel;
	last_act = CurrentAct;

	original();
}

static void MeshNull_CreateVertexBuffer_original(MeshSetBuffer* mesh, int count)
{
	// ReSharper disable once CppEntityNeverUsed
	auto original = MeshNull_CreateVertexBuffer_t->Target();
	__asm
	{
		mov edi, mesh
		push count
		call original
		add esp, 4
	}
}

// Overrides landtable vertex colors with white (retaining alpha channel)
static void __cdecl MeshNull_CreateVertexBuffer_c(MeshSetBuffer* mesh, int count)
{
	if (mesh->VertexBuffer == nullptr && mesh->Meshset->vertcolor != nullptr)
	{
		auto n = count;

		switch (mesh->Meshset->type_matId & NJD_MESHSET_MASK)
		{
			default:
				n = n - 2 * mesh->Meshset->nbMesh;
				break;

			case NJD_MESHSET_3:
				n = mesh->Meshset->nbMesh * 3;
				break;

			case NJD_MESHSET_4:
				n = mesh->Meshset->nbMesh * 4;
				break;
		}

		// TODO: consider checking for 0x**B2B2B2
		for (int i = 0; i < n; i++)
		{
			mesh->Meshset->vertcolor[i].color |= 0x00FFFFFF;
		}
	}

	MeshNull_CreateVertexBuffer_original(mesh, count);
}
static void __declspec(naked) MeshNull_CreateVertexBuffer_r()
{
	__asm
	{
		push [esp + 04h] // count
		push edi         // mesh
		call MeshNull_CreateVertexBuffer_c
		pop edi          // mesh
		add esp, 4       // count
		retn
	}
}

void __cdecl SetLevelAndAct_r(Uint8 level, Uint8 act)
{
	auto original = (decltype(SetLevelAndAct_r)*)SetLevelAndAct_t->Target();
	original(level, act);
	LoadLanternFiles();
}

void __cdecl GoToNextLevel_r()
{
	auto original = (decltype(GoToNextLevel_r)*)GoToNextLevel_t->Target();
	original();
	LoadLanternFiles();
}

void __cdecl IncrementAct_r(int amount)
{
	auto original = (decltype(IncrementAct_r)*)IncrementAct_t->Target();
	original(amount);

	if (amount != 0)
	{
		LoadLanternFiles();
	}
}

void __cdecl SetTimeOfDay_r(Sint8 time)
{
	auto original = (decltype(SetTimeOfDay_r)*)SetTimeOfDay_t->Target();
	original(time);
	LoadLanternFiles();
}

static void __cdecl LoadLevelFiles_r()
{
	auto original = (decltype(LoadLevelFiles_r)*)LoadLevelFiles_t->Target();
	original();
	LoadLanternFiles();
}

extern "C"
{
	EXPORT ModInfo SADXModInfo = { ModLoaderVer };
	EXPORT void __cdecl Init(const char *path)
	{
		auto handle = GetModuleHandle(L"d3d9.dll");
		if (handle == nullptr)
		{
			MessageBoxA(WindowHandle, "Unable to detect Direct3D 9 DLL. The mod will not function.",
				"D3D9 not loaded", MB_OK | MB_ICONERROR);
			return;
		}

		globals::system = path;
		globals::system.append("\\system\\");

		d3d::InitTrampolines();
		Direct3D_ParseMaterial_t      = new Trampoline(0x00784850, 0x00784858, Direct3D_ParseMaterial_r);
		Direct3D_PerformLighting_t    = new Trampoline(0x00412420, 0x00412426, Direct3D_PerformLighting_r);
		Direct3D_SetWorldTransform_t  = new Trampoline(0x00791AB0, 0x00791AB5, Direct3D_SetWorldTransform_r);
		DrawLandTable_t               = new Trampoline(0x0043A6A0, 0x0043A6A8, DrawLandTable_r);
		CharSel_LoadA_t               = new Trampoline(0x00512BC0, 0x00512BC6, CharSel_LoadA_r);
		MeshNull_CreateVertexBuffer_t = new Trampoline(0x007853D0, 0x007853D6, MeshNull_CreateVertexBuffer_r);
		SetLevelAndAct_t              = new Trampoline(0x00414570, 0x00414576, SetLevelAndAct_r);
		GoToNextLevel_t               = new Trampoline(0x00414610, 0x00414616, GoToNextLevel_r);
		IncrementAct_t                = new Trampoline(0x004146E0, 0x004146E5, IncrementAct_r);
		SetTimeOfDay_t                = new Trampoline(0x00412C00, 0x00412C05, SetTimeOfDay_r);
		LoadLevelFiles_t              = new Trampoline(0x00422AD0, 0x00422AD8, LoadLevelFiles_r);

		// Correcting a function call since they're relative
		WriteCall(IncrementAct_t->Target(), (void*)0x00424830);

		WriteJump((void*)0x0040A340, CorrectMaterial_r);
	}

	EXPORT void __cdecl OnFrame()
	{
		auto pad = ControllerPointers[0];
		if (pad && pad->PressedButtons & Buttons_C)
		{
			d3d::LoadShader();
			SetFogParameters();
		}

		if (d3d::effect == nullptr)
			return;

#if 0
		if (pad)
		{
			auto pressed = pad->PressedButtons;

			if (pressed & Buttons_Up)
			{
				++use_palette %= 8;
			}
			else if (pressed & Buttons_Down)
			{
				use_palette = !use_palette ? 7 : use_palette - 1;
			}
		}
#endif

		DisplayLightDirection();
	}

	EXPORT void __cdecl OnRenderDeviceLost()
	{
		if (d3d::effect != nullptr)
			d3d::effect->OnLostDevice();
	}

	EXPORT void __cdecl OnRenderDeviceReset()
	{
		if (d3d::effect != nullptr)
		{
			d3d::effect->OnResetDevice();
			SetFogParameters();
		}
	}
}
