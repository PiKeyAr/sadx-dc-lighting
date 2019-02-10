#include "stdafx.h"

#include <Windows.h>
#include <WinCrypt.h>

// Direct3D
#include <d3dx9.h>

// d3d8to9
#include <d3d8to9.hpp>

// Mod loader
#include <SADXModLoader.h>
#include <Trampoline.h>

// MinHook
#include <MinHook.h>

// Standard library
#include <iomanip>
#include <sstream>
#include <vector>
#include <functional>

// Local
#include "d3d.h"
#include "datapointers.h"
#include "globals.h"
#include "../include/lanternapi.h"
#include "ShaderParameter.h"
#include "FileSystem.h"

#define STORE_RS(RS) \
	DWORD RS; \
	d3d::device->GetRenderState(D3DRS_ ## RS, &RS)

#define RESTORE_RS(RS) \
	d3d::device->SetRenderState(D3DRS_ ## RS, RS);

template <class T>
inline void hash_combine(std::size_t& seed, const T& v)
{
	std::hash<T> hasher;
	seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct OhKey
{
	int i;
	void* ptr;

	bool operator==(const OhKey& rhs) const
	{
		return i == rhs.i && ptr == rhs.ptr;
	}
};

namespace std
{
	template <>
	struct hash<OhKey>
	{
		size_t operator()(const OhKey& x) const noexcept
		{
			auto h = std::hash<int>()(x.i);
			hash_combine(h, x.ptr);
			return h;
		}
	};
}

static bool blur_enabled = false;
static int blur_i = 0;
static NJS_MODEL_SADX* curr_model = nullptr;

static bool blur_begin()
{
	if (!blur_enabled)
	{
		return false;
	}

	return blur_i++ == 0;
}

static void blur_end()
{
	blur_i = 0;
}

namespace param
{
	ShaderParameter<Texture>     PaletteA(1, nullptr, IShaderParameter::Type::vertex);
	ShaderParameter<Texture>     PaletteB(2, nullptr, IShaderParameter::Type::vertex);

	ShaderParameter<D3DXMATRIX>  wvMatrix(4, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  wvMatrixInvT(12, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  TextureTransform(16, {}, IShaderParameter::Type::vertex);

	ShaderParameter<D3DXVECTOR3> NormalScale(20, { 1.0f, 1.0f, 1.0f }, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXVECTOR3> LightDirection(21, { 0.0f, -1.0f, 0.0f }, IShaderParameter::Type::vertex);
	ShaderParameter<int>         DiffuseSource(22, 0, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXCOLOR>   MaterialDiffuse(23, { 0.0f, 0.0f, 0.0f, 0.0f }, IShaderParameter::Type::vertex);
	
	ShaderParameter<D3DXVECTOR4> Indices(24, { 0.0f, 0.0f, 0.0f, 0.0f }, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXVECTOR2> BlendFactor(25, { 0.0f, 0.0f }, IShaderParameter::Type::vertex);

	ShaderParameter<bool>        AllowVertexColor(26, true, IShaderParameter::Type::vertex);
	ShaderParameter<bool>        ForceDefaultDiffuse(27, false, IShaderParameter::Type::vertex);
	ShaderParameter<bool>        DiffuseOverride(28, false, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXVECTOR3> DiffuseOverrideColor(29, { 1.0f, 1.0f, 1.0f }, IShaderParameter::Type::vertex);

	ShaderParameter<int>         FogMode(30, 0, IShaderParameter::Type::pixel);
	ShaderParameter<D3DXVECTOR3> FogConfig(31, {}, IShaderParameter::Type::pixel);
	ShaderParameter<D3DXCOLOR>   FogColor(32, {}, IShaderParameter::Type::pixel);
	ShaderParameter<float>       AlphaRef(33, 16.0f / 255.0f, IShaderParameter::Type::pixel);

	ShaderParameter<D3DXMATRIX>  WorldMatrix(40, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  ViewMatrix(44, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  ProjectionMatrix(48, {}, IShaderParameter::Type::vertex);

	ShaderParameter<D3DXVECTOR2> Viewport(52, {}, IShaderParameter::Type::pixel);

	ShaderParameter<D3DXMATRIX>  l_WorldMatrix(60, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  l_ViewMatrix(64, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  l_ProjectionMatrix(68, {}, IShaderParameter::Type::vertex);

	IShaderParameter* const parameters[] = {
		&PaletteA,
		&PaletteB,

		&wvMatrix,
		&wvMatrixInvT,
		&TextureTransform,

		&Indices,
		&BlendFactor,

		&NormalScale,
		&LightDirection,
		&DiffuseSource,
		&MaterialDiffuse,

		&AllowVertexColor,
		&ForceDefaultDiffuse,
		&DiffuseOverride,
		&DiffuseOverrideColor,

		&FogMode,
		&FogConfig,
		&FogColor,
		&AlphaRef,

		&WorldMatrix,
		&ViewMatrix,
		&ProjectionMatrix,

		&Viewport,

		&l_WorldMatrix,
		&l_ViewMatrix,
		&l_ProjectionMatrix,
	};

	static void release_parameters()
	{
		for (auto& i : parameters)
		{
			i->release();
		}
	}
}

namespace local
{
	static Trampoline* Direct3D_PerformLighting_t         = nullptr;
	static Trampoline* sub_77EAD0_t                       = nullptr;
	static Trampoline* sub_77EBA0_t                       = nullptr;
	static Trampoline* njDrawModel_SADX_t                 = nullptr;
	static Trampoline* njDrawModel_SADX_Dynamic_t         = nullptr;
	static Trampoline* Direct3D_SetProjectionMatrix_t     = nullptr;
	static Trampoline* Direct3D_SetViewportAndTransform_t = nullptr;
	static Trampoline* Direct3D_SetWorldTransform_t       = nullptr;
	static Trampoline* CreateDirect3DDevice_t             = nullptr;
	static Trampoline* PolyBuff_DrawTriangleStrip_t       = nullptr;
	static Trampoline* PolyBuff_DrawTriangleList_t        = nullptr;

	static HRESULT __stdcall SetTransform_r(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix);

	static HRESULT __stdcall DrawPrimitive_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT StartVertex,
		UINT PrimitiveCount);
	static HRESULT __stdcall DrawIndexedPrimitive_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		INT BaseVertexIndex,
		UINT MinVertexIndex,
		UINT NumVertices,
		UINT startIndex,
		UINT primCount);
	static HRESULT __stdcall DrawPrimitiveUP_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT PrimitiveCount,
		CONST void* pVertexStreamZeroData,
		UINT VertexStreamZeroStride);
	static HRESULT __stdcall DrawIndexedPrimitiveUP_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT MinVertexIndex,
		UINT NumVertices,
		UINT PrimitiveCount,
		CONST void* pIndexData,
		D3DFORMAT IndexDataFormat,
		CONST void* pVertexStreamZeroData,
		UINT VertexStreamZeroStride);

	static decltype(SetTransform_r)*           SetTransform_t           = nullptr;
	static decltype(DrawPrimitive_r)*          DrawPrimitive_t          = nullptr;
	static decltype(DrawIndexedPrimitive_r)*   DrawIndexedPrimitive_t   = nullptr;
	static decltype(DrawPrimitiveUP_r)*        DrawPrimitiveUP_t        = nullptr;
	static decltype(DrawIndexedPrimitiveUP_r)* DrawIndexedPrimitiveUP_t = nullptr;

	constexpr auto COMPILER_FLAGS = D3DXSHADER_PACKMATRIX_ROWMAJOR | D3DXSHADER_OPTIMIZATION_LEVEL3;

	constexpr auto DEFAULT_FLAGS = ShaderFlags_Alpha | ShaderFlags_Fog | ShaderFlags_Light | ShaderFlags_Texture;
	constexpr auto VS_FLAGS = ShaderFlags_Texture | ShaderFlags_EnvMap | ShaderFlags_Light | ShaderFlags_Blend;
	constexpr auto PS_FLAGS = ShaderFlags_Texture | ShaderFlags_Alpha | ShaderFlags_Fog;

	static Uint32 shader_flags = DEFAULT_FLAGS;
	static Uint32 last_flags = DEFAULT_FLAGS;

	static std::vector<uint8_t> shader_file;
	static std::unordered_map<ShaderFlags, VertexShader> vertex_shaders;
	static std::unordered_map<ShaderFlags, PixelShader> pixel_shaders;

	static bool initialized = false;
	static Uint32 drawing = 0;
	static bool using_shader = false;
	static bool supports_xrgb = false;
	static std::vector<D3DXMACRO> macros;

	DataPointer(Direct3DDevice8*, Direct3D_Device, 0x03D128B0);
	DataPointer(Direct3D8*, Direct3D_Object, 0x03D11F60);

	static Texture color_buffer     = nullptr;
	static Texture velocity_buffer  = nullptr;
	static Surface og_render_target = nullptr;

	VertexShader velocity_vs, quad_vs;
	PixelShader velocity_ps, quad_ps, blur_ps;
	D3DXMATRIX _proj_matrix {};
	D3DXMATRIX _view_matrix {};
	D3DXMATRIX _view_matrix_inv {};

	struct QuadVertex
	{
		static const UINT format = D3DFVF_XYZRHW | D3DFVF_TEX1;
		D3DXVECTOR4 position;
		D3DXVECTOR2 uv;
	};

	static void draw_quad()
	{
		const auto& present = PresentParameters;
		QuadVertex quad[4] = {};

		const float width_half_pixel  = present.BackBufferWidth - 0.5f;
		const float height_half_pixel = present.BackBufferHeight - 0.5f;

		constexpr float left   = 0.0f;
		constexpr float top    = 0.0f;
		constexpr float right  = 1.0f;
		constexpr float bottom = 1.0f;

		quad[0].position = D3DXVECTOR4(-0.5f, -0.5f, 0.5f, 1.0f);
		quad[0].uv       = D3DXVECTOR2(left, top);

		quad[1].position = D3DXVECTOR4(width_half_pixel, -0.5f, 0.5f, 1.0f);
		quad[1].uv       = D3DXVECTOR2(right, top);

		quad[2].position = D3DXVECTOR4(-0.5f, height_half_pixel, 0.5f, 1.0f);
		quad[2].uv       = D3DXVECTOR2(left, bottom);

		quad[3].position = D3DXVECTOR4(width_half_pixel, height_half_pixel, 0.5f, 1.0f);
		quad[3].uv       = D3DXVECTOR2(right, bottom);

		d3d::device->SetFVF(QuadVertex::format);
		d3d::device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, &quad, sizeof(QuadVertex));
	}

	static auto sanitize(Uint32& flags)
	{
		flags &= ShaderFlags_Mask;

		if (flags & ShaderFlags_Blend && !(flags & ShaderFlags_Light))
		{
			flags &= ~ShaderFlags_Blend;
		}

		if (flags & ShaderFlags_EnvMap && !(flags & ShaderFlags_Texture))
		{
			flags &= ~ShaderFlags_EnvMap;
		}

		return flags;
	}

	static void free_render_targets()
	{
		color_buffer     = nullptr;
		velocity_buffer  = nullptr;
		og_render_target = nullptr;
	}

	static void free_shaders()
	{
		vertex_shaders.clear();
		pixel_shaders.clear();
		d3d::vertex_shader = nullptr;
		d3d::pixel_shader = nullptr;

		velocity_vs = nullptr;
		velocity_ps = nullptr;
		quad_vs = nullptr;
		quad_ps = nullptr;
		blur_ps = nullptr;
	}

	static void clear_shaders()
	{
		shader_file.clear();
		free_shaders();
	}

	static VertexShader get_vertex_shader(Uint32 flags);
	static PixelShader get_pixel_shader(Uint32 flags);

	static void load_generic_vs(std::vector<uint8_t>& hlsl, VertexShader& vs);
	static void load_generic_vs(const std::string& name, VertexShader& vs);
	static void load_generic_ps(std::vector<uint8_t>& hlsl, PixelShader& ps);
	static void load_generic_ps(const std::string& name, PixelShader& ps);
	static void load_generic_shader(const std::string& name, VertexShader&, PixelShader&);

	static void create_render_targets()
	{
		using namespace d3d;

		param::Viewport = { static_cast<float>(HorizontalResolution), static_cast<float>(VerticalResolution) };
		param::Viewport.commit_now(device);

		HRESULT hr = 0;
		color_buffer = nullptr;
		velocity_buffer = nullptr;

		hr = device->CreateTexture(HorizontalResolution, VerticalResolution, 1, D3DUSAGE_RENDERTARGET,
		                           D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &color_buffer, nullptr);

		if (FAILED(hr))
		{
			throw;
		}

		hr = device->CreateTexture(HorizontalResolution, VerticalResolution, 1, D3DUSAGE_RENDERTARGET,
		                           D3DFMT_G32R32F, D3DPOOL_DEFAULT, &velocity_buffer, nullptr);

		if (FAILED(hr))
		{
			throw;
		}

		og_render_target = nullptr;
		device->GetRenderTarget(0, &og_render_target);

		Surface surface;
		color_buffer->GetSurfaceLevel(0, &surface);
		device->SetRenderTarget(0, surface);
	}

	static void create_shaders()
	{
		try
		{
			velocity_vs = nullptr;
			velocity_ps = nullptr;
			load_generic_shader("velocity.hlsl", velocity_vs, velocity_ps);
		}
		catch (const std::exception& ex)
		{
			MessageBoxA(WindowHandle, ex.what(), "VELOCITY Shader creation failed", MB_OK | MB_ICONERROR);
		}

		try
		{
			quad_vs = nullptr;
			quad_ps = nullptr;
			load_generic_shader("quad.hlsl", quad_vs, quad_ps);
		}
		catch (const std::exception& ex)
		{
			MessageBoxA(WindowHandle, ex.what(), "QUAD Shader creation failed", MB_OK | MB_ICONERROR);
		}

		try
		{
			blur_ps = nullptr;
			load_generic_ps("blur.hlsl", blur_ps);
		}
		catch (const std::exception& ex)
		{
			MessageBoxA(WindowHandle, ex.what(), "BLUR Shader creation failed", MB_OK | MB_ICONERROR);
		}

		try
		{
			d3d::vertex_shader = get_vertex_shader(DEFAULT_FLAGS);
			d3d::pixel_shader = get_pixel_shader(DEFAULT_FLAGS);

		#ifdef PRECOMPILE_SHADERS
			for (Uint32 i = 0; i < ShaderFlags_Count; i++)
			{
				auto flags = i;
				local::sanitize(flags);

				auto vs = static_cast<ShaderFlags>(flags & VS_FLAGS);
				if (vertex_shaders.find(vs) == vertex_shaders.end())
				{
					get_vertex_shader(flags);
				}

				auto ps = static_cast<ShaderFlags>(flags & PS_FLAGS);
				if (pixel_shaders.find(ps) == pixel_shaders.end())
				{
					get_pixel_shader(flags);
				}
			}
		#endif

			for (auto& i : param::parameters)
			{
				i->commit_now(d3d::device);
			}
		}
		catch (std::exception& ex)
		{
			d3d::vertex_shader = nullptr;
			d3d::pixel_shader = nullptr;
			MessageBoxA(WindowHandle, ex.what(), "Shader creation failed", MB_OK | MB_ICONERROR);
		}
	}

	static std::string to_string(Uint32 flags)
	{
		bool thing = false;
		std::stringstream result;

		while (flags != 0)
		{
			using namespace d3d;

			if (thing)
			{
				result << " | ";
			}

			if (flags & ShaderFlags_Fog)
			{
				flags &= ~ShaderFlags_Fog;
				result << "USE_FOG";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_Blend)
			{
				flags &= ~ShaderFlags_Blend;
				result << "USE_BLEND";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_Light)
			{
				flags &= ~ShaderFlags_Light;
				result << "USE_LIGHT";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_Alpha)
			{
				flags &= ~ShaderFlags_Alpha;
				result << "USE_ALPHA";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_EnvMap)
			{
				flags &= ~ShaderFlags_EnvMap;
				result << "USE_ENVMAP";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_Texture)
			{
				flags &= ~ShaderFlags_Texture;
				result << "USE_TEXTURE";
				thing = true;
				continue;
			}

			break;
		}

		return result.str();
	}

	static void create_cache()
	{
		if (!filesystem::create_directory(globals::cache_path))
		{
			throw std::exception("Failed to create cache directory!");
		}
	}

	static void invalidate_cache()
	{
		if (filesystem::exists(globals::cache_path))
		{
			if (!filesystem::remove_all(globals::cache_path))
			{
				throw std::runtime_error("Failed to delete cache directory!");
			}
		}

		create_cache();
	}

	static auto shader_hash()
	{
		HCRYPTPROV hProv = 0;
		if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
		{
			throw std::runtime_error("CryptAcquireContext failed.");
		}

		HCRYPTHASH hHash = 0;
		if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash))
		{
			CryptReleaseContext(hProv, 0);
			throw std::runtime_error("CryptCreateHash failed.");
		}

		try
		{
			if (!CryptHashData(hHash, shader_file.data(), shader_file.size(), 0)
				|| !CryptHashData(hHash, reinterpret_cast<const BYTE*>(&COMPILER_FLAGS), sizeof(COMPILER_FLAGS), 0))
			{
				throw std::runtime_error("CryptHashData failed.");
			}

			// temporary
			DWORD buffer_size = sizeof(size_t);
			// actual size
			DWORD hash_size = 0;

			if (!CryptGetHashParam(hHash, HP_HASHSIZE, reinterpret_cast<BYTE*>(&hash_size), &buffer_size, 0))
			{
				throw std::runtime_error("CryptGetHashParam failed while asking for hash buffer size.");
			}

			std::vector<uint8_t> result(hash_size);

			if (!CryptGetHashParam(hHash, HP_HASHVAL, result.data(), &hash_size, 0))
			{
				throw std::runtime_error("CryptGetHashParam failed while asking for hash value.");
			}

			CryptDestroyHash(hHash);
			CryptReleaseContext(hProv, 0);
			return result;
		}
		catch (std::exception&)
		{
			CryptDestroyHash(hHash);
			CryptReleaseContext(hProv, 0);
			throw;
		}
	}

	static void load_shader_file(const std::basic_string<char>& shader_path)
	{
		std::ifstream file(shader_path, std::ios::ate);
		auto size = file.tellg();
		file.seekg(0);

		if (file.is_open() && size > 0)
		{
			shader_file.resize(static_cast<size_t>(size));
			file.read(reinterpret_cast<char*>(shader_file.data()), size);
		}

		file.close();
	}

	static auto read_checksum(const std::basic_string<char>& checksum_path)
	{
		std::ifstream file(checksum_path, std::ios::ate | std::ios::binary);
		auto size = file.tellg();
		file.seekg(0);

		if (size > 256 || size < 1)
		{
			throw std::runtime_error("checksum.bin file size out of range");
		}

		std::vector<uint8_t> data(static_cast<size_t>(size));
		file.read(reinterpret_cast<char*>(data.data()), data.size());
		file.close();

		return data;
	}

	static void store_checksum(const std::vector<uint8_t>& current_hash, const std::basic_string<char>& checksum_path)
	{
		invalidate_cache();

		std::ofstream file(checksum_path, std::ios::binary | std::ios::out);

		if (!file.is_open())
		{
			std::string error = "Failed to open file for writing: " + checksum_path;
			throw std::exception(error.c_str());
		}

		file.write(reinterpret_cast<const char*>(current_hash.data()), current_hash.size());
		file.close();
	}

	static auto shader_id(Uint32 flags)
	{
		std::stringstream result;

		result << std::hex
			<< std::setw(2)
			<< std::setfill('0')
			<< flags;

		return result.str();
	}

	static void populate_macros(Uint32 flags)
	{
		while (flags != 0)
		{
			using namespace d3d;

			if (flags & ShaderFlags_Texture)
			{
				flags &= ~ShaderFlags_Texture;
				macros.push_back({ "USE_TEXTURE", "1" });
				continue;
			}

			if (flags & ShaderFlags_EnvMap)
			{
				flags &= ~ShaderFlags_EnvMap;
				macros.push_back({ "USE_ENVMAP", "1" });
				continue;
			}

			if (flags & ShaderFlags_Light)
			{
				flags &= ~ShaderFlags_Light;
				macros.push_back({ "USE_LIGHT", "1" });
				continue;
			}

			if (flags & ShaderFlags_Blend)
			{
				flags &= ~ShaderFlags_Blend;
				macros.push_back({ "USE_BLEND", "1" });
				continue;
			}

			if (flags & ShaderFlags_Alpha)
			{
				flags &= ~ShaderFlags_Alpha;
				macros.push_back({ "USE_ALPHA", "1" });
				continue;
			}

			if (flags & ShaderFlags_Fog)
			{
				flags &= ~ShaderFlags_Fog;
				macros.push_back({ "USE_FOG", "1" });
				continue;
			}

			break;
		}

		macros.push_back({});
	}

	static __declspec(noreturn) void d3d_exception(const Buffer& buffer, HRESULT code)
	{
		using namespace std;

		stringstream message;

		message << '['
			<< hex
			<< setw(8)
			<< setfill('0')
			<< code;

		message << "] ";

		if (buffer != nullptr)
		{
			message << reinterpret_cast<const char*>(buffer->GetBufferPointer());
		}
		else
		{
			message << "Unspecified error.";
		}

		throw runtime_error(message.str());
	}

	static void check_shader_cache()
	{
		load_shader_file(globals::shader_path);

		const std::string checksum_path = filesystem::combine_path(globals::cache_path, "checksum.bin");
		const std::vector<uint8_t> current_hash = shader_hash();

		if (filesystem::exists(globals::cache_path))
		{
			if (!filesystem::exists(checksum_path))
			{
				store_checksum(current_hash, checksum_path);
			}
			else
			{
				const std::vector<uint8_t> last_hash(read_checksum(checksum_path));

				if (last_hash != current_hash)
				{
					store_checksum(current_hash, checksum_path);
				}
			}
		}
		else
		{
			store_checksum(current_hash, checksum_path);
		}
	}

	static void load_cached_shader(const std::string& sid_path, std::vector<uint8_t>& data)
	{
		std::ifstream file(sid_path, std::ios_base::ate | std::ios_base::binary);
		auto size = file.tellg();
		file.seekg(0);

		if (size < 1)
		{
			throw std::runtime_error("corrupt vertex shader cache");
		}

		data.resize(static_cast<size_t>(size));
		file.read(reinterpret_cast<char*>(data.data()), data.size());
	}

	static void save_cached_shader(const std::string& sid_path, std::vector<uint8_t>& data)
	{
		std::ofstream file(sid_path, std::ios_base::binary);

		if (!file.is_open())
		{
			throw std::runtime_error("Failed to open file for cache storage.");
		}

		file.write(reinterpret_cast<char*>(data.data()), data.size());
	}

	static VertexShader get_vertex_shader(Uint32 flags)
	{
		using namespace std;

		sanitize(flags);
		flags &= VS_FLAGS;

		if (shader_file.empty())
		{
			check_shader_cache();
		}
		else
		{
			const auto it = vertex_shaders.find(static_cast<ShaderFlags>(flags));
			if (it != vertex_shaders.end())
			{
				return it->second;
			}
		}

		macros.clear();

		const string sid_path = filesystem::combine_path(globals::cache_path, shader_id(flags) + ".vs");
		bool is_cached = filesystem::exists(sid_path);

		vector<uint8_t> data;

		if (is_cached)
		{
			PrintDebug("[lantern] Loading cached vertex shader #%02d: %08X (%s)\n",
				vertex_shaders.size(), flags, to_string(flags).c_str());

			load_cached_shader(sid_path, data);
		}
		else
		{
			PrintDebug("[lantern] Compiling vertex shader #%02d: %08X (%s)\n",
				vertex_shaders.size(), flags, to_string(flags).c_str());

			populate_macros(flags);

			Buffer errors;
			Buffer buffer;

			auto result = D3DXCompileShader(reinterpret_cast<char*>(shader_file.data()), shader_file.size(), macros.data(), nullptr,
				"vs_main", "vs_3_0", COMPILER_FLAGS, &buffer, &errors, nullptr);

			if (FAILED(result) || errors != nullptr)
			{
				d3d_exception(errors, result);
			}

			data.resize(static_cast<size_t>(buffer->GetBufferSize()));
			memcpy(data.data(), buffer->GetBufferPointer(), data.size());
		}

		VertexShader shader;
		auto result = d3d::device->CreateVertexShader(reinterpret_cast<const DWORD*>(data.data()), &shader);

		if (FAILED(result))
		{
			d3d_exception(nullptr, result);
		}

		if (!is_cached)
		{
			save_cached_shader(sid_path, data);
		}

		vertex_shaders[static_cast<ShaderFlags>(flags)] = shader;
		return shader;
	}

	static PixelShader get_pixel_shader(Uint32 flags)
	{
		using namespace std;

		if (shader_file.empty())
		{
			check_shader_cache();
		}
		else
		{
			const auto it = pixel_shaders.find(static_cast<ShaderFlags>(flags & PS_FLAGS));
			if (it != pixel_shaders.end())
			{
				return it->second;
			}
		}

		macros.clear();

		sanitize(flags);
		flags &= PS_FLAGS;

		const string sid_path = filesystem::combine_path(globals::cache_path, shader_id(flags) + ".ps");
		bool is_cached = filesystem::exists(sid_path);

		vector<uint8_t> data;

		if (is_cached)
		{
			PrintDebug("[lantern] Loading cached pixel shader #%02d: %08X (%s)\n",
				pixel_shaders.size(), flags, to_string(flags).c_str());

			load_cached_shader(sid_path, data);
		}
		else
		{
			PrintDebug("[lantern] Compiling pixel shader #%02d: %08X (%s)\n",
				pixel_shaders.size(), flags, to_string(flags).c_str());

			populate_macros(flags);

			Buffer errors;
			Buffer buffer;

			auto result = D3DXCompileShader(reinterpret_cast<char*>(shader_file.data()), shader_file.size(), macros.data(), nullptr,
				"ps_main", "ps_3_0", COMPILER_FLAGS, &buffer, &errors, nullptr);

			if (FAILED(result) || errors != nullptr)
			{
				d3d_exception(errors, result);
			}

			data.resize(static_cast<size_t>(buffer->GetBufferSize()));
			memcpy(data.data(), buffer->GetBufferPointer(), data.size());
		}

		PixelShader shader;
		auto result = d3d::device->CreatePixelShader(reinterpret_cast<const DWORD*>(data.data()), &shader);

		if (FAILED(result))
		{
			d3d_exception(nullptr, result);
		}

		if (!is_cached)
		{
			save_cached_shader(sid_path, data);
		}

		pixel_shaders[static_cast<ShaderFlags>(flags & PS_FLAGS)] = shader;
		return shader;
	}

	std::vector<uint8_t> load_hlsl(const std::string& name)
	{
		std::vector<uint8_t> hlsl;

		{
			std::ifstream file(globals::get_system_path(name), std::ios::ate | std::ios::binary);

			if (!file.is_open())
			{
				std::stringstream ss;
				ss << "Error loading shader source: " << name;
				throw std::runtime_error(ss.str().c_str()); // ew
			}

			auto size = file.tellg();
			file.seekg(0);

			hlsl.resize(static_cast<size_t>(size));
			file.read(reinterpret_cast<char*>(hlsl.data()), hlsl.size());
		}

		return hlsl;
	}

	static void load_generic_vs(std::vector<uint8_t>& hlsl, VertexShader& vs)
	{
		std::vector<uint8_t> vs_data;
		Buffer errors;
		Buffer buffer;

		auto result = D3DXCompileShader(reinterpret_cast<const char*>(hlsl.data()), hlsl.size(), nullptr, nullptr,
		                                "vs_main", "vs_3_0", COMPILER_FLAGS, &buffer, &errors, nullptr);

		if (FAILED(result) || errors != nullptr)
		{
			d3d_exception(errors, result);
		}

		vs_data.resize(static_cast<size_t>(buffer->GetBufferSize()));
		memcpy(vs_data.data(), buffer->GetBufferPointer(), vs_data.size());

		result = d3d::device->CreateVertexShader(reinterpret_cast<const DWORD*>(vs_data.data()), &vs);

		if (FAILED(result))
		{
			d3d_exception(nullptr, result);
		}
	}

	static void load_generic_vs(const std::string& name, VertexShader& vs)
	{
		auto hlsl = load_hlsl(name);
		load_generic_vs(hlsl, vs);
	}

	static void load_generic_ps(std::vector<uint8_t>& hlsl, PixelShader& ps)
	{
		std::vector<uint8_t> ps_data;
		Buffer errors;
		Buffer buffer;

		auto result = D3DXCompileShader(reinterpret_cast<const char*>(hlsl.data()), hlsl.size(), nullptr, nullptr,
		                                "ps_main", "ps_3_0", COMPILER_FLAGS, &buffer, &errors, nullptr);

		if (FAILED(result) || errors != nullptr)
		{
			d3d_exception(errors, result);
		}

		ps_data.resize(static_cast<size_t>(buffer->GetBufferSize()));
		memcpy(ps_data.data(), buffer->GetBufferPointer(), ps_data.size());

		result = d3d::device->CreatePixelShader(reinterpret_cast<const DWORD*>(ps_data.data()), &ps);

		if (FAILED(result))
		{
			d3d_exception(nullptr, result);
		}
	}

	static void load_generic_ps(const std::string& name, PixelShader& ps)
	{
		auto hlsl = load_hlsl(name);
		load_generic_ps(hlsl, ps);
	}

	static void load_generic_shader(const std::string& name, VertexShader& vs, PixelShader& ps)
	{
		auto hlsl = load_hlsl(name);
		load_generic_vs(hlsl, vs);
		load_generic_ps(hlsl, ps);
	}

	static void begin()
	{
		++drawing;
	}

	static void end()
	{
		if (drawing > 0 && --drawing < 1)
		{
			drawing = 0;
			d3d::do_effect = false;
			d3d::reset_overrides();
		}
	}

	static void shader_end()
	{
		if (using_shader)
		{
			d3d::device->SetPixelShader(nullptr);
			d3d::device->SetVertexShader(nullptr);
			using_shader = false;
		}
	}

	static void shader_start()
	{
		if (!d3d::do_effect || !drawing)
		{
			shader_end();
			return;
		}

		globals::palettes.apply_parameters();

		bool changes = false;

		// The value here is copied so that UseBlend can be safely removed
		// when possible without permanently removing it. It's required by
		// Sky Deck, and it's only added to the flags once on stage load.
		auto flags = shader_flags;
		sanitize(flags);

		if (flags != last_flags)
		{
			VertexShader vs;
			PixelShader ps;

			changes = true;
			last_flags = flags;

			try
			{
				vs = get_vertex_shader(flags);
				ps = get_pixel_shader(flags);
			}
			catch (std::exception& ex)
			{
				shader_end();
				MessageBoxA(WindowHandle, ex.what(), "Shader creation failed", MB_OK | MB_ICONERROR);
				return;
			}

			if (!using_shader || vs != d3d::vertex_shader)
			{
				d3d::vertex_shader = vs;
				d3d::device->SetVertexShader(d3d::vertex_shader);
			}

			if (!using_shader || ps != d3d::pixel_shader)
			{
				d3d::pixel_shader = ps;
				d3d::device->SetPixelShader(d3d::pixel_shader);
			}
		}
		else if (!using_shader)
		{
			d3d::device->SetVertexShader(d3d::vertex_shader);
			d3d::device->SetPixelShader(d3d::pixel_shader);
		}

		if (changes || !IShaderParameter::values_assigned.empty())
		{
			for (auto& it : IShaderParameter::values_assigned)
			{
				it->commit(d3d::device);
			}

			IShaderParameter::values_assigned.clear();
		}

		using_shader = true;
	}

	static void set_light_parameters()
	{
		if (!LanternInstance::use_palette())
		{
			return;
		}

		D3DLIGHT9 light;
		d3d::device->GetLight(0, &light);
		param::LightDirection = -*static_cast<D3DXVECTOR3*>(&light.Direction);
	}

#define MHOOK(NAME) MH_CreateHook(vtbl[IndexOf_ ## NAME], NAME ## _r, (LPVOID*)&NAME ## _t)

	static void hook_vtable()
	{
		enum
		{
			IndexOf_SetTransform = 44,
			IndexOf_SetTexture = 65,
			IndexOf_DrawPrimitive = 81,
			IndexOf_DrawIndexedPrimitive,
			IndexOf_DrawPrimitiveUP,
			IndexOf_DrawIndexedPrimitiveUP
		};

		auto vtbl = (void**)(*(void**)d3d::device);

		MHOOK(SetTransform);
		MHOOK(DrawPrimitive);
		MHOOK(DrawIndexedPrimitive);
		MHOOK(DrawPrimitiveUP);
		MHOOK(DrawIndexedPrimitiveUP);

		MH_EnableHook(MH_ALL_HOOKS);
	}

#pragma region Trampolines

	template<typename T, typename... Args>
	static void run_trampoline(const T& original, Args... args)
	{
		begin();
		original(args...);
		end();
	}

	static void __cdecl sub_77EAD0_r(void* a1, int a2, int a3)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(sub_77EAD0), a1, a2, a3);
		end();
	}

	static void __cdecl sub_77EBA0_r(void* a1, int a2, int a3)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(sub_77EBA0), a1, a2, a3);
		end();
	}

	static constexpr auto age_threshold = 4;

	struct OhGod
	{
		uint32_t time;
		D3DXMATRIX world, view, proj;
	};

	std::unordered_map<OhKey, OhGod> transform_map;

	static void store_transform(void* ptr)
	{
		if (!ptr || !blur_enabled)
		{
			return;
		}

		auto m = *reinterpret_cast<D3DXMATRIX *>(_nj_current_matrix_ptr_) * _view_matrix_inv;

		auto it = transform_map.find({ /*blur_i*/ 0, ptr });

		if (it != transform_map.end())
		{
			if (true || FrameCounter - it->second.time < age_threshold)
			{
				param::l_WorldMatrix      = it->second.world;
				param::l_ViewMatrix       = it->second.view;
				param::l_ProjectionMatrix = it->second.proj;

				it->second.time = static_cast<uint32_t>(FrameCounter);
				it->second.world = m;
				it->second.view = _view_matrix;
				it->second.proj = _proj_matrix;
			}
			else
			{
				param::l_WorldMatrix = m;
				param::l_ViewMatrix = _view_matrix;
				param::l_ProjectionMatrix = _proj_matrix;
				it = transform_map.erase(it);
				PrintDebug("discarding old transform!\n");
			}
		}
		else
		{
			transform_map[{ /*blur_i*/ 0, ptr }] = { static_cast<uint32_t>(FrameCounter), m, _view_matrix, _proj_matrix };
			param::l_WorldMatrix = m;
			param::l_ViewMatrix = _view_matrix;
			param::l_ProjectionMatrix = _proj_matrix;
			PrintDebug("storing new transform\n");
		}

		param::l_WorldMatrix.commit(d3d::device);
		param::l_ViewMatrix.commit(d3d::device);
		param::l_ProjectionMatrix.commit(d3d::device);
	}

	static void __cdecl njDrawModel_SADX_r(NJS_MODEL_SADX* a1)
	{
		begin();
		blur_begin();

		curr_model = a1;

		if (a1 && a1->nbMat && a1->mats)
		{
			globals::first_material = true;

			const auto _control_3d = _nj_control_3d_flag_;
			const auto _attr_or = _nj_constant_attr_or_;
			const auto _attr_and = _nj_constant_attr_and_;

			run_trampoline(TARGET_DYNAMIC(njDrawModel_SADX), a1);

			_nj_control_3d_flag_ = _control_3d;
			_nj_constant_attr_and_ = _attr_and;
			_nj_constant_attr_or_ = _attr_or;
		}
		else
		{
			run_trampoline(TARGET_DYNAMIC(njDrawModel_SADX), a1);
		}

		end();
	}

	static void __cdecl njDrawModel_SADX_Dynamic_r(NJS_MODEL_SADX* a1)
	{
		begin();
		blur_begin();

		curr_model = a1;

		if (a1 && a1->nbMat && a1->mats)
		{
			globals::first_material = true;

			const auto _control_3d = _nj_control_3d_flag_;
			const auto _attr_or = _nj_constant_attr_or_;
			const auto _attr_and = _nj_constant_attr_and_;

			run_trampoline(TARGET_DYNAMIC(njDrawModel_SADX_Dynamic), a1);

			_nj_control_3d_flag_ = _control_3d;
			_nj_constant_attr_and_ = _attr_and;
			_nj_constant_attr_or_ = _attr_or;
		}
		else
		{
			run_trampoline(TARGET_DYNAMIC(njDrawModel_SADX_Dynamic), a1);
		}

		end();
	}

	static void __fastcall PolyBuff_DrawTriangleStrip_r(PolyBuff* _this)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(PolyBuff_DrawTriangleStrip), _this);
		end();
	}

	static void __fastcall PolyBuff_DrawTriangleList_r(PolyBuff* _this)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(PolyBuff_DrawTriangleList), _this);
		end();
	}

	static void check_format()
	{
		const auto fmt = *reinterpret_cast<D3DFORMAT*>(reinterpret_cast<char*>(0x03D0FDC0) + 0x08);

		auto result = Direct3D_Object->CheckDeviceFormat(DisplayAdapter, D3DDEVTYPE_HAL, fmt,
			D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_X8R8G8B8);

		if (result == D3D_OK)
		{
			supports_xrgb = true;
			return;
		}

		result = Direct3D_Object->CheckDeviceFormat(DisplayAdapter, D3DDEVTYPE_HAL, fmt,
			D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F);

		if (result != D3D_OK)
		{
			MessageBoxA(WindowHandle, "Your GPU does not support any (reasonable) vertex texture sample formats.",
				"Insufficient GPU support", MB_OK | MB_ICONERROR);

			Exit();
		}
	}

	// ReSharper disable once CppDeclaratorNeverUsed
	static void __cdecl CreateDirect3DDevice_c(int behavior, int type)
	{
		if (Direct3D_Device == nullptr && Direct3D_Object != nullptr)
		{
			check_format();
		}

		auto orig = CreateDirect3DDevice_t->Target();
		auto _type = type;

		(void)orig;
		(void)_type;
		(void)behavior;

		__asm
		{
			push _type
			mov edx, behavior
			call orig
		}

		if (Direct3D_Device != nullptr && !initialized)
		{
			d3d::device = Direct3D_Device->GetProxyInterface();

			initialized = true;
			d3d::load_shader();
			hook_vtable();
		}
	}

	static void __declspec(naked) CreateDirect3DDevice_r()
	{
		__asm
		{
			push [esp + 04h] // type
			push edx // behavior

			call CreateDirect3DDevice_c

			pop edx // behavior
			add esp, 4
			retn 4
		}
	}

	static void __cdecl Direct3D_SetWorldTransform_r()
	{
		TARGET_DYNAMIC(Direct3D_SetWorldTransform)();

		store_transform(curr_model);

		if (false && !LanternInstance::use_palette())
		{
			return;
		}

		param::WorldMatrix = WorldMatrix;

		auto wvMatrix = D3DXMATRIX(WorldMatrix) * D3DXMATRIX(ViewMatrix);
		param::wvMatrix = wvMatrix;

		D3DXMatrixInverse(&wvMatrix, nullptr, &wvMatrix);
		D3DXMatrixTranspose(&wvMatrix, &wvMatrix);
		// The inverse transpose matrix is used for environment mapping.
		param::wvMatrixInvT = wvMatrix;
	}

	static void __stdcall Direct3D_SetProjectionMatrix_r(float hfov, float nearPlane, float farPlane)
	{
		TARGET_DYNAMIC(Direct3D_SetProjectionMatrix)(hfov, nearPlane, farPlane);

		// The view matrix can also be set here if necessary.
		_proj_matrix = D3DXMATRIX(ProjectionMatrix) * D3DXMATRIX(TransformationMatrix);
		_view_matrix = ViewMatrix;
		_view_matrix_inv = InverseViewMatrix;

		param::ProjectionMatrix = _proj_matrix;
	}

	static void __cdecl Direct3D_SetViewportAndTransform_r()
	{
		const auto original = TARGET_DYNAMIC(Direct3D_SetViewportAndTransform);
		bool invalid = TransformAndViewportInvalid != 0;
		original();

		if (invalid)
		{
			_proj_matrix = D3DXMATRIX(ProjectionMatrix) * D3DXMATRIX(TransformationMatrix);
			param::ProjectionMatrix = _proj_matrix;
		}
	}

	static void __cdecl Direct3D_PerformLighting_r(int type)
	{
		const auto target = TARGET_DYNAMIC(Direct3D_PerformLighting);

		if (!LanternInstance::use_palette())
		{
			globals::light_type = 0;
			target(type);
			return;
		}

		// This specifically forces light type 0 to prevent
		// the light direction from being overwritten.
		target(0);

		auto div2 = type / 2;

		if (div2 != CurrentLightType)
		{
			CurrentLightType = div2;

			// deliberately avoiding the call to SetCurrentLightType_Copy
			// to maintain onion-blur compatibility
			CurrentLightType_Copy = div2;
		}

		d3d::set_flags(ShaderFlags_Light, true);
		globals::palettes.set_palettes(type, 0);
		set_light_parameters();
	}


#define D3D_ORIG(NAME) \
	NAME ## _t

	template<typename T, typename... Args>
	static HRESULT run_d3d_trampoline(const T& original, Args... args)
	{
		using namespace d3d;

		shader_start();
		auto result = original(args...);

		if (SUCCEEDED(result) && blur_enabled)
		{
			STORE_RS(ZWRITEENABLE);
			STORE_RS(ZENABLE);

			if (!ZWRITEENABLE || !ZENABLE)
			{
				return result;
			}

			STORE_RS(ALPHABLENDENABLE);
			STORE_RS(ZFUNC);

			device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			device->SetRenderState(D3DRS_ZENABLE, TRUE);
			device->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
			device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

			Surface surface;
			velocity_buffer->GetSurfaceLevel(0, &surface);
			auto hr = device->SetRenderTarget(0, surface);

			VertexShader vs;
			device->GetVertexShader(&vs);

			PixelShader ps;
			device->GetPixelShader(&ps);

			device->SetVertexShader(velocity_vs);
			device->SetPixelShader(velocity_ps);

			for (auto& i : param::parameters)
			{
				i->commit_now(d3d::device);
			}

			result = original(args...);

			device->SetVertexShader(vs);
			device->SetPixelShader(ps);

			surface = nullptr;
			color_buffer->GetSurfaceLevel(0, &surface);
			device->SetRenderTarget(0, surface);

			RESTORE_RS(ALPHABLENDENABLE);
			RESTORE_RS(ZWRITEENABLE);
			RESTORE_RS(ZENABLE);
			RESTORE_RS(ZFUNC);
		}

		shader_end();
		return result;
	}

	HRESULT __stdcall SetTransform_r(IDirect3DDevice9* _this, D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix)
	{
		if (pMatrix)
		{
			switch (State)
			{
				default:
					break;

				case D3DTS_VIEW:
					param::ViewMatrix = *pMatrix;
					break;

				case D3DTS_PROJECTION:
					param::ProjectionMatrix = *pMatrix;
					break;

				case D3DTS_WORLD:
					param::WorldMatrix = *pMatrix;
					break;
			}
		}

		return run_d3d_trampoline(D3D_ORIG(SetTransform), _this, State, pMatrix);
	}

	static HRESULT __stdcall DrawPrimitive_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT StartVertex,
		UINT PrimitiveCount)
	{
		return run_d3d_trampoline(D3D_ORIG(DrawPrimitive), _this, PrimitiveType, StartVertex, PrimitiveCount);
	}
	static HRESULT __stdcall DrawIndexedPrimitive_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		INT BaseVertexIndex,
		UINT MinVertexIndex,
		UINT NumVertices,
		UINT startIndex,
		UINT primCount)
	{
		return run_d3d_trampoline(D3D_ORIG(DrawIndexedPrimitive), _this, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
	}
	static HRESULT __stdcall DrawPrimitiveUP_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT PrimitiveCount,
		CONST void* pVertexStreamZeroData,
		UINT VertexStreamZeroStride)
	{
		return run_d3d_trampoline(D3D_ORIG(DrawPrimitiveUP), _this, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
	}
	static HRESULT __stdcall DrawIndexedPrimitiveUP_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT MinVertexIndex,
		UINT NumVertices,
		UINT PrimitiveCount,
		CONST void* pIndexData,
		D3DFORMAT IndexDataFormat,
		CONST void* pVertexStreamZeroData,
		UINT VertexStreamZeroStride)
	{
		return run_d3d_trampoline(D3D_ORIG(DrawIndexedPrimitiveUP),
		                          _this, PrimitiveType, MinVertexIndex,
		                          NumVertices, PrimitiveCount, pIndexData,
		                          IndexDataFormat, pVertexStreamZeroData,
		                          VertexStreamZeroStride);
	}

	// ReSharper disable once CppDeclaratorNeverUsed
	static void __stdcall DrawMeshSetBuffer_c(MeshSetBuffer* buffer)
	{
		if (!buffer->FVF)
		{
			return;
		}

		Direct3D_Device->SetVertexShader(buffer->FVF);
		Direct3D_Device->SetStreamSource(0, buffer->VertexBuffer, buffer->Size);

		const auto index_buffer = buffer->IndexBuffer;
		if (index_buffer)
		{
			Direct3D_Device->SetIndices(index_buffer, 0);

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

	// ReSharper disable once CppDeclaratorNeverUsed
	static const auto loc_77EF09 = reinterpret_cast<void*>(0x0077EF09);
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
		param::ProjectionMatrix = *matrix;
		return _device->SetTransform(type, matrix);
	}

	static bool show_velocity = false;

	static void __cdecl Direct3D_Present_r();
	static Trampoline Direct3D_Present_t(0x0078BA30, 0x0078BA35, Direct3D_Present_r);
	static void __cdecl Direct3D_Present_r()
	{
		using namespace d3d;

		auto original = reinterpret_cast<decltype(Direct3D_Present_r)*>(Direct3D_Present_t.Target());

		device->SetRenderTarget(0, og_render_target);

		if (ControllerPointers[0] && ControllerPointers[0]->PressedButtons & Buttons_Up)
		{
			show_velocity = !show_velocity;
		}

		if (show_velocity)
		{
			device->SetTexture(1, velocity_buffer);
		}
		else
		{
			device->SetTexture(1, color_buffer);
			device->SetTexture(2, velocity_buffer);
		}

		device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

		device->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		device->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		device->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

		VertexShader vs;
		device->GetVertexShader(&vs);

		PixelShader ps;
		device->GetPixelShader(&ps);

		device->SetVertexShader(quad_vs);

		if (show_velocity)
		{
			device->SetPixelShader(quad_ps);
		}
		else
		{
			device->SetPixelShader(blur_ps);
		}

		STORE_RS(ALPHABLENDENABLE);
		STORE_RS(SRCBLEND);
		STORE_RS(DESTBLEND);
		STORE_RS(ZENABLE);
		STORE_RS(ZWRITEENABLE);

		device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
		device->SetRenderState(D3DRS_ZENABLE, FALSE);
		device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

		draw_quad();
		original();

		RESTORE_RS(ALPHABLENDENABLE);
		RESTORE_RS(SRCBLEND);
		RESTORE_RS(DESTBLEND);
		RESTORE_RS(ZENABLE);
		RESTORE_RS(ZWRITEENABLE);

		device->SetTexture(1, nullptr);
		device->SetTexture(2, nullptr);

		Surface surface;
		velocity_buffer->GetSurfaceLevel(0, &surface);
		device->SetRenderTarget(0, surface);
		device->Clear(1, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 127, 127, 127), 0.0f, 0);

		surface = nullptr;
		color_buffer->GetSurfaceLevel(0, &surface);
		device->SetRenderTarget(0, surface);
		device->Clear(1, nullptr, D3DCLEAR_TARGET, 0, 0.0f, 0);

		device->SetVertexShader(vs);
		device->SetPixelShader(ps);
	}
#pragma endregion

	static void __cdecl Sonic_Display_r(ObjectMaster* o);
	static Trampoline Sonic_Display_t(0x004948C0, 0x004948C7, Sonic_Display_r);
	static void __cdecl Sonic_Display_r(ObjectMaster* o)
	{
		auto original = reinterpret_cast<decltype(Sonic_Display_r)*>(Sonic_Display_t.Target());

		blur_enabled = true;
		blur_begin();

		original(o);

		blur_end();
		blur_enabled = false;
	}

	static void __cdecl DrawModelThing_r(NJS_MODEL_SADX* a1);
	static Trampoline DrawModelThing_t(0x00403470, 0x00403479, DrawModelThing_r);
	static void __cdecl DrawModelThing_r(NJS_MODEL_SADX* a1)
	{
		auto original = reinterpret_cast<decltype(DrawModelThing_r)*>(DrawModelThing_t.Target());

		//blur_enabled = true;
		blur_begin();

		curr_model = a1;
		original(a1);

		blur_end();
		//blur_enabled = false;
	}
}

namespace d3d
{
	IDirect3DDevice9* device = nullptr;
	VertexShader vertex_shader;
	PixelShader pixel_shader;
	bool do_effect = false;

	float alpha_ref_value = 16.0f / 255.0f;
	bool alpha_ref_temp = false;

	bool supports_xrgb()
	{
		return local::supports_xrgb;
	}

	void reset_overrides()
	{
		if (LanternInstance::diffuse_override_temp)
		{
			LanternInstance::diffuse_override = false;
			param::DiffuseOverride = false;
		}

		if (LanternInstance::specular_override_temp)
		{
			LanternInstance::specular_override = false;
		}

		if (alpha_ref_temp)
		{
			param::AlphaRef = alpha_ref_value;
			alpha_ref_temp = false;
		}

		param::ForceDefaultDiffuse = false;
	}

	void load_shader()
	{
		if (!local::initialized)
		{
			return;
		}

		local::create_render_targets();
		local::clear_shaders();
		local::create_shaders();
	}

	void set_flags(Uint32 flags, bool add)
	{
		if (add)
		{
			local::shader_flags |= flags;
		}
		else
		{
			local::shader_flags &= ~flags;
		}
	}

	bool shaders_null()
	{
		return vertex_shader == nullptr || pixel_shader == nullptr;
	}

	void init_trampolines()
	{
		using namespace local;

		Direct3D_PerformLighting_t         = new Trampoline(0x00412420, 0x00412426, Direct3D_PerformLighting_r);
		sub_77EAD0_t                       = new Trampoline(0x0077EAD0, 0x0077EAD7, sub_77EAD0_r);
		sub_77EBA0_t                       = new Trampoline(0x0077EBA0, 0x0077EBA5, sub_77EBA0_r);
		njDrawModel_SADX_t                 = new Trampoline(0x0077EDA0, 0x0077EDAA, njDrawModel_SADX_r);
		njDrawModel_SADX_Dynamic_t         = new Trampoline(0x00784AE0, 0x00784AE5, njDrawModel_SADX_Dynamic_r);
		Direct3D_SetProjectionMatrix_t     = new Trampoline(0x00791170, 0x00791175, Direct3D_SetProjectionMatrix_r);
		Direct3D_SetViewportAndTransform_t = new Trampoline(0x007912E0, 0x007912E8, Direct3D_SetViewportAndTransform_r);
		Direct3D_SetWorldTransform_t       = new Trampoline(0x00791AB0, 0x00791AB5, Direct3D_SetWorldTransform_r);
		CreateDirect3DDevice_t             = new Trampoline(0x00794000, 0x00794007, CreateDirect3DDevice_r);
		PolyBuff_DrawTriangleStrip_t       = new Trampoline(0x00794760, 0x00794767, PolyBuff_DrawTriangleStrip_r);
		PolyBuff_DrawTriangleList_t        = new Trampoline(0x007947B0, 0x007947B7, PolyBuff_DrawTriangleList_r);

		WriteJump(reinterpret_cast<void*>(0x0077EE45), DrawMeshSetBuffer_asm);

		// Hijacking a IDirect3DDevice8::SetTransform call in Direct3D_SetNearFarPlanes
		// to update the projection matrix.
		// This nops:
		// mov ecx, [eax] (device)
		// call dword ptr [ecx+94h] (device->SetTransform)
		WriteData<8>(reinterpret_cast<void*>(0x00403234), 0x90i8);
		WriteCall(reinterpret_cast<void*>(0x00403236), SetTransformHijack);
	}
}

extern "C"
{
	using namespace local;

	EXPORT void __cdecl OnRenderDeviceLost()
	{
		end();
		free_render_targets();
		free_shaders();
	}

	EXPORT void __cdecl OnRenderDeviceReset()
	{
		create_render_targets();
		create_shaders();
	}

	EXPORT void __cdecl OnExit()
	{
		param::release_parameters();
		free_render_targets();
		free_shaders();
	}
}
