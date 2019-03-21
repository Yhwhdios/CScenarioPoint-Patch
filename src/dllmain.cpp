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
using CScenarioInfoManager_GetScenarioTypeByHash_fn = int(*)(void* mgr, uint32_t* name, bool a3, bool searchInScenarioTypeGroups);
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

static void FindGameVariables()
{
	g_AmbientModelSetsMgr = hook::get_address<void**>(hook::get_pattern("48 8B 0D ? ? ? ? E8 ? ? ? ? 83 F8 FF 75 07", 3));
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

	//spdlog::info(" TransformIdsToIndices:: OrigIndex -> {} | FinalIndex -> {}  (Total: {})", p.ModelSetId, point->ModelSetId, g_Points.size());
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
	MH_CreateHook(hook::get_pattern("48 89 5C 24 ? 57 48 83 EC 20 C6 41 16 FF 41 8A C0"), CScenarioPoint_SetModelSet_detour, (void**)&CScenarioPoint_SetModelSet_orig);
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

			spdlog::info("InitFromSpawnPointDef:: SavePoint -> point:{}, spawnType:{:08X}, scenarioType:{:08X}, modelSetHash:{:08X}, modelSet:{:08X}",
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
