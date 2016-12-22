#include "stdafx.h"
#include <SADXModLoader.h>
#include "FixCharacterMaterials.h"

static HMODULE chrmodels_handle = nullptr;

inline int get_handle()
{
	if (chrmodels_handle != nullptr)
	{
		return (int)chrmodels_handle;
	}

	chrmodels_handle = GetModuleHandle(L"CHRMODELS_orig");
	if (chrmodels_handle == nullptr)
	{
		throw;
	}

	return (int)chrmodels_handle;
}

static std::vector<NJS_MATERIAL*> materials;

#ifdef _DEBUG
template<typename T = Uint32, size_t N>
static void models(NJS_MODEL_SADX* model, const T(&ids)[N])
{
	if (!model)
	{
		return;
	}

	if (model->nbMat != N)
	{
		return;
	}

	const auto& mats = model->mats;

	if (!mats)
	{
		return;
	}

	auto it = std::find(materials.begin(), materials.end(), mats);

	if (it != materials.end())
	{
		return;
	}

	for (int i = 0; i < N; i++)
	{
		if (mats[i].attr_texId != ids[i])
			return;
	}

	materials.push_back(mats);
	PrintDebug("HIT: 0x%08X\n", (int)mats - (int)chrmodels_handle);
}
template<typename T = Uint32, size_t N>
static void models(const std::string& id, int length, const T(&ids)[N])
{
	auto handle = (NJS_MODEL_SADX**)GetProcAddress(chrmodels_handle, ("___" + id + "_MODELS").c_str());
	if (!handle)
	{
		return;
	}

	for (int i = 0; i < length; i++)
	{
		models(handle[i], ids);
	}
}

template<typename T = Uint32, size_t N>
static void objects(NJS_OBJECT* object, const T(&ids)[N])
{
	if (!object)
	{
		return;
	}

	auto model = object->getbasicdxmodel();

	if (model)
	{
		models(model, ids);
	}

	if (object->child)
	{
		objects(object->child, ids);
	}

	if (object->sibling)
	{
		objects(object->sibling, ids);
	}
}
template<typename T = Uint32, size_t N>
static void objects(const std::string& id, int length, const T(&ids)[N])
{
	auto handle = (NJS_OBJECT**)GetProcAddress(chrmodels_handle, ("___" + id + "_OBJECTS").c_str());
	if (!handle)
	{
		return;
	}

	for (int i = 0; i < length; i++)
	{
		objects(handle[i], ids);
	}
}

template<typename T = Uint32, size_t N>
static void actions(NJS_ACTION* action, const T(&ids)[N])
{
	if (!action)
	{
		return;
	}

	auto object = action->object;
	objects(object, ids);
}
template<typename T = Uint32, size_t N>
static void actions(const std::string& id, int length, const T(&ids)[N])
{
	auto handle = (NJS_ACTION**)GetProcAddress(chrmodels_handle, ("___" + id + "_ACTIONS").c_str());
	if (!handle)
	{
		return;
	}

	for (int i = 0; i < length; i++)
	{
		actions(handle[i], ids);
	}
}
#endif

void FixCharacterMaterials()
{
	auto handle = get_handle();

#if _DEBUG
	int ids[] = { 0, 11 };
	actions("MILES", 114, ids);
	objects("MILES", 72, ids);
	models ("MILES", 15, ids);
	materials.clear();
#endif
	
	// Sonic's nose:
	DataArray_(NJS_MATERIAL, matlist_00565C68, (0x00565C68 + handle), 3);
	DataArray_(NJS_MATERIAL, matlist_0057636C, (0x0057636C + handle), 3);
	DataArray_(NJS_MATERIAL, matlist_0057D7BC, (0x0057D7BC + handle), 3);

	matlist_00565C68[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0057636C[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0057D7BC[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Super Sonic's jump ball:
	DataArray_(NJS_MATERIAL, matlist_0062DEBC, (0x0062DEBC + handle), 1);

	matlist_0062DEBC[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	
	// Tails' shoes (additional) (8 duplicates? Are you kidding me?):
	DataArray_(NJS_MATERIAL, matlist_00420290, (0x00420290 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_004208B8, (0x004208B8 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_004219F0, (0x004219F0 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_00422018, (0x00422018 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_0042D180, (0x0042D180 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_0042D7A8, (0x0042D7A8 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_0042E8E0, (0x0042E8E0 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_0042EF08, (0x0042EF08 + handle), 2);

	matlist_00420290[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_00420290[1].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_004208B8[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_004208B8[1].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_004219F0[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_004219F0[1].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_00422018[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_00422018[1].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_0042D180[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_0042D180[1].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_0042D7A8[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_0042D7A8[1].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_0042E8E0[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_0042E8E0[1].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_0042EF08[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_0042EF08[1].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;

	// Tails' eye whites ([1]) and nose ([2]):
	DataArray_(NJS_MATERIAL, matlist_00426F04, (0x00426F04 + handle), 3);
	DataArray_(NJS_MATERIAL, matlist_00433DF4, (0x00433DF4 + handle), 3);
	DataArray_(NJS_MATERIAL, matlist_004414C0, (0x004414C0 + handle), 3);

	matlist_00426F04[1].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00433DF4[1].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_004414C0[1].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00426F04[2].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00433DF4[2].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_004414C0[2].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Tails' eye pupils
	DataArray_(NJS_MATERIAL, matlist_00426884, (0x00426884 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_00426BC4, (0x00426BC4 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_00433774, (0x00433774 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_00433AB4, (0x00433AB4 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_0043CC48, (0x0043CC48 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_0043CDD0, (0x0043CDD0 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_00447098, (0x00447098 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_004473D4, (0x004473D4 + handle), 1);

	matlist_00426884[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00426BC4[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00433774[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00433AB4[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0043CC48[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0043CDD0[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00447098[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_004473D4[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Knuckles' eye whites:
	DataArray_(NJS_MATERIAL, matlist_002DD1E4, (0x002DD1E4 + handle), 3);
	DataArray_(NJS_MATERIAL, matlist_002E9C64, (0x002E9C64 + handle), 3);
	DataArray_(NJS_MATERIAL, matlist_00327E7C, (0x00327E7C + handle), 3);

	matlist_002DD1E4[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_002E9C64[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;
	matlist_00327E7C[0].attrflags &= ~NJD_FLAG_IGNORE_SPECULAR;

	// Knuckles' nose:
	DataArray_(NJS_MATERIAL, matlist_002DD8FC, (0x002DD8FC + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_002EA37C, (0x002EA37C + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_002F8564, (0x002F8564 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_002FE25C, (0x002FE25C + handle), 2);

	matlist_002DD8FC[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_002EA37C[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_002F8564[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_002FE25C[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Amy's nose:
	DataArray_(NJS_MATERIAL, matlist_00012358, (0x00012358 + handle), 3);
	DataArray_(NJS_MATERIAL, matlist_00018ABC, (0x00018ABC + handle), 3);

	matlist_00012358[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00018ABC[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Amy's eye pupils:
	DataArray_(NJS_MATERIAL, matlist_000113FC, (0x000113FC + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_000116F0, (0x000116F0 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_00015A00, (0x00015A00 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_00015DF8, (0x00015DF8 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_0001DC00, (0x0001DC00 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_0001DEF0, (0x0001DEF0 + handle), 1);

	matlist_000113FC[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_000116F0[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00015A00[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00015DF8[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0001DC00[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0001DEF0[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Amy's headband:
	DataArray_(NJS_MATERIAL, matlist_00012048, (0x00012048 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_0001E848, (0x0001E848 + handle), 1);

	matlist_00012048[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0001E848[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Amy's bracelets
	DataArray_(NJS_MATERIAL, matlist_0000D030, (0x0000D030 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_00010B2C, (0x00010B2C + handle), 2);

	matlist_0000D030[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0000D030[1].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00010B2C[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00010B2C[1].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Big's nose:
	DataArray_(NJS_MATERIAL, matlist_0011BB58, (0x0011BB58 + handle), 7);
	DataArray_(NJS_MATERIAL, matlist_00125450, (0x00125450 + handle), 7);

	matlist_0011BB58[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00125450[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Big's eyes
	DataArray_(NJS_MATERIAL, matlist_0011B788, (0x0011B788 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_0011B978, (0x0011B978 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_00129208, (0x00129208 + handle), 1);
	DataArray_(NJS_MATERIAL, matlist_001293F8, (0x001293F8 + handle), 1);

	matlist_0011B788[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0011B978[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_00129208[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_001293F8[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	// Big's fishing rod (without upgrades)
	DataArray_(NJS_MATERIAL, matlist_0011E8E0, (0x0011E8E0 + handle), 3);
	DataArray_(NJS_MATERIAL, matlist_0011E718, (0x0011E718 + handle), 2);
	DataArray_(NJS_MATERIAL, matlist_001284F0, (0x001284F0 + handle), 2);

	matlist_0011E8E0[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0011E8E0[1].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0011E8E0[2].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

	matlist_0011E718[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_0011E718[1].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_001284F0[0].attrflags |= NJD_FLAG_IGNORE_SPECULAR;
	matlist_001284F0[1].attrflags |= NJD_FLAG_IGNORE_SPECULAR;

#pragma region help me
	DataPointer_(NJS_MATERIAL, mat_00011A00, (0x00011A00 + handle));
	DataPointer_(NJS_MATERIAL, mat_00015A00, (0x00015A00 + handle));
	DataPointer_(NJS_MATERIAL, mat_00015DF8, (0x00015DF8 + handle));
	DataPointer_(NJS_MATERIAL, mat_0011A080, (0x0011A080 + handle));
	DataPointer_(NJS_MATERIAL, mat_00127554, (0x00127554 + handle));
	DataPointer_(NJS_MATERIAL, mat_001276B4, (0x001276B4 + handle));
	DataPointer_(NJS_MATERIAL, mat_001286B8, (0x001286B8 + handle));
	DataPointer_(NJS_MATERIAL, mat_001286CC, (0x001286CC + handle));
	DataPointer_(NJS_MATERIAL, mat_001286E0, (0x001286E0 + handle));
	DataPointer_(NJS_MATERIAL, mat_001FDBB4, (0x001FDBB4 + handle));
	DataPointer_(NJS_MATERIAL, mat_001FDBC8, (0x001FDBC8 + handle));
	DataPointer_(NJS_MATERIAL, mat_00200540, (0x00200540 + handle));
	DataPointer_(NJS_MATERIAL, mat_00200554, (0x00200554 + handle));
	DataPointer_(NJS_MATERIAL, mat_002009E8, (0x002009E8 + handle));
	DataPointer_(NJS_MATERIAL, mat_00200DD4, (0x00200DD4 + handle));
	DataPointer_(NJS_MATERIAL, mat_00204EF0, (0x00204EF0 + handle));
	DataPointer_(NJS_MATERIAL, mat_002054FC, (0x002054FC + handle));
	DataPointer_(NJS_MATERIAL, mat_00205510, (0x00205510 + handle));
	DataPointer_(NJS_MATERIAL, mat_00206314, (0x00206314 + handle));
	DataPointer_(NJS_MATERIAL, mat_00206924, (0x00206924 + handle));
	DataPointer_(NJS_MATERIAL, mat_00206938, (0x00206938 + handle));
	DataPointer_(NJS_MATERIAL, mat_0020B9E0, (0x0020B9E0 + handle));
	DataPointer_(NJS_MATERIAL, mat_002D71A8, (0x002D71A8 + handle));
	DataPointer_(NJS_MATERIAL, mat_002D7934, (0x002D7934 + handle));
	DataPointer_(NJS_MATERIAL, mat_002D8930, (0x002D8930 + handle));
	DataPointer_(NJS_MATERIAL, mat_002D90BC, (0x002D90BC + handle));
	DataPointer_(NJS_MATERIAL, mat_002E3C28, (0x002E3C28 + handle));
	DataPointer_(NJS_MATERIAL, mat_002E43B4, (0x002E43B4 + handle));
	DataPointer_(NJS_MATERIAL, mat_002E53B0, (0x002E53B0 + handle));
	DataPointer_(NJS_MATERIAL, mat_002E5B3C, (0x002E5B3C + handle));
	DataPointer_(NJS_MATERIAL, mat_0046E05C, (0x0046E05C + handle));
	DataPointer_(NJS_MATERIAL, mat_0046E670, (0x0046E670 + handle));
	DataPointer_(NJS_MATERIAL, mat_0046E684, (0x0046E684 + handle));
	DataPointer_(NJS_MATERIAL, mat_0046E698, (0x0046E698 + handle));
	DataPointer_(NJS_MATERIAL, mat_0046E6AC, (0x0046E6AC + handle));
	DataPointer_(NJS_MATERIAL, mat_0046EE8C, (0x0046EE8C + handle));
	DataPointer_(NJS_MATERIAL, mat_0046F4A0, (0x0046F4A0 + handle));
	DataPointer_(NJS_MATERIAL, mat_0046F4B4, (0x0046F4B4 + handle));
	DataPointer_(NJS_MATERIAL, mat_0046F4C8, (0x0046F4C8 + handle));
	DataPointer_(NJS_MATERIAL, mat_0046F4DC, (0x0046F4DC + handle));
	DataPointer_(NJS_MATERIAL, mat_00579C94, (0x00579C94 + handle));
	DataPointer_(NJS_MATERIAL, mat_00582D08, (0x00582D08 + handle));
#pragma endregion

	mat_00011A00.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00012014 Amy's eye (white)
	mat_00015A00.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00015D28 Amy's eye (pupil)
	mat_00015DF8.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00016120 Amy's eye (pupil)

	mat_0011A080.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_0011A158 Big's belt buckle
	mat_00127554.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_0012766C Big's life belt buckle
	mat_001276B4.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_001284BC Big's life belt [1]
	mat_001286B8.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00128A90 Big's fishing rod (w/ upgrades) [0]
	mat_001286CC.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00128A90 Big's fishing rod (w/ upgrades) [1]
	mat_001286E0.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00128A90 Big's fishing rod (w/ upgrades) [2]

	mat_001FDBB4.attrflags |=  NJD_FLAG_IGNORE_LIGHT;    // object_001FDF7C Gamma's light 1FDBA0[1]
	mat_001FDBC8.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_001FDF7C Gamma's light 1FDBA0[2]
	mat_00200540.attrflags |=  NJD_FLAG_IGNORE_LIGHT;    // object_0020098C Gamma's right eye 20052C[1]
	mat_00200554.attrflags |=  NJD_FLAG_IGNORE_LIGHT;    // object_0020098C Gamma's uh... arrow things in between his eyes 20052C[2]
	mat_002009E8.attrflags |=  NJD_FLAG_IGNORE_LIGHT;    // object_00200D14 Gamma's left eye 2009E8[2]
	mat_00200DD4.attrflags |=  NJD_FLAG_IGNORE_LIGHT;    // object_00201AE4 Gamma's head (blue line) [7]
	mat_00204EF0.attrflags |=  NJD_FLAG_IGNORE_LIGHT;    // object_0020524C Gamma's left foot [2]
	mat_002054FC.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00205794 Gamma's left tire [0]
	mat_00205510.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00205794 Gamma's left tire [1]
	mat_00206314.attrflags |=  NJD_FLAG_IGNORE_LIGHT;    // object_00206674 Gamma's right foot [3]
	mat_00206924.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00206BBC Gamma's right tire [0]
	mat_00206938.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_00206BBC Gamma's right tire [1]
	mat_0020B9E0.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_0020BCAC gamma something idk [2]

	mat_002D71A8.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_002D7900 Knuckles' left foot
	mat_002D7934.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_002D7FDC Knuckles' left heel
	mat_002D8930.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_002D9088 Knuckles' right foot
	mat_002D90BC.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_002D9754 Knuckles' right heel
	mat_002E3C28.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_002E4380 Knuckles' left shoe (gliding)
	mat_002E43B4.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_002E4A5C Knuckles' left heel (gliding)
	mat_002E53B0.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_002E5B08 Knuckles' right foot (gliding)
	mat_002E5B3C.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_002E61D4 Knuckles' right heel (gliding)

	mat_0046E05C.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046E63C Tails' right foot (flying) [1]
	mat_0046E670.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046EE44 Tails' left heel (jet anklet) (flying) [0] / 3
	mat_0046E684.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046EE44 Tails' left heel (jet anklet) (flying) [1] / 3
	mat_0046E698.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046EE44 Tails' left heel (jet anklet) (flying) [2] / 3
	mat_0046E6AC.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046EE44 Tails' left heel (jet anklet) (flying) [3] / 3
	mat_0046EE8C.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046F46C Tails' right foot (jet anklet) (flying) [1]
	mat_0046F4A0.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046FC84 Tails' right heel (jet anklet) (flying) [0] / 3
	mat_0046F4B4.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046FC84 Tails' right heel (jet anklet) (flying) [1] / 3
	mat_0046F4C8.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046FC84 Tails' right heel (jet anklet) (flying) [2] / 3
	mat_0046F4DC.attrflags &= ~NJD_FLAG_IGNORE_SPECULAR; // object_0046FC84 Tails' right heel (jet anklet) (flying) [3] / 3

	mat_00579C94.attrflags |=  NJD_FLAG_IGNORE_SPECULAR; // object_0057BC44 Sonic's jump ball
	mat_00582D08.attrflags |=  NJD_FLAG_IGNORE_LIGHT;    // object_00583284 Sonic's Crystal Ring [1]
}
