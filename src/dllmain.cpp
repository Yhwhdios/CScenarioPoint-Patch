#include <Windows.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include "Hooking.Patterns.h"
#include "Hooking.h"
#include "CScenarioInfo.h"
#include "CScenarioPoint.h"
#include "CScenarioPointRegion.h"
#include <unordered_map>
#include <MinHook.h>
#include <jitasm.h>

constexpr bool EnableLogging = true;

static void WaitForWindow()
{
	spdlog::info("Waiting for window...");
	while (!FindWindow("grcWindow", NULL))
	{
		Sleep(100);
	}
}

static void WaitForIntroToFinish()
{
	uintptr_t addr = (uintptr_t)hook::get_pattern("44 39 3D ? ? ? ? 75 09 83 BB ? ? ? ? ? 7D 24");
	addr = addr + *(int*)(addr + 3) + 7;
	unsigned int* gameState = (unsigned int*)addr;

	spdlog::info("Waiting for intro to finish...");
	while (*gameState == 0 || *gameState == 1)
	{
		Sleep(100);
	}
}

using IsScenarioVehicleInfo_fn = bool(*)(uint32_t index);
using CAmbientModelSetsManager_FindIndexByHash_fn = uint32_t(*)(void* mgr, int type, uint32_t hash);
using CScenarioInfoManager_GetScenarioTypeByHash_fn = uint32_t(*)(void* mgr, uint32_t* name, bool a3, bool searchInScenarioTypeGroups);
static IsScenarioVehicleInfo_fn IsScenarioVehicleInfo;
static CAmbientModelSetsManager_FindIndexByHash_fn CAmbientModelSetsManager_FindIndexByHash;
static CScenarioInfoManager_GetScenarioTypeByHash_fn CScenarioInfoManager_GetScenarioTypeByHash;

static void FindGameFunctions()
{
	IsScenarioVehicleInfo = (IsScenarioVehicleInfo_fn)hook::pattern("48 83 EC 28 48 8B 15 ? ? ? ? 0F B7 42 10 3B C8 7D 2A").get(1).get<void>();
	CAmbientModelSetsManager_FindIndexByHash = (CAmbientModelSetsManager_FindIndexByHash_fn)hook::get_pattern("44 89 44 24 ? 48 83 EC 28 48 63 C2 48 8D 14 80");
	CScenarioInfoManager_GetScenarioTypeByHash = (CScenarioInfoManager_GetScenarioTypeByHash_fn)hook::get_pattern("48 8B F9 66 39 59 48 76 1C 8B 02", -0x1C);
}

static void** g_AmbientModelSetsMgr;
static void** g_ScenarioInfoMgr;

static void FindGameVariables()
{
	g_AmbientModelSetsMgr = hook::get_address<void**>(hook::get_pattern("48 8B 0D ? ? ? ? E8 ? ? ? ? 83 F8 FF 75 07", 3));
	g_ScenarioInfoMgr = hook::get_address<void**>(hook::get_pattern("8B 42 30 48 8B 0D ? ? ? ? 48 8D 54 24 ? 89 44 24 30", 6));
}

struct ExtendedScenarioPoint
{
	uint32_t iType;
	uint32_t ModelSetId;
};
static std::unordered_map<CScenarioPoint*, ExtendedScenarioPoint> g_Points;

static void SavePoint(CScenarioPoint* point, uint32_t scenarioType, uint32_t modelSetId)
{
	g_Points[point] = { scenarioType, modelSetId };
}

static void RemovePoint(CScenarioPoint* point)
{
	g_Points.erase(point);
}

static void Patch1()
{
	spdlog::info("Patch 1...");

	// CScenarioPointRegion::LookUps::ConvertHashesToIndices
	hook::put(hook::get_pattern("41 BD ? ? ? ? 85 ED 7E 51 4C 8B F3", 2), 0xFFFFFFFF);
}

static void(*CScenarioPoint_TransformIdsToIndices_orig)(CScenarioPointRegion::sLookUps*, CScenarioPoint*);
static void CScenarioPoint_TransformIdsToIndices_detour(CScenarioPointRegion::sLookUps* indicesLookups, CScenarioPoint* point)
{
	uint32_t scenarioIndex = indicesLookups->TypeNames.Items[point->iType];

	atArray<uint32_t>* modelSetNames = IsScenarioVehicleInfo(scenarioIndex) ?
										&indicesLookups->VehicleModelSetNames :
										&indicesLookups->PedModelSetNames;

	SavePoint(point, scenarioIndex, modelSetNames->Items[point->ModelSetId]);

	CScenarioPoint_TransformIdsToIndices_orig(indicesLookups, point);

	spdlog::info(" TransformIdsToIndices:: detour -> point:{}, scenarioType:{:08X}, modelSet:{:08X}", (void*)point, scenarioIndex, modelSetNames->Items[point->ModelSetId]);
}

static void Patch2()
{
	spdlog::info("Patch 2...");

	// CScenarioPoint::TransformIdsToIndices
	MH_CreateHook(hook::get_pattern("48 8B 01 44 0F B6 42 ? 0F B6 72 16", -0xF), CScenarioPoint_TransformIdsToIndices_detour, (void**)&CScenarioPoint_TransformIdsToIndices_orig);
}

static void Patch3()
{
	spdlog::info("Patch 3...");

	// CScenarioInfoManager::IsValidModelSet
	hook::put(hook::get_pattern("81 FF ? ? ? ? 74 6F 48 8B 05", 2), 0xFFFFFFFF);
}

static void Patch4()
{
	spdlog::info("Patch 4...");

	// CScenarioPoint::CanSpawn
	static struct : jitasm::Frontend
	{
		static int GetModelSetIndex(CScenarioPoint* point)
		{
			if (!point)
			{
				return 0xFFFFFFFF;
			}

			auto p = g_Points.find(point);
			if (p != g_Points.end())
			{
				return p->second.ModelSetId;
			}
			else
			{
				return point->ModelSetId;
			}
		}

		void InternalMain() override
		{
			push(rcx);
			sub(rsp, 0x10);

			mov(rcx, rdi);
			mov(rax, (uintptr_t)GetModelSetIndex);
			call(rax);

			add(rsp, 0x10);
			pop(rcx);

			mov(edx, eax);
			ret();
		}
	} getModelSetIndexStub;

	auto location = hook::get_pattern("48 85 C9 74 06 0F B6 51 16 EB 05");
	hook::nop(location, 0x10);
	hook::call(location, getModelSetIndexStub.GetCode());
}

static void Patch5()
{
	spdlog::info("Patch 5...");

	// bool GetAndLoadScenarioPointModel(__int64 rcx0, signed int scenarioIndex, CScenarioPoint *point, __int64 a4, ...)
	static struct : jitasm::Frontend
	{
		static int GetModelSetIndex(CScenarioInfo* scenario, CScenarioPoint* point)
		{
			constexpr uint32_t CScenarioVehicleInfo_ClassId = 0xFB9AD9D7;

			if (scenario->GetIsClassId(CScenarioVehicleInfo_ClassId))
			{
				return 0xFFFFFFFF;
			}

			auto p = g_Points.find(point);
			if (p != g_Points.end())
			{
				return p->second.ModelSetId;
			}
			else
			{
				return point->ModelSetId;
			}
		}

		void InternalMain() override
		{
			push(rcx);
			push(rdx);
			sub(rsp, 0x18);

			mov(rcx, rdi); // first param: CScenarioInfo*
			mov(rdx, r14); // second param: CScenarioPoint*
			mov(rax, (uintptr_t)GetModelSetIndex);
			call(rax);

			add(rsp, 0x18);
			pop(rdx);
			pop(rcx);

			mov(r15d, eax);
			ret();
		}
	} getModelSetIndexStub;

	auto location = hook::get_pattern("48 8B CF 41 BF ? ? ? ? FF 10 84 C0");
	hook::nop(location, 0x14);
	hook::call(location, getModelSetIndexStub.GetCode());

	// cmp against 0xFFFFFFF
	hook::put(hook::get_pattern("41 81 FF ? ? ? ? 0F 85 ? ? ? ? B9", 3), 0xFFFFFFFF);
}

static bool(*CScenarioPoint_SetModelSet_orig)(CScenarioPoint*, uint32_t*, bool);
static bool CScenarioPoint_SetModelSet_detour(CScenarioPoint* _this, uint32_t* modelSetHash, bool isVehicle)
{
	constexpr uint32_t usepopulation_hash = 0xA7548A2;

	bool success = true;
	uint32_t hash = *modelSetHash;
	uint32_t index = 0xFFFFFFFF;
	if (hash != usepopulation_hash)
	{
		index = CAmbientModelSetsManager_FindIndexByHash(*g_AmbientModelSetsMgr, isVehicle ? 2 : 0, hash);
		if (index == 0xFFFFFFFF)
		{
			success = false;
		}
	}

	SavePoint(_this, 0xDEADBEEF, index);
	_this->ModelSetId = index;

	return success;
}

static void Patch6()
{
	spdlog::info("Patch 6...");

	// TODO: remove this hook
	//MH_CreateHook(hook::get_pattern("48 89 5C 24 ? 57 48 83 EC 20 C6 41 16 FF 41 8A C0"), CScenarioPoint_SetModelSet_detour, (void**)&CScenarioPoint_SetModelSet_orig);
}

static void(*CScenarioPoint_Delete_orig)(CScenarioPoint*);
static void CScenarioPoint_Delete_detour(CScenarioPoint* _this)
{
	RemovePoint(_this);

	CScenarioPoint_Delete_orig(_this);
}

static void Patch7()
{
	spdlog::info("Patch 7...");

	MH_CreateHook(hook::get_pattern("48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B CB E8 ? ? ? ? C6 05", -0xC), CScenarioPoint_Delete_detour, (void**)&CScenarioPoint_Delete_orig);
}

static void Patch8()
{
	spdlog::info("Patch 8...");

	static struct : jitasm::Frontend
	{
		static int GetModelSetIndex(CScenarioPoint* point)
		{
			auto p = g_Points.find(point);
			if (p != g_Points.end())
			{
				return p->second.ModelSetId;
			}
			else
			{
				return point->ModelSetId;
			}
		}

		void InternalMain() override
		{
			push(rcx);
			sub(rsp, 0x10);

			mov(rcx, rdi); // param: CScenarioPoint*
			mov(rax, (uintptr_t)GetModelSetIndex);
			call(rax);

			add(rsp, 0x10);
			pop(rcx);

			cmp(eax, 0xFFFFFFFF);
			ret();
		}
	} getModelSetIndexAndCmpStub;

	hook::pattern pattern("0F B6 47 16 3D ? ? ? ? 74 13 8B D0 48 8B 05");
	pattern.count(2);
	pattern.for_each_result([](const hook::pattern_match& match)
	{
		auto location = match.get<void>();
		hook::nop(location, 0x9);
		hook::call(location, getModelSetIndexAndCmpStub.GetCode());
	});
}

static uint32_t GetFinalModelSetHash(uint32_t hash)
{
	constexpr uint32_t any_hash = 0xDF3407B5;
	constexpr uint32_t usepopulation_hash = 0xA7548A2;

	return hash == any_hash ? usepopulation_hash : hash;
}

static void Patch9()
{
	// CScenarioPoint::InitFromSpawnPointDef

	static struct : jitasm::Frontend
	{
		static int Save(CScenarioPoint* point, char* extensionDefSpawnPoint, void* scenarioInfoMgr)
		{
			uint32_t spawnType = *(uint32_t*)(extensionDefSpawnPoint + 0x30);
			int scenarioType = CScenarioInfoManager_GetScenarioTypeByHash(scenarioInfoMgr, &spawnType, true, true);

			uint32_t pedType = *(uint32_t*)(extensionDefSpawnPoint + 0x34);
			uint32_t modelSetHash = GetFinalModelSetHash(pedType);
			int modelSetType = IsScenarioVehicleInfo(scenarioType) ? 2 : 0;
			uint32_t modelSet = CAmbientModelSetsManager_FindIndexByHash(*g_AmbientModelSetsMgr, modelSetType, modelSetHash);

			spdlog::info("InitFromSpawnPointDef:: Save -> point:{}, spawnType:{:08X}, scenarioType:{:08X}, modelSetHash:{:08X}, modelSet:{:08X}",
						(void*)point, spawnType, scenarioType, modelSetHash, modelSet);
			SavePoint(point, scenarioType, modelSet);

			return scenarioType;
		}

		void InternalMain() override
		{
			push(rcx);
			push(r8);
			sub(rsp, 0x18);

			mov(r8, rcx);    // third param:  CScenarioInfoManager*
			//mov(rdx, rdx); // second param: CExtensionDefSpawnPoint*
			mov(rcx, rbx);   // first param:  CScenarioPoint*
			mov(rax, (uintptr_t)Save);
			call(rax);

			add(rsp, 0x18);
			pop(r8);
			pop(rcx);

			ret();
		}
	} savePointStub;

	auto location = hook::get_pattern("41 B1 01 48 8D 55 20 89 45 20");
	hook::nop(location, 0x12);
	hook::call(location, savePointStub.GetCode());
}

static std::unordered_map<void*, uint32_t> g_SpawnPointsScenarioTypes;

static void(*CSpawnPoint_InitFromDef_orig)(void* spawnPoint, char* extensionDefSpawnPoint);
static void CSpawnPoint_InitFromDef_detour(void* spawnPoint, char* extensionDefSpawnPoint)
{
	uint32_t spawnType = *(uint32_t*)(extensionDefSpawnPoint + 0x30);
	uint32_t scenarioType = CScenarioInfoManager_GetScenarioTypeByHash(*g_ScenarioInfoMgr, &spawnType, true, true);

	g_SpawnPointsScenarioTypes[spawnPoint] = scenarioType;

	CSpawnPoint_InitFromDef_orig(spawnPoint, extensionDefSpawnPoint);
}

static void*(*CSpawnPoint_dtor_orig)(void* spawnPoint, char a2);
static void* CSpawnPoint_dtor_detour(void* spawnPoint, char a2)
{
	g_SpawnPointsScenarioTypes.erase(spawnPoint);

	return CSpawnPoint_dtor_orig(spawnPoint, a2);
}

static void Patch10()
{
	auto cspawnPointVTable = hook::get_address<void**>(hook::get_pattern("48 8D 05 ? ? ? ? 41 B9 ? ? ? ? 48 89 02", 3));
	CSpawnPoint_dtor_orig = (decltype(CSpawnPoint_dtor_orig))cspawnPointVTable[0];
	CSpawnPoint_InitFromDef_orig = (decltype(CSpawnPoint_InitFromDef_orig))cspawnPointVTable[2];

	cspawnPointVTable[0] = CSpawnPoint_dtor_detour;
	cspawnPointVTable[2] = CSpawnPoint_InitFromDef_detour;


	// CScenarioPoint::ctorWithEntity

	static struct : jitasm::Frontend
	{
		static uint32_t Save(CScenarioPoint* point, void* spawnPoint)
		{
			uint32_t scenarioType = 0;
			auto p = g_SpawnPointsScenarioTypes.find(spawnPoint);
			if (p != g_SpawnPointsScenarioTypes.end())
			{
				scenarioType = p->second;
			}

			uint32_t pedType = *(uint32_t*)((char*)spawnPoint + 0x1C);
			uint32_t modelSetHash = GetFinalModelSetHash(pedType);
			int modelSetType = IsScenarioVehicleInfo(scenarioType) ? 2 : 0;
			uint32_t modelSet = CAmbientModelSetsManager_FindIndexByHash(*g_AmbientModelSetsMgr, modelSetType, modelSetHash);

			spdlog::info("ctorWithEntity:: Save -> point:{}, spawnPoint:{}, scenarioType:{:08X}, modelSetHash:{:08X}, modelSet:{:08X}",
				(void*)point, spawnPoint, scenarioType, modelSetHash, modelSet);
			SavePoint(point, scenarioType, modelSet);

			return modelSetHash;
		}

		void InternalMain() override
		{
			push(rcx);
			push(rdx);
			sub(rsp, 0x18);

			mov(rdx, rdi); // second param: CSpawnPoint*
			mov(rcx, rbx); // first param:  CScenarioPoint*
			mov(rax, (uintptr_t)Save);
			call(rax);

			add(rsp, 0x18);
			pop(rdx);
			pop(rcx);

			ret();
		}
	} savePointStub;

	auto location = hook::get_pattern("E8 ? ? ? ? 89 47 1C 0F B6 4B 15");
	hook::nop(location, 0x5);
	hook::call(location, savePointStub.GetCode());
}

static void(*CSpawnPointOverrideExtension_OverrideScenarioPoint_orig)(char* spawnPointOverrideExtension, CScenarioPoint* point);
static void CSpawnPointOverrideExtension_OverrideScenarioPoint_detour(char* spawnPointOverrideExtension, CScenarioPoint* point)
{
	auto p = g_Points.find(point);
	if (p != g_Points.end()) // every point passed to this method should already be in the map from the CScenarioPoint::ctorWithEntity hook
	{
		uint32_t origiType = p->second.iType;
		uint32_t origModelSetId = p->second.ModelSetId;

		uint32_t overrideScenarioType = *(uint32_t*)(spawnPointOverrideExtension + 0x8);
		if (overrideScenarioType)
		{
			uint32_t newScenarioType = CScenarioInfoManager_GetScenarioTypeByHash(*g_ScenarioInfoMgr, &overrideScenarioType, true, true);
			if (newScenarioType == 0xFFFFFFFF)
				newScenarioType = 0;
			p->second.iType = newScenarioType;
		}

		uint32_t overrideModelSet = *(uint32_t*)(spawnPointOverrideExtension + 0x14);
		if (overrideModelSet)
		{
			overrideModelSet = GetFinalModelSetHash(overrideModelSet);
			int modelSetType = IsScenarioVehicleInfo(p->second.iType) ? 2 : 0;
			uint32_t newModelSet = CAmbientModelSetsManager_FindIndexByHash(*g_AmbientModelSetsMgr, modelSetType, overrideModelSet);
			p->second.ModelSetId = newModelSet;
		}


		spdlog::info("OverrideScenarioPoint:: detour -> spawnPointOverrideExtension:{}, point:{}, iType:{:08X}, new_iType:{:08X}, ModelSetId:{:08X}, new_ModelSetId:{:08X}",
			(void*)spawnPointOverrideExtension, (void*)point, origiType, p->second.iType, origModelSetId, p->second.ModelSetId);
	}

	CSpawnPointOverrideExtension_OverrideScenarioPoint_orig(spawnPointOverrideExtension, point);
}

static void Patch11()
{
	// CSpawnPointOverrideExtension::OverrideScenarioPoint

	MH_CreateHook(hook::get_pattern("48 83 EC 20 8B 41 08 33 FF 48 8B F2", -0xB), CSpawnPointOverrideExtension_OverrideScenarioPoint_detour,
		(void**)&CSpawnPointOverrideExtension_OverrideScenarioPoint_orig);
}

static uint32_t GetScenarioType(CScenarioPoint* point)
{
	auto p = g_Points.find(point);
	if (p != g_Points.end())
	{
		return p->second.iType;
	}
	else
	{
		return point->iType;
	}
}

static void Patch12()
{
	// patch calls to CScenarioPoint::GetScenarioType

	constexpr int R14_REG = 0;
	constexpr int EBX_REG = 1;
	constexpr int ECX_REG = 2;
	constexpr int ESI_REG = 3;
	struct stub : jitasm::Frontend
	{
		const int m_reg;

		stub(int reg) : m_reg(reg)
		{
		}

		void InternalMain() override
		{
			sub(rsp, 0x8);

			// rcx already is CScenarioPoint*
			mov(rax, (uintptr_t)GetScenarioType);
			call(rax);

			add(rsp, 0x8);

			switch (m_reg)
			{
			case R14_REG: mov(r14d, eax); break;
			case EBX_REG: mov(ebx, eax); break;
			case ECX_REG: mov(ecx, eax); break;
			case ESI_REG: mov(esi, eax); break;
			}
			
			ret();
		}
	};
	static stub getScenarioTypeAndStoreInR14Stub(R14_REG);
	static stub getScenarioTypeAndStoreInEBXStub(EBX_REG);
	static stub getScenarioTypeAndStoreInECXStub(ECX_REG);
	static stub getScenarioTypeAndStoreInESIStub(ESI_REG);

	{
		auto location = hook::get_pattern("E8 ? ? ? ? 44 0F B6 F0 41 8B CE E8");
		hook::nop(location, 0x9);
		hook::call(location, getScenarioTypeAndStoreInR14Stub.GetCode());
	}

	{
		auto location = hook::get_pattern("E8 ? ? ? ? 0F B6 D8 8B CB E8");
		hook::nop(location, 0x8);
		hook::call(location, getScenarioTypeAndStoreInEBXStub.GetCode());
	}
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 4C 8B 0D ? ? ? ? 45 0F B7 51");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInEBXStub.GetCode());
		hook::nop((char*)location + 0x11, 3);
	}
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 48 8B 15 ? ? ? ? 0F B7 4A 10");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInEBXStub.GetCode());
		hook::nop((char*)location + 0x10, 3);
	}

	{
		auto location = hook::get_pattern("E8 ? ? ? ? 0F B6 C8 E8");
		hook::nop(location, 0x8);
		hook::call(location, getScenarioTypeAndStoreInECXStub.GetCode());
	}
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 48 8B D5 0F B6 C8");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInECXStub.GetCode());
		hook::nop((char*)location + 0x8, 3);
	}
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 48 8B D3 0F B6 C8");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInECXStub.GetCode());
		hook::nop((char*)location + 0x8, 3);
	}

	{
		auto location = hook::get_pattern("E8 ? ? ? ? 4C 8B 2D ? ? ? ? 44 8A FF");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInESIStub.GetCode());
		hook::nop((char*)location + 0xF, 3);
	}
}

static DWORD WINAPI Main()
{
	if (EnableLogging)
	{
		spdlog::set_default_logger(spdlog::basic_logger_mt("file_logger", "CScenarioPoint-Patch.log"));
		spdlog::flush_every(std::chrono::seconds(5));
	}
	else
	{
		spdlog::set_level(spdlog::level::off);
	}

	spdlog::info("Initializing MinHook...");
	MH_Initialize();
	
	FindGameFunctions();
	FindGameVariables();

	Patch1();
	Patch2();
	Patch3();
	Patch4();
	Patch5();
	Patch6();
	Patch7();
	Patch8();
	Patch9();
	Patch10();
	Patch11();
	Patch12();

	MH_EnableHook(MH_ALL_HOOKS);

	//WaitForWindow();
	//WaitForIntroToFinish();

	spdlog::info("End");
	return 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		CloseHandle(CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)Main, NULL, NULL, NULL));
	}
	else if (ul_reason_for_call == DLL_PROCESS_DETACH)
	{
		spdlog::shutdown();
	}

	return TRUE;
}
