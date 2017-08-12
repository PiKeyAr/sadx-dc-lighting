struct VS_IN
{
	float3 position : POSITION;
	float3 normal   : NORMAL;
	float2 tex      : TEXCOORD0;
	float4 color    : COLOR0;
};

struct PS_IN
{
	float4 position : POSITION0;
	float4 diffuse  : COLOR0;
	float4 specular : COLOR1;
	float2 tex      : TEXCOORD0;
	float2 depth    : TEXCOORD1;
	float  fogDist  : FOG;
};

#ifdef USE_SL
struct SourceLight_t
{
	int y, z;
	float3 color;
	float specular;
	float diffuse;
	float ambient;
	float unknown2[15];
};

struct StageLight
{
	float3 direction;
	float specular;
	float multiplier;
	float3 diffuse;
	float3 ambient;
	float padding[5];
};
#endif

// From FixedFuncEMU.fx
// Copyright (c) 2005 Microsoft Corporation. All rights reserved.
#define FOGMODE_NONE   0
#define FOGMODE_EXP    1
#define FOGMODE_EXP2   2
#define FOGMODE_LINEAR 3
#define E 2.71828

#define D3DMCS_MATERIAL 0 // Color from material is used
#define D3DMCS_COLOR1   1 // Diffuse vertex color is used
#define D3DMCS_COLOR2   2 // Specular vertex color is used

// This never changes
static float AlphaRef = 16.0f / 255.0f;

// Diffuse texture
shared Texture2D BaseTexture;
// Palette atlas A
shared Texture2D PaletteA;
// Palette atlas B
shared Texture2D PaletteB;

shared float4x4 WorldMatrix;
shared float4x4 wvMatrix;
shared float4x4 ProjectionMatrix;
// The inverse transpose of the world view matrix - used for environment mapping.
shared float4x4 wvMatrixInvT;

// Used primarily for environment mapping.
shared float4x4 TextureTransform = {
	-0.5, 0.0, 0.0, 0.0,
	 0.0, 0.5, 0.0, 0.0,
	 0.0, 0.0, 1.0, 0.0,
	 0.5, 0.5, 0.0, 1.0
};

shared uint DiffuseSource = (uint)D3DMCS_COLOR1;
shared float4 MaterialDiffuse = float4(1.0f, 1.0f, 1.0f, 1.0f);

// Pre-adjusted on the CPU before being sent to the shader.
// Used for sampling colors from the palette atlases.
shared float DiffuseIndexA  = 0;
shared float DiffuseIndexB  = 0;
shared float SpecularIndexA = 0;
shared float SpecularIndexB = 0;

shared float3 LightDirection = float3(0.0f, -1.0f, 0.0f);
shared float3 NormalScale = float3(1, 1, 1);

shared uint   FogMode = (uint)FOGMODE_NONE;
shared float  FogStart;
shared float  FogEnd;
shared float  FogDensity;
shared float4 FogColor;

shared float DiffuseBlendFactor = 0.0f;
shared float SpecularBlendFactor = 0.0f;

shared bool AllowVertexColor       = true;
shared bool ForceDefaultDiffuse    = false;
shared bool DiffuseOverride        = false;
shared float3 DiffuseOverrideColor = float3(1, 1, 1);

#ifdef USE_SL
shared float4 MaterialSpecular = float4(0.0f, 0.0f, 0.0f, 0.0f);
shared float  MaterialPower = 1.0f;
shared SourceLight_t SourceLight;
shared StageLight Lights[4];
#endif

// Samplers
sampler2D baseSampler = sampler_state
{
	Texture = <BaseTexture>;
};

sampler2D atlasSamplerA = sampler_state
{
	Texture = <PaletteA>;
	MinFilter = Point;
	MagFilter = Point;
	AddressU = Clamp;
	AddressV = Clamp;
};

sampler2D atlasSamplerB = sampler_state
{
	Texture = <PaletteB>;
	MinFilter = Point;
	MagFilter = Point;
	AddressU = Clamp;
	AddressV = Clamp;
};

shared Texture2D OpaqueDepth;
shared Texture2D AlphaDepth;

sampler opaqueDepthSampler = sampler_state
{
	Texture = <OpaqueDepth>;
	MinFilter = Point;
	MagFilter = Point;
	AddressU = Clamp;
	AddressV = Clamp;
};
sampler alphaDepthSampler = sampler_state
{
	Texture = <AlphaDepth>;
	MinFilter = Point;
	MagFilter = Point;
	AddressU = Clamp;
	AddressV = Clamp;
};

// Used for correcting screen-space coordinates to sample the depth buffer.
shared float2 ViewPort;
shared uint SourceBlend, DestinationBlend;
shared float DrawDistance;
shared float DepthOverride;

// Helpers

#ifdef USE_FOG
// From FixedFuncEMU.fx
// Copyright (c) 2005 Microsoft Corporation. All rights reserved.

float CalcFogFactor(float d)
{
	float fogCoeff;

	switch (FogMode)
	{
		default:
			break;

		case FOGMODE_EXP:
			fogCoeff = 1.0 / pow(E, d * FogDensity);
			break;

		case FOGMODE_EXP2:
			fogCoeff = 1.0 / pow(E, d * d * FogDensity * FogDensity);
			break;

		case FOGMODE_LINEAR:
			fogCoeff = (FogEnd - d) / (FogEnd - FogStart);
			break;
	}

	return clamp(fogCoeff, 0, 1);
}
#endif

float4 GetDiffuse(in float4 vcolor)
{
	if (DiffuseSource == D3DMCS_COLOR1 && !AllowVertexColor)
	{
		return float4(1, 1, 1, vcolor.a);
	}

	if (DiffuseSource == D3DMCS_MATERIAL && ForceDefaultDiffuse)
	{
		return float4(178.0 / 255.0, 178.0 / 255.0, 178.0 / 255.0, MaterialDiffuse.a);
	}

	float4 color = (DiffuseSource == D3DMCS_COLOR1 && any(vcolor)) ? vcolor : MaterialDiffuse;

	int3 icolor = color.rgb * 255.0;
	if (icolor.r == 178 && icolor.g == 178 && icolor.b == 178)
	{
		return float4(1, 1, 1, color.a);
	}

	return color;
}

// Vertex shaders

PS_IN vs_main(VS_IN input)
{
	PS_IN output;

	float4 position;

	position = mul(float4(input.position, 1), wvMatrix);
	output.fogDist = position.z;
	position = mul(position, ProjectionMatrix);

	output.depth = position.zw;
	output.position = position;

#if defined(USE_TEXTURE) && defined(USE_ENVMAP)
	output.tex = (float2)mul(float4(input.normal, 1), wvMatrixInvT);
	output.tex = (float2)mul(float4(output.tex, 0, 1), TextureTransform);
#else
	output.tex = input.tex;
#endif

#if defined(USE_LIGHT)
	{
		float3 worldNormal = mul(input.normal * NormalScale, (float3x3)WorldMatrix);
		float4 diffuse = GetDiffuse(input.color);

		// This is the "brightness index" calculation. Just a dot product
		// of the vertex normal (in world space) and the light direction.
		float _dot = dot(LightDirection, worldNormal);

		// The palette's brightest point is 0, and its darkest point is 1,
		// so we push the dot product (-1 .. 1) into the rage 0 .. 1, and
		// subtract it from 1. This is the value we use for indexing into
		// the palette.
		// HACK: This clamp prevents a visual bug in the Mystic Ruins past (shrine on fire)
		float i = floor(clamp(1 - (_dot + 1) / 2, 0, 0.99) * 255) / 255;

		float4 pdiffuse;

		if (DiffuseOverride)
		{
			pdiffuse = float4(DiffuseOverrideColor, 1);
		}
		else
		{
			pdiffuse = tex2Dlod(atlasSamplerA, float4(i, DiffuseIndexA, 0, 0));
		}

		float4 pspecular = tex2Dlod(atlasSamplerA, float4(i, SpecularIndexA, 0, 0));

		#ifdef USE_BLEND
			float4 bdiffuse = tex2Dlod(atlasSamplerB, float4(i, DiffuseIndexB, 0, 0));
			float4 bspecular = tex2Dlod(atlasSamplerB, float4(i, SpecularIndexB, 0, 0));

			pdiffuse = lerp(pdiffuse, bdiffuse, DiffuseBlendFactor);
			pspecular = lerp(pspecular, bspecular, SpecularBlendFactor);
		#endif

		output.diffuse = float4((diffuse * pdiffuse).rgb, diffuse.a);
		output.specular = float4(pspecular.rgb, 0.0f);
	}
#else
	{
		// Just spit out the vertex or material color if lighting is off.
		output.diffuse = GetDiffuse(input.color);
		output.specular = 0;
	}
#endif

	return output;
}

uniform float alpha_bias = 1 / 255;

#ifdef USE_OIT
float4 ps_main(PS_IN input, out float oDepth : DEPTH0, float2 vpos : VPOS, out float4 blend : COLOR1) : COLOR0
#else
float4 ps_main(PS_IN input, out float oDepth : DEPTH0, float2 vpos : VPOS) : COLOR0
#endif
{
	float4 result;

	const float C = 1.0;
	const float offset = 1;

	// Logarithmic depth
	float currentDepth = log(C * (input.depth.x + DepthOverride) + offset) / log(C * DrawDistance + offset);

	oDepth = currentDepth;

#ifdef USE_OIT
	float2 depthcoord = vpos / ViewPort;

	// Exclude any fragment whose depth exceeds that of any opaque fragment.
	// (equivalent to D3DCMP_LESS)
	float baseDepth = tex2D(opaqueDepthSampler, depthcoord).r;
	if (currentDepth >= baseDepth)
	{
		discard;
	}

	// Discard any fragment whose depth is less than the last fragment depth.
	// (equivalent to D3DCMP_GREATER)
	float lastDepth = tex2D(alphaDepthSampler, depthcoord).r;
	if (currentDepth <= lastDepth)
	{
		discard;
	}

	blend = float4((float)SourceBlend / 15.0f, (float)DestinationBlend / 15.0f, 0, 1);
#endif

#ifdef USE_TEXTURE
	result = tex2D(baseSampler, input.tex);
	result = result * input.diffuse + input.specular;
#else
	result = input.diffuse;
#endif

#ifdef USE_ALPHA
	clip(result.a < AlphaRef ? -1 : 1);
#endif

#ifdef USE_FOG
	float factor = CalcFogFactor(input.fogDist);
	result.rgb = (factor * result + (1.0 - factor) * FogColor).rgb;
#endif

	return result;
}

technique Main
{
	pass p0
	{
#ifdef USE_OIT
		ZEnable = true;
		ZWriteEnable = true;
#endif

		VertexShader = compile vs_3_0 vs_main();
		PixelShader = compile ps_3_0 ps_main();
	}
}
