#include "stdafx.h"

// Direct3D
#include <d3dx9.h>

// d3d8to9
#include <d3d8to9.hpp>

// Mod loader
#include <SADXModLoader/SADXFunctions.h>
#include <Trampoline.h>

// MinHook
#include <MinHook.h>

// Standard library
#include <vector>

// Local
#include "d3d.h"
#include "datapointers.h"
#include "globals.h"
#include "lantern.h"
#include "EffectParameter.h"

// TODO: Organize/split this file.

#pragma pack(push, 1)
struct __declspec(align(2)) PolyBuff_RenderArgs
{
	Uint32 StartVertex;
	Uint32 PrimitiveCount;
	Uint32 CullMode;
	Uint32 d;
};
struct PolyBuff
{
	/*I*/Direct3DVertexBuffer8 *pStreamData;
	Uint32 TotalSize;
	Uint32 CurrentSize;
	Uint32 Stride;
	Uint32 FVF;
	PolyBuff_RenderArgs *RenderArgs;
	Uint32 LockCount;
	const char *name;
	int i;
};
#pragma pack(pop)

constexpr auto DEFAULT_OPTIONS = d3d::UseAlpha | d3d::UseFog | d3d::UseLight | d3d::UseTexture;

static Uint32 shader_options = DEFAULT_OPTIONS;
static Uint32 last_options = DEFAULT_OPTIONS;

static std::vector<Uint8> shaderFile;
static Uint32 shaderCount = 0;
static Effect shaders[d3d::ShaderOptions::Count] = {};
static CComPtr<ID3DXEffectPool> pool = nullptr;

static bool initialized  = false;
static Uint32 drawing    = 0;

static Trampoline* Direct3D_PerformLighting_t         = nullptr;
static Trampoline* sub_77EAD0_t                       = nullptr;
static Trampoline* sub_77EBA0_t                       = nullptr;
static Trampoline* njDrawModel_SADX_t                 = nullptr;
static Trampoline* njDrawModel_SADX_B_t               = nullptr;
static Trampoline* Direct3D_SetProjectionMatrix_t     = nullptr;
static Trampoline* Direct3D_SetViewportAndTransform_t = nullptr;
static Trampoline* Direct3D_SetWorldTransform_t       = nullptr;
static Trampoline* CreateDirect3DDevice_t             = nullptr;
static Trampoline* PolyBuff_DrawTriangleStrip_t       = nullptr;
static Trampoline* PolyBuff_DrawTriangleList_t        = nullptr;

DataPointer(Direct3DDevice8*, Direct3D_Device, 0x03D128B0);
DataPointer(D3DXMATRIX, InverseViewMatrix, 0x0389D358);
DataPointer(D3DXMATRIX, TransformationMatrix, 0x03D0FD80);
DataPointer(D3DXMATRIX, ViewMatrix, 0x0389D398);
DataPointer(D3DXMATRIX, WorldMatrix, 0x03D12900);
DataPointer(D3DXMATRIX, _ProjectionMatrix, 0x03D129C0);
DataPointer(int, TransformAndViewportInvalid, 0x03D0FD1C);

namespace d3d
{
	IDirect3DDevice9* device = nullptr;
	Effect effect = nullptr;
	bool do_effect = false;
}

namespace param
{
	EffectParameter<Texture> BaseTexture("BaseTexture", nullptr);

	EffectParameter<Texture> PaletteA("PaletteA", nullptr);
	EffectParameter<float> DiffuseIndexA("DiffuseIndexA", 0.0f);
	EffectParameter<float> SpecularIndexA("SpecularIndexA", 0.0f);

	EffectParameter<Texture> PaletteB("PaletteB", nullptr);
	EffectParameter<float> DiffuseIndexB("DiffuseIndexB", 0.0f);
	EffectParameter<float> SpecularIndexB("SpecularIndexB", 0.0f);

	EffectParameter<float> BlendFactor("BlendFactor", 0.0f);
	EffectParameter<D3DXMATRIX> WorldMatrix("WorldMatrix", {});
	EffectParameter<D3DXMATRIX> wvMatrix("wvMatrix", {});
	EffectParameter<D3DXMATRIX> ProjectionMatrix("ProjectionMatrix", {});
	EffectParameter<D3DXMATRIX> wvMatrixInvT("wvMatrixInvT", {});
	EffectParameter<D3DXMATRIX> TextureTransform("TextureTransform", {});
	EffectParameter<int> FogMode("FogMode", 0);
	EffectParameter<float> FogStart("FogStart", 0.0f);
	EffectParameter<float> FogEnd("FogEnd", 0.0f);
	EffectParameter<float> FogDensity("FogDensity", 0.0f);
	EffectParameter<D3DXCOLOR> FogColor("FogColor", {});
	EffectParameter<D3DXVECTOR3> LightDirection("LightDirection", {});
	EffectParameter<int> DiffuseSource("DiffuseSource", 0);

	EffectParameter<D3DXCOLOR> MaterialDiffuse("MaterialDiffuse", {});
	EffectParameter<float> AlphaRef("AlphaRef", 0.0f);
	EffectParameter<D3DXVECTOR3> NormalScale("NormalScale", { 1.0f, 1.0f, 1.0f });

#ifdef USE_SL
	EffectParameter<D3DXCOLOR> MaterialSpecular("MaterialSpecular", {});
	EffectParameter<float> MaterialPower("MaterialPower", 1.0f);
	EffectParameter<SourceLight_t> SourceLight("SourceLight", {});
#endif

	static IEffectParameter* const parameters[] = {
		&BaseTexture,

		&PaletteA,
		&DiffuseIndexA,
		&SpecularIndexA,

		&PaletteB,
		&DiffuseIndexB,
		&SpecularIndexB,
		
		&BlendFactor,
		&WorldMatrix,
		&wvMatrix,
		&ProjectionMatrix,
		&wvMatrixInvT,
		&TextureTransform,
		&FogMode,
		&FogStart,
		&FogEnd,
		&FogDensity,
		&FogColor,
		&LightDirection,
		&DiffuseSource,
		&MaterialDiffuse,
		&AlphaRef,
		&NormalScale,

#ifdef USE_SL
		&SourceLight,
		&MaterialSpecular,
		&MaterialPower,
		&UseSourceLight,
#endif
	};
}

static void UpdateParameterHandles()
{
	for (auto i : param::parameters)
	{
		i->UpdateHandle(d3d::effect);
	}
}

static auto sanitize(Uint32& options)
{
	return options &= d3d::Mask;
}

static std::vector<D3DXMACRO> macros;
static Effect compileShader(Uint32 options)
{
	sanitize(options);
	PrintDebug("[lantern] Compiling shader #%02d: %08X\n", ++shaderCount, options);

	if (pool == nullptr)
	{
		if (FAILED(D3DXCreateEffectPool(&pool)))
		{
			throw std::runtime_error("Failed to create effect pool?!");
		}
	}

	macros.clear();
	auto o = options;

	while (o != 0)
	{
		if (o & d3d::UseTexture)
		{
			o &= ~d3d::UseTexture;
			macros.push_back({ "USE_TEXTURE", "1" });
			continue;
		}

		if (o & d3d::UseEnvMap)
		{
			o &= ~d3d::UseEnvMap;
			macros.push_back({ "USE_ENVMAP", "1" });
			continue;
		}

		if (o & d3d::UseLight)
		{
			o &= ~d3d::UseLight;
			macros.push_back({ "USE_LIGHT", "1" });
			continue;
		}

		if (o & d3d::UseBlend)
		{
			o &= ~d3d::UseBlend;
			macros.push_back({ "USE_BLEND", "1" });
			continue;
		}

		if (o & d3d::UseAlpha)
		{
			o &= ~d3d::UseAlpha;
			macros.push_back({ "USE_ALPHA", "1" });
			continue;
		}

		if (o & d3d::UseFog)
		{
			o &= ~d3d::UseFog;
			macros.push_back({ "USE_FOG", "1" });
			continue;
		}

		break;
	}

	macros.push_back({});

	ID3DXBuffer* errors = nullptr;
	Effect effect;

	auto path = globals::system + "lantern.fx";

	if (shaderFile.empty())
	{
		std::ifstream file(path, std::ios::ate);
		auto size = file.tellg();
		file.seekg(0);

		if (file.is_open() && size > 0)
		{
			shaderFile.resize((size_t)size);
			file.read((char*)shaderFile.data(), size);
		}

		file.close();
	}

	if (!shaderFile.empty())
	{
		auto result = D3DXCreateEffect(d3d::device, shaderFile.data(), shaderFile.size(), macros.data(), nullptr,
			D3DXFX_NOT_CLONEABLE | D3DXFX_DONOTSAVESTATE | D3DXFX_DONOTSAVESAMPLERSTATE,
			pool, &effect, &errors);

		if (FAILED(result) || errors)
		{
			if (errors)
			{
				std::string compilationErrors(static_cast<const char*>(
					errors->GetBufferPointer()));

				errors->Release();
				throw std::runtime_error(compilationErrors);
			}
		}
	}

	if (effect == nullptr)
	{
		throw std::runtime_error("Shader creation failed with an unknown error. (Does " + path + " exist?)");
	}

	effect->SetTechnique("Main");
	shaders[options] = effect;
	return effect;
}

using namespace d3d;

static void begin()
{
	++drawing;
}
static void end()
{
	if (effect == nullptr || drawing > 0 && --drawing < 1)
	{
		drawing = 0;
		do_effect = false;
	}
}

static bool began_effect = false;
static void end_effect()
{
	if (effect != nullptr && began_effect)
	{
		effect->EndPass();
		effect->End();
	}

	began_effect = false;
}

static void start_effect()
{
	if (!do_effect || effect == nullptr
		|| do_effect && !drawing)
	{
		end_effect();
		return;
	}

	bool changes = false;

	if (sanitize(shader_options) && shader_options != last_options)
	{
		end_effect();
		changes = true;

		last_options = shader_options;
		auto e = shaders[shader_options];
		if (e == nullptr)
		{
			try
			{
				e = compileShader(shader_options);
			}
			catch (std::exception& ex)
			{
				effect = nullptr;
				end_effect();
				MessageBoxA(WindowHandle, ex.what(), "Shader creation failed", MB_OK | MB_ICONERROR);
				return;
			}
		}

		effect = e;
	}

	for (auto& it : param::parameters)
	{
		if (it->Commit(effect))
		{
			changes = true;
		}
	}

	if (began_effect)
	{
		if (changes)
		{
			effect->CommitChanges();
		}
	}
	else
	{
		UINT passes = 0;
		if (FAILED(effect->Begin(&passes, 0)))
		{
			throw std::runtime_error("Failed to begin shader!");
		}

		if (passes > 1)
		{
			throw std::runtime_error("Multi-pass shaders are not supported.");
		}

		if (FAILED(effect->BeginPass(0)))
		{
			throw std::runtime_error("Failed to begin pass!");
		}

		began_effect = true;
	}
}

template<typename T, typename... Args>
void RunTrampoline(const T& original, Args... args)
{
	begin();
	original(args...);
	end();
}

static void DrawPolyBuff(PolyBuff* _this, D3DPRIMITIVETYPE type)
{
	/*
	 * This isn't ideal where mod compatibility is concerned.
	 * Since we're not calling the trampoline, this must be the
	 * last mod loaded in order for things to work nicely.
	 */

	Uint32 cullmode = D3DCULL_FORCE_DWORD;
	auto args = _this->RenderArgs;

	for (auto i = _this->LockCount; i; --i)
	{
		if (args->CullMode != cullmode)
		{
			Direct3D_Device->SetRenderState(D3DRS_CULLMODE, args->CullMode);
			cullmode = args->CullMode;
		}

		Direct3D_Device->DrawPrimitive(type, args->StartVertex, args->PrimitiveCount);
		++args;
	}

	_this->LockCount = 0;
}

static void SetLightParameters()
{
	if (!LanternInstance::UsePalette() || effect == nullptr)
	{
		return;
	}

	D3DLIGHT9 light;
	device->GetLight(0, &light);
	param::LightDirection = -*(D3DXVECTOR3*)&light.Direction;
}

#pragma region Trampolines

static void __cdecl sub_77EAD0_r(void* a1, int a2, int a3)
{
	begin();
	RunTrampoline(TARGET_DYNAMIC(sub_77EAD0), a1, a2, a3);
	end();
}

static void __cdecl sub_77EBA0_r(void* a1, int a2, int a3)
{
	begin();
	RunTrampoline(TARGET_DYNAMIC(sub_77EBA0), a1, a2, a3);
	end();
}

static void __cdecl njDrawModel_SADX_r(NJS_MODEL_SADX* a1)
{
	begin();
	RunTrampoline(TARGET_DYNAMIC(njDrawModel_SADX), a1);
	end();
}

static void __cdecl njDrawModel_SADX_B_r(NJS_MODEL_SADX* a1)
{
	begin();
	RunTrampoline(TARGET_DYNAMIC(njDrawModel_SADX_B), a1);
	end();
}

static void __fastcall PolyBuff_DrawTriangleStrip_r(PolyBuff* _this)
{
	begin();
	DrawPolyBuff(_this, D3DPT_TRIANGLESTRIP);
	end();
}

static void __fastcall PolyBuff_DrawTriangleList_r(PolyBuff* _this)
{
	begin();
	DrawPolyBuff(_this, D3DPT_TRIANGLELIST);
	end();
}

static void hookVtbl();
static void __fastcall CreateDirect3DDevice_r(int a1, int behavior, int type)
{
	TARGET_DYNAMIC(CreateDirect3DDevice)(a1, behavior, type);
	if (Direct3D_Device != nullptr && !initialized)
	{
		device = Direct3D_Device->GetProxyInterface();
		initialized = true;
		LoadShader();
		hookVtbl();
	}
}

static void __cdecl Direct3D_SetWorldTransform_r()
{
	TARGET_DYNAMIC(Direct3D_SetWorldTransform)();

	if (!LanternInstance::UsePalette() || effect == nullptr)
	{
		return;
	}

	param::WorldMatrix = WorldMatrix;

	auto wvMatrix = WorldMatrix * ViewMatrix;
	param::wvMatrix = wvMatrix;

	D3DXMatrixInverse(&wvMatrix, nullptr, &wvMatrix);
	D3DXMatrixTranspose(&wvMatrix, &wvMatrix);
	// The inverse transpose matrix is used for environment mapping.
	param::wvMatrixInvT = wvMatrix;
}

static void __stdcall Direct3D_SetProjectionMatrix_r(float hfov, float nearPlane, float farPlane)
{
	TARGET_DYNAMIC(Direct3D_SetProjectionMatrix)(hfov, nearPlane, farPlane);

	if (effect == nullptr)
	{
		return;
	}

	// The view matrix can also be set here if necessary.
	param::ProjectionMatrix = _ProjectionMatrix * TransformationMatrix;
}

static void __cdecl Direct3D_SetViewportAndTransform_r()
{
	auto original = TARGET_DYNAMIC(Direct3D_SetViewportAndTransform);
	bool invalid = TransformAndViewportInvalid != 0;
	original();

	if (effect != nullptr && invalid)
	{
		param::ProjectionMatrix = _ProjectionMatrix * TransformationMatrix;
	}
}

static void __cdecl Direct3D_PerformLighting_r(int type)
{
	auto target = TARGET_DYNAMIC(Direct3D_PerformLighting);

	if (effect == nullptr || !LanternInstance::UsePalette())
	{
		target(type);
		return;
	}

	// This specifically force light type 0 to prevent
	// the light direction from being overwritten.
	target(0);
	SetShaderOptions(UseLight, true);

	if (type != globals::light_type)
	{
		SetLightParameters();
	}

	globals::palettes.SetPalettes(type, globals::no_specular ? NJD_FLAG_IGNORE_SPECULAR : 0);
}

#pragma endregion

static void __stdcall DrawMeshSetBuffer_c(MeshSetBuffer* buffer)
{
	if (!buffer->FVF)
	{
		return;
	}

	Direct3D_Device->SetVertexShader(buffer->FVF);
	Direct3D_Device->SetStreamSource(0, buffer->VertexBuffer, buffer->Size);

	auto indexBuffer = buffer->IndexBuffer;
	if (indexBuffer)
	{
		Direct3D_Device->SetIndices(indexBuffer, 0);

		begin();

		Direct3D_Device->DrawIndexedPrimitive(
			buffer->PrimitiveType,
			buffer->MinIndex,
			buffer->NumVertecies,
			buffer->StartIndex,
			buffer->PrimitiveCount);
	}
	else
	{
		begin();

		Direct3D_Device->DrawPrimitive(
			buffer->PrimitiveType,
			buffer->StartIndex,
			buffer->PrimitiveCount);
	}

	end();
}

static const auto loc_77EF09 = (void*)0x0077EF09;
static void __declspec(naked) DrawMeshSetBuffer_asm()
{
	__asm
	{
		push esi
		call DrawMeshSetBuffer_c
		jmp  loc_77EF09
	}
}

static auto __stdcall SetTransformHijack(Direct3DDevice8* _device, D3DTRANSFORMSTATETYPE type, D3DXMATRIX* matrix)
{
	if (effect != nullptr)
	{
		param::ProjectionMatrix = *matrix;
	}

	return Direct3D_Device->SetTransform(type, matrix);
}

void releaseParameters()
{
	for (auto& i : param::parameters)
	{
		i->Release();
	}
}

void releaseShaders()
{
	shaderFile.clear();
	effect = nullptr;

	for (auto& e : shaders)
	{
		e = nullptr;
	}

	shaderCount = 0;
	pool = nullptr;
}

enum
{
	IndexOf_DrawPrimitive = 81,
	IndexOf_DrawIndexedPrimitive,
	IndexOf_DrawPrimitiveUP,
	IndexOf_DrawIndexedPrimitiveUP
};

#define D3DORIG(NAME) \
	NAME ## _orig

HRESULT __stdcall DrawPrimitive(IDirect3DDevice9* _this, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount);
HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* _this, D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount);
HRESULT __stdcall DrawPrimitiveUP(IDirect3DDevice9* _this, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride);
HRESULT __stdcall DrawIndexedPrimitiveUP(IDirect3DDevice9* _this, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride);

decltype(DrawPrimitive)*          DrawPrimitive_orig          = nullptr;
decltype(DrawIndexedPrimitive)*   DrawIndexedPrimitive_orig   = nullptr;
decltype(DrawPrimitiveUP)*        DrawPrimitiveUP_orig        = nullptr;
decltype(DrawIndexedPrimitiveUP)* DrawIndexedPrimitiveUP_orig = nullptr;

HRESULT __stdcall DrawPrimitive(IDirect3DDevice9* _this, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
	start_effect();
	auto result = D3DORIG(DrawPrimitive)(_this, PrimitiveType, StartVertex, PrimitiveCount);
	end_effect();
	return result;
}
HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* _this, D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
{
	start_effect();
	auto result = D3DORIG(DrawIndexedPrimitive)(_this, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
	end_effect();
	return result;
}
HRESULT __stdcall DrawPrimitiveUP(IDirect3DDevice9* _this, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	start_effect();
	auto result = D3DORIG(DrawPrimitiveUP)(_this, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
	end_effect();
	return result;
}
HRESULT __stdcall DrawIndexedPrimitiveUP(IDirect3DDevice9* _this, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	start_effect();
	auto result = D3DORIG(DrawIndexedPrimitiveUP)(_this, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
	end_effect();
	return result;
}

static void hookVtbl()
{
	auto vtbl = (void**)(*(void**)device);

#define HOOK(NAME) \
	MH_CreateHook(vtbl[IndexOf_ ## NAME], NAME, (LPVOID*)& ## NAME ## _orig)

	HOOK(DrawPrimitive);
	HOOK(DrawIndexedPrimitive);
	HOOK(DrawPrimitiveUP);
	HOOK(DrawIndexedPrimitiveUP);

	MH_EnableHook(MH_ALL_HOOKS);
}

void d3d::LoadShader()
{
	if (!initialized)
	{
		return;
	}

	releaseShaders();

	try
	{
		effect = compileShader(DEFAULT_OPTIONS);
		UpdateParameterHandles();
	}
	catch (std::exception& ex)
	{
		effect = nullptr;
		MessageBoxA(WindowHandle, ex.what(), "Shader creation failed", MB_OK | MB_ICONERROR);
	}
}

void d3d::SetShaderOptions(Uint32 options, bool add)
{
	if (add)
	{
		shader_options |= options;
	}
	else
	{
		shader_options &= ~options;
	}
}

void d3d::InitTrampolines()
{
	Direct3D_PerformLighting_t         = new Trampoline(0x00412420, 0x00412426, Direct3D_PerformLighting_r);
	sub_77EAD0_t                       = new Trampoline(0x0077EAD0, 0x0077EAD7, sub_77EAD0_r);
	sub_77EBA0_t                       = new Trampoline(0x0077EBA0, 0x0077EBA5, sub_77EBA0_r);
	njDrawModel_SADX_t                 = new Trampoline(0x0077EDA0, 0x0077EDAA, njDrawModel_SADX_r);
	njDrawModel_SADX_B_t               = new Trampoline(0x00784AE0, 0x00784AE5, njDrawModel_SADX_B_r);
	Direct3D_SetProjectionMatrix_t     = new Trampoline(0x00791170, 0x00791175, Direct3D_SetProjectionMatrix_r);
	Direct3D_SetViewportAndTransform_t = new Trampoline(0x007912E0, 0x007912E8, Direct3D_SetViewportAndTransform_r);
	Direct3D_SetWorldTransform_t       = new Trampoline(0x00791AB0, 0x00791AB5, Direct3D_SetWorldTransform_r);
	CreateDirect3DDevice_t             = new Trampoline(0x00794000, 0x00794007, CreateDirect3DDevice_r);
	PolyBuff_DrawTriangleStrip_t       = new Trampoline(0x00794760, 0x00794767, PolyBuff_DrawTriangleStrip_r);
	PolyBuff_DrawTriangleList_t        = new Trampoline(0x007947B0, 0x007947B7, PolyBuff_DrawTriangleList_r);

	WriteJump((void*)0x0077EE45, DrawMeshSetBuffer_asm);

	// Hijacking a IDirect3DDevice8::SetTransform call in Direct3D_SetNearFarPlanes
	// to update the projection matrix.
	// This nops:
	// mov ecx, [eax] (device)
	// call dword ptr [ecx+94h] (device->SetTransform)
	WriteData((void*)0x00403234, 0x90i8, 8);
	WriteCall((void*)0x00403236, SetTransformHijack);
}

// These exports are for the window resize branch of the mod loader.
extern "C"
{
	EXPORT void __cdecl OnRenderDeviceLost()
	{
		end();

		for (auto& e : shaders)
		{
			if (e != nullptr)
			{
				e->OnLostDevice();
			}
		}
	}

	EXPORT void __cdecl OnRenderDeviceReset()
	{
		for (auto& e : shaders)
		{
			if (e != nullptr)
			{
				e->OnResetDevice();
			}
		}

		UpdateParameterHandles();
	}

	EXPORT void __cdecl OnExit()
	{
		releaseParameters();
		releaseShaders();
	}
}
