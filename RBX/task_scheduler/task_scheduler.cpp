#include <Utilitys/Globals.hpp>
#include <Utilitys/Utils.hpp>
#include <Utilitys/Log.hpp>
#include <ltable.h>
#include <lstring.h>
#include <lgc.h>
#include <RBX/execution/execution.hpp>
#include <RBX/environment/env.hpp>
#include <RBX/task_scheduler/task_scheduler.hpp>
#include <RBX/environment/yielding/yielding.hpp>
#include <RBX/environment/libs/raknet/raknet.hpp>
#include <RBX/environment/libs/websocket/websocket.hpp>
#include <RBX/environment/libs/drawing/drawing.hpp>
#include <RBX/environment/libs/drawingimmediate/drawingimmediate.hpp>
#include <RBX/environment/libs/closures/closures.hpp>
#include <RBX/environment/libs/signals/signals.hpp>
#include <RBX/environment/libs/reflection/reflection.hpp>
#include <RBX/environment/libs/oth/oth.hpp>
#include <RBX/environment/libs/actor/actor.hpp>
#include <RBX/diagnostics.hpp>
#include <Renderer/Renderer.hpp>
#include <RBX/environment/env.hpp>
#include <imgui/addons/imgui_notify.h>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <imgui/backends/imgui_impl_dx11.h>
#include <imgui/backends/imgui_impl_win32.h>
#include <imgui/imgui.h>
#include <imgui/addons/TextEditor.h>
#include <imgui/imgui.h>
#include <Windows.h>
#include <d3d11.h>
#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui/imgui_internal.h>
namespace
{
	thread_local std::string LastSchedulerError;
	thread_local const char* InitializationPhase = "not started";

	// Filled from the __except filter of ExecuteGame (GetExceptionInformation
	// is only valid there) so the handler can log where the fault happened.
	struct DispatchFault
	{
		unsigned long Code = 0;
		void* Address = nullptr;
		const char* Stage = nullptr;
		const char* Callback = nullptr;
	};
	thread_local DispatchFault LastDispatchFault;

	void ReportSchedulerError(const char* Message)
	{
		if (LastSchedulerError == Message)
			return;

		LastSchedulerError = Message;
		LOGF("TaskScheduler", "%s", Message);
	}

	// ── Init-stage gate ──────────────────────────────────────────────────────
	// Bisecting the client regression needs a way to stop initialization after
	// a given sub-stage without rebuilding. The stage comes from glx_stage.txt
	// next to Module.dll; a missing or invalid file means full initialization.
	//   0 = job hook only, VM untouched
	//   1 = executor thread created + rooted in the registry
	//   2 = + luaL_sandboxthread
	//   3 = + client globals imported
	//   4 = + environment libraries registered
	//   5 = + Instance direct-access fast paths cleared
	//   6 = + Instance metatable hooks installed
	//   7 = full (dispatch enabled)
	//   8 = allocation-only probe (two tables, no sandbox)   [diagnostic]
	//   9 = + luaL_sandboxthread with the collector suppressed [diagnostic]
	//  10 = full init with our compiled luaC_step disabled     [diagnostic]
	constexpr int kFullInitStage = 7;
	constexpr int kMaxInitStage = 10;
	int g_InitStage = kFullInitStage;
	bool g_PartialInit = false;

	// Self-test: one tiny script is queued through the ordinary execution path
	// right after full init, and the following job steps watch for its marker.
	// This answers "does an injected script actually run" from the log alone,
	// with no user interaction. 0 = idle, 1 = waiting, 2 = passed, 3 = failed.
	int g_SelfTestState = 0;
	int g_SelfTestSteps = 0;

	int ReadInitStageFile()
	{
		char ModulePath[MAX_PATH]{};
		HMODULE Module = nullptr;
		if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&ReadInitStageFile), &Module))
			return kFullInitStage;

		const DWORD Length = GetModuleFileNameA(Module, ModulePath, MAX_PATH);
		if (Length == 0 || Length >= MAX_PATH)
			return kFullInitStage;

		char* Slash = strrchr(ModulePath, '\\');
		if (!Slash)
			return kFullInitStage;
		Slash[1] = '\0';

		char StagePath[MAX_PATH]{};
		snprintf(StagePath, sizeof(StagePath), "%sglx_stage.txt", ModulePath);

		FILE* File = nullptr;
		if (fopen_s(&File, StagePath, "r") != 0 || !File)
			return kFullInitStage;

		int Stage = -1;
		const int Read = fscanf_s(File, "%d", &Stage);
		fclose(File);
		if (Read != 1 || Stage < 0 || Stage > kMaxInitStage)
			return kFullInitStage;

		return Stage;
	}

	bool FinishPartialInit(const char* StageName)
	{
		// Marking the game initialized stops stepHook from retrying every frame,
		// while ExecuteGameImpl early-returns because the gate is below full.
		Shared::IsInitializedOnGame = true;
		g_PartialInit = true;
		LastSchedulerError.clear();
		LOGF("TaskScheduler", "Init stage gate: stopped after %s", StageName);
		return true;
	}
}

// ══════════════════════════════════════════════════════════════════════════════
// Identity / Capability Helpers
// ══════════════════════════════════════════════════════════════════════════════

void task_scheduler::SetProtoCapabilities(Proto* Root, uintptr_t* Capabilities)
{
	if (!Root)
		return;

	std::vector<Proto*> Pending{ Root };
	while (!Pending.empty())
	{
		Proto* Current = Pending.back();
		Pending.pop_back();
		if (!Current)
			continue;

		Current->userdata = Capabilities;
		if (Current->sizep < 0 || Current->sizep > 100000 || (!Current->p && Current->sizep != 0))
			continue;

		for (int Index = 0; Index < Current->sizep; ++Index)
			if (Current->p[Index])
				Pending.push_back(Current->p[Index]);
	}
}

void task_scheduler::SetThreadCapabilities(lua_State* L, int Level, uintptr_t Capabilities)
{
	if (!L || !L->userdata)
		return;

	L->userdata->identity.type = Level;
	L->userdata->capabilities = Capabilities;
}

void task_scheduler::SetIdentity(lua_State* Thread, int Identity, bool IsInstance)
{
	if (!Thread) {
		return;
	}
	if (!Thread->userdata) {
		return;
	}

	RobloxExtraSpace* basedData = reinterpret_cast<RobloxExtraSpace*>(Thread->userdata);
	basedData->identity.type = Identity;
	basedData->capabilities = Roblox::GetCapabilities(&Identity);

	if (IsInstance) {
		try {
			uintptr_t IdentityPointer = *reinterpret_cast<uintptr_t*>(Offsets::RobloxThread::IdentityPtr);
			if (!IdentityPointer || IdentityPointer > 0xFFFFFFFFFFFF) {
				return;
			}
			uintptr_t IdentityStructure = Roblox::GetIdentityStruct(IdentityPointer);
			if (!IdentityStructure) {
				return;
			}
			*reinterpret_cast<int32_t*>(IdentityStructure) = Identity;
			*reinterpret_cast<uintptr_t*>(IdentityStructure + Offsets::Capabilities::Capabilities) =
				Roblox::GetCapabilities(&Identity);
		}
		catch (...) {
		}
	}
}

// ══════════════════════════════════════════════════════════════════════════════
// DataModel / ScriptContext / LuaState Retrieval
// ══════════════════════════════════════════════════════════════════════════════

uintptr_t task_scheduler::GetDataModel()
{
	__try
	{
		uintptr_t FakeDataModel = *reinterpret_cast<uintptr_t*>(Offsets::DataModel::FakeDataModelPointer);
		if (!FakeDataModel)
			return 0;
		return *reinterpret_cast<uintptr_t*>(FakeDataModel + Offsets::DataModel::FakeDataModelToDataModel);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

uintptr_t task_scheduler::GetScriptContext(uintptr_t DataModel)
{
	if (!DataModel)
		return 0;
	__try
	{
		const auto lookup = [DataModel]() { return task_scheduler::GetJobByTypeName("WaitingHybridScriptsJob", DataModel); };
		const uintptr_t Job = lookup();
		if (!Job)
		{
			ReportSchedulerError("ScriptContext lookup: waiting hybrid job is missing");
			return 0;
		}
		const uintptr_t Context = *reinterpret_cast<uintptr_t*>(Job + ClientOffsets::jobs_offsets::scriptContext);
		if (!Context)
		{
			ReportSchedulerError("ScriptContext lookup: job context is null");
			return 0;
		}
		const uintptr_t ContextDataModel = *reinterpret_cast<uintptr_t*>(Context + ClientOffsets::ScriptContext::DataModel);
		if (ContextDataModel != DataModel && ContextDataModel + ClientOffsets::DataModel::InstanceSubobject != DataModel)
		{
			char Message[256]{};
			snprintf(Message, sizeof(Message), "ScriptContext DataModel mismatch: job=%p context=%p contextDataModel=%p expected=%p",
				reinterpret_cast<void*>(Job), reinterpret_cast<void*>(Context),
				reinterpret_cast<void*>(ContextDataModel), reinterpret_cast<void*>(DataModel));
			ReportSchedulerError(Message);
			return 0;
		}
		return Context;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ReportSchedulerError("ScriptContext lookup raised a memory exception");
		return 0;
	}
}

inline bool CheckMemory(uintptr_t address) {
	if (address < 0x10000 || address > 0x7FFFFFFFFFFF)
		return false;

	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0)
		return false;

	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD) || mbi.State != MEM_COMMIT)
		return false;

	return true;
}

inline lua_State* GetLuaStateForInstance(uintptr_t sc)
{
    if (!sc) return nullptr;
    __try
    {
        *reinterpret_cast<BOOLEAN*>(sc + Offsets::ExtraSpace::RequireBypass) = TRUE;
        uintptr_t identity = 2;
        uintptr_t script = 0;
        lua_State* state = Roblox::GetLuaStateForInstance(sc, &identity, &script);
        if (!state || !CheckMemory(reinterpret_cast<uintptr_t>(state)) || state->tt != LUA_TTHREAD || !state->global)
            return nullptr;
        return state;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

lua_State* task_scheduler::GetLuaState(uintptr_t ScriptContext, uintptr_t Identity, uintptr_t Script)
{
	return Roblox::GetLuaStateForInstance(ScriptContext, &Identity, &Script);
}

// ══════════════════════════════════════════════════════════════════════════════
// GetGameLoaded — reads raw GameLoaded state from DataModel
// ══════════════════════════════════════════════════════════════════════════════

static bool IsGameLoaded(uintptr_t Datamodel)
{
	if (!Datamodel)
		return 0;
	__try
	{
		return *reinterpret_cast<int64_t*>(Datamodel + ClientOffsets::DataModel::PlaceId) > 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

// ══════════════════════════════════════════════════════════════════════════════
// Script Processing
// ══════════════════════════════════════════════════════════════════════════════

int ScriptsHandler(lua_State* L)
{
	lua_State* BoundExecutorThread = static_cast<lua_State*>(
		lua_touserdata(L, lua_upvalueindex(1)));
	if (!BoundExecutorThread || BoundExecutorThread != Shared::ExploitThread)
		return 0;

	// drain the whole queue each tick — one script per step made a batch
	// of N scripts cost N frames
	for (;;)
	{
		std::string Script;
		{
			std::lock_guard<std::mutex> lock(Shared::ExecutionRequestsMutex);
			if (Shared::ExecutionRequests.empty())
				break;
			Script = std::move(Shared::ExecutionRequests.front());
			Shared::ExecutionRequests.erase(Shared::ExecutionRequests.begin());
		}

		if (!Script.empty())
			Execution::ExecuteScript(BoundExecutorThread, std::move(Script));
	}

	Raknet::ProcessCallbacks(BoundExecutorThread);

	Yielding::RunYield();

	return 0;
}



static void RestoreJobVtableSafely()
{
	__try
	{
		if (Shared::LastWaitingHybridScriptsJob && Shared::OriginalHybridJobVtable &&
			CheckMemory(Shared::LastWaitingHybridScriptsJob))
		{
			*reinterpret_cast<uintptr_t**>(Shared::LastWaitingHybridScriptsJob) = Shared::OriginalHybridJobVtable;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

static std::mutex g_StepHookMutex;

void Reset()
{
	Shared::JobStepLocked = true;
	Shared::IsInitializedOnGame = false;
	g_PartialInit = false;
	Shared::StateGeneration.fetch_add(1);

	std::lock_guard<std::mutex> stepLock(g_StepHookMutex);

	LOGF("Reset", "begin");
	// Everything that writes into engine or Lua objects is deliberately
	// skipped during teardown. Roblox is destroying the DataModel concurrently:
	// a job/RakPeer/closure we still hold can already be freed or mid-collection,
	// and writing to it corrupts the heap (crash a few seconds later in engine
	// code). Destroying a Lua state does not invoke hooked closures, and
	// stepHook is gated by JobStepLocked, so leaving those hooks installed is
	// harmless.
	// RestoreJobVtableSafely();
	// Closures::RestoreAllHooksForTeardown(Shared::RobloxState);

	Websocket::teardown();
	LOGF("Reset", "websocket torn down");
	Drawing::ResetLibraryState();
	Closures::ResetLibraryState();
	Signals::ResetLibraryState();
	Reflection::ResetLibraryState();
	Oth::ResetLibrary();

	Shared::ExploitThread = nullptr;
	Shared::RobloxState = nullptr;
	Shared::ScriptContext = 0;

	// Drop the cached client dispatch targets for the next game. The registry
	// slots are few and reusing them across a torn-down VM is not safe, so they
	// are only forgotten here.
	Shared::ClientTaskRef = LUA_NOREF;
	Shared::ClientTaskDeferRef = LUA_NOREF;
	Shared::ExploitThreadRef = LUA_NOREF;
	Shared::ClientGlobals = nullptr;

	{
		std::lock_guard<std::mutex> lock(Shared::YieldQueueMutex);
		std::queue<std::function<void()>> empty;
		Shared::YieldQueue.swap(empty);
	}
	{
		std::lock_guard<std::mutex> lock(Shared::ExecutionRequestsMutex);
		Shared::ExecutionRequests.clear();
	}
	Shared::ExecutorClosures.clear();
	Raknet::ResetHook(false); // skip vtable writes into possibly-freed engine objects
	LOGF("Reset", "done");

	// No UI notification during teardown: the ImGui context can already be
	// gone and InsertNotification touches renderer state that Roblox frees.
	// ImGuiToast toast(...); ImGui::InsertNotification(...);
}

namespace
{
	const char* ProbeTypeName(int Type)
	{
		switch (Type)
		{
		case LUA_TNIL: return "nil";
		case LUA_TBOOLEAN: return "boolean";
		case LUA_TLIGHTUSERDATA: return "lightuserdata";
		case LUA_TNUMBER: return "number";
		case LUA_TVECTOR: return "vector";
		case LUA_TSTRING: return "string";
		case LUA_TTABLE: return "table";
		case LUA_TFUNCTION: return "function";
		case LUA_TUSERDATA: return "userdata";
		case LUA_TTHREAD: return "thread";
		case LUA_TBUFFER: return "buffer";
		default: return "none";
		}
	}

	// Plain-data results: the functions doing the __try may not need C++
	// unwinding (MSVC C2712), so all logging happens after the guard returns.
	struct GlobalProbe
	{
		int Type = LUA_TNONE;
		void* Pointer = nullptr;
		bool Faulted = false;
		unsigned long FaultCode = 0;
	};

	bool RunGlobalProbe(lua_State* L, const char* Name, GlobalProbe* Result)
	{
		__try
		{
			lua_getglobal(L, Name);
			Result->Type = lua_type(L, -1);
			Result->Pointer = const_cast<void*>(lua_topointer(L, -1));
			lua_pop(L, 1);
			return true;
		}
		__except (
			(Result->FaultCode = GetExceptionCode()),
			(Result->Faulted = true),
			EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	struct DispatchCache
	{
		int TaskType = LUA_TNONE;
		void* TaskPointer = nullptr;
		int DeferType = LUA_TNONE;
		void* DeferPointer = nullptr;
		bool DeferIsC = false;
		bool Faulted = false;
		unsigned long FaultCode = 0;
	};

	bool CaptureDispatchTargets(lua_State* L, DispatchCache* Out)
	{
		__try
		{
			lua_getglobal(L, "task");
			Out->TaskType = lua_type(L, -1);
			Out->TaskPointer = const_cast<void*>(lua_topointer(L, -1));
			if (Out->TaskType == LUA_TTABLE)
			{
				Shared::ClientTaskRef = lua_ref(L, -1);
				lua_getfield(L, -1, "defer");
				Out->DeferType = lua_type(L, -1);
				Out->DeferPointer = const_cast<void*>(lua_topointer(L, -1));
				if (Out->DeferType == LUA_TFUNCTION)
				{
					Closure* DeferClosure = clvalue(index2addr(L, -1));
					Out->DeferIsC = DeferClosure && DeferClosure->isC;
					Shared::ClientTaskDeferRef = lua_ref(L, -1);
				}
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
			return true;
		}
		__except (
			(Out->FaultCode = GetExceptionCode()),
			(Out->Faulted = true),
			EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void LogGlobalProbe(const char* When, const char* Name, const GlobalProbe& Probe)
	{
		if (Probe.Faulted)
			LOGF("TaskScheduler", "%s: %s probe faulted 0x%08lX", When, Name, Probe.FaultCode);
		else
			LOGF("TaskScheduler", "%s: %s = %s @ %p", When, Name,
				ProbeTypeName(Probe.Type), Probe.Pointer);
	}

	void LogRawGlobal(const char* When, const char* Name, LuaTable* Table, lua_State* L)
	{
		if (!Table || !L)
		{
			LOGF("TaskScheduler", "%s: raw %s has no table", When, Name);
			return;
		}

		TString* Key = luaS_new(L, Name);
		const TValue* Value = luaH_getstr(Table, Key);
		const int Type = Value ? ttype(Value) : LUA_TNIL;
		void* Pointer = (Value && ttistable(Value)) ? reinterpret_cast<void*>(hvalue(Value)) : nullptr;
		LOGF("TaskScheduler", "%s: raw %s in %p = %s @ %p", When, Name,
			reinterpret_cast<void*>(Table), ProbeTypeName(Type), Pointer);
	}

	// Sanity-check the global state / registry before and after lua_ref. The
	// ref number observed in diag5/diag6 was 0x70000000, which is not a normal
	// registry index, so the registry free list and table header are logged.
	void LogRegistryState(const char* When, lua_State* L)
	{
		if (!L || !L->global)
		{
			LOGF("TaskScheduler", "%s: no state/global", When);
			return;
		}

		global_State* G = L->global;
		const TValue* RegistryValue = registry(L);
		const int RegistryType = ttype(RegistryValue);
		LuaTable* Registry = ttistable(RegistryValue) ? hvalue(RegistryValue) : nullptr;

		LOGF("TaskScheduler",
			"%s: global=%p mainthread=%p robloxstate=%p registry=%p type=%s free=%d",
			When, reinterpret_cast<void*>(G), reinterpret_cast<void*>(G->mainthread),
			reinterpret_cast<void*>(Shared::RobloxState),
			reinterpret_cast<void*>(Registry), ProbeTypeName(RegistryType),
			G->registryfree);

		if (Registry)
			LOGF("TaskScheduler",
				"%s: registry sizearray=%d aboundary=%d nodemask8=%d getn=%d",
				When, Registry->sizearray, Registry->aboundary,
				Registry->nodemask8, luaH_getn(Registry));
	}

	// Snapshot of the collector so a bisect stage can tell whether one of our
	// VM calls let luaC_step run on the client's heap (gcstate, totalbytes and
	// GCthreshold all move when a step fires).
	void LogGcState(const char* When, lua_State* L)
	{
		if (!L || !L->global)
		{
			LOGF("TaskScheduler", "%s: gc: no state/global", When);
			return;
		}

		global_State* G = L->global;
		LOGF("TaskScheduler",
			"%s: gc: state=%u white=%u disabled=%u bytes=%llu threshold=%llu gray=%p weak=%p grayagain=%p",
			When, unsigned(G->gcstate), unsigned(G->currentwhite),
			unsigned(G->gcdisabled), static_cast<unsigned long long>(G->totalbytes),
			static_cast<unsigned long long>(G->GCthreshold),
			reinterpret_cast<void*>(G->gray), reinterpret_cast<void*>(G->weak),
			reinterpret_cast<void*>(G->grayagain));
	}
}

// Self-test queueing lives outside InitializeGameImpl: the std::string temporary
// it creates needs unwinding, which MSVC forbids in a function that uses __try.
static void QueueSelfTestMarker()
{
	g_SelfTestState = 1;
	g_SelfTestSteps = 0;
	task_scheduler::RequestExecution("__glx_selftest_marker = 'ran'");
	LOGF("TaskScheduler", "Self-test: marker script queued");
}

static bool InitializeGameImpl()
{
	if (Shared::IsInitializedOnGame)
		return true;

	g_InitStage = ReadInitStageFile();
	Shared::InitStage = g_InitStage;
	LOGF("TaskScheduler", "Init stage gate = %d of %d", g_InitStage, kFullInitStage);

	if (g_InitStage == 10)
	{
		// Full initialization, but our separately compiled collector may not run
		// a step over the client's heap. If full init is clean with this on, the
		// GC step was the corruption and this becomes the permanent fix.
		luaC_step_suppressed = true;
		LOGF("TaskScheduler", "Collector: our luaC_step disabled for this run");
	}

	// Our VM is a separate Luau build, so its FFlag defaults are not the client's
	// runtime values. The disagreements (string interning salt, cage split data,
	// buffer sweep) change how we touch the client's shared heap, so mirror them
	// before creating the executor thread.
	SyncClientLuauFlags();

	if (g_InitStage < 1)
	{
		// Stage 0: leave the client VM completely untouched. Only the hybrid
		// job vtable hook installed by TeleportHandler remains in play.
		Shared::IsInitializedOnGame = true;
		LastSchedulerError.clear();
		LOGF("TaskScheduler", "Init stage gate: stopped after job hook only");
		return true;
	}

	InitializationPhase = "ScriptContext lookup";
	uintptr_t ScriptContext =
		task_scheduler::GetScriptContext(Shared::LastDataModel);

	if (!ScriptContext)
		return false;
	InitializationPhase = "native Lua state lookup";
	lua_State* MainThread = nullptr;
	__try { MainThread = GetLuaStateForInstance(ScriptContext); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }

	if (!MainThread)
	{
		ReportSchedulerError("Native Lua state lookup returned no valid thread");
		return false;
	}

	Shared::RobloxState = MainThread;

	Shared::RobloxState = reinterpret_cast<lua_State*>(MainThread);
	if (!Shared::RobloxState)
		return false;

	InitializationPhase = "lua_newthread";
	Shared::ExploitThread = lua_newthread(Shared::RobloxState);
	if (!Shared::ExploitThread)
	{
		ReportSchedulerError("lua_newthread returned null");
		return false;
	}

	Shared::ClientTaskRef = LUA_NOREF;
	Shared::ClientTaskDeferRef = LUA_NOREF;

	// Root the executor thread in the registry before anything else can run the
	// collector: thread->gt is only traced while the thread itself is marked. A
	// GC pass right after the first dispatch otherwise frees the sandbox proxy,
	// after which every global lookup in later script threads resolves to nil.
	LogRegistryState("pre-ref", Shared::RobloxState);
	Shared::ExploitThreadRef = lua_ref(Shared::RobloxState, -1);
	LogRegistryState("post-ref", Shared::RobloxState);
	LOGF("TaskScheduler", "Rooted executor thread on RobloxState stack top: ref=%d", Shared::ExploitThreadRef);

	// The fresh thread still shares the client's real global table; remember it
	// and resolve the dispatch target before the sandbox replaces gt with a
	// proxy.
	Shared::ClientGlobals = Shared::ExploitThread->gt;
	LOGF("TaskScheduler", "Globals: main=%p executor_gt=%p client=%p",
		reinterpret_cast<void*>(MainThread->gt),
		reinterpret_cast<void*>(Shared::ExploitThread->gt),
		reinterpret_cast<void*>(Shared::ClientGlobals));

	if (g_InitStage <= 1)
		return FinishPartialInit("executor thread root");

	InitializationPhase = "dispatch target capture";
	{
		GlobalProbe TaskProbe{};
		RunGlobalProbe(Shared::ExploitThread, "task", &TaskProbe);
		LogGlobalProbe("pre-sandbox", "task", TaskProbe);

		LogRawGlobal("pre-sandbox", "task", Shared::ClientGlobals, Shared::ExploitThread);

		// The cached refs only exist to feed dispatch, so partial stages skip
		// them: this keeps stage 1 limited to creating and rooting the thread.
		if (g_InitStage >= kFullInitStage)
		{
			DispatchCache Cache{};
			if (CaptureDispatchTargets(Shared::ExploitThread, &Cache))
			{
				LOGF("TaskScheduler",
					"Dispatch target: task=%s@%p defer=%s@%p isC=%d refs=%d/%d",
					ProbeTypeName(Cache.TaskType), Cache.TaskPointer,
					ProbeTypeName(Cache.DeferType), Cache.DeferPointer,
					Cache.DeferIsC ? 1 : 0,
					Shared::ClientTaskRef, Shared::ClientTaskDeferRef);
			}
			else
			{
				LOGF("TaskScheduler", "Dispatch target capture faulted 0x%08lX", Cache.FaultCode);
			}
		}
	}

	InitializationPhase = "sandbox thread";

	if (g_InitStage == 8)
	{
		// Allocate exactly what the sandbox allocates (two tables) but stop short
		// of touching the thread's gt. If this alone breaks the client, the poison
		// is our allocation/GC path and not sandbox semantics.
		LogGcState("alloc-probe pre", Shared::RobloxState);
		lua_newtable(Shared::ExploitThread);
		LogGcState("alloc-probe after table 1", Shared::RobloxState);
		lua_newtable(Shared::ExploitThread);
		LogGcState("alloc-probe after table 2", Shared::RobloxState);
		lua_pop(Shared::ExploitThread, 2);
		LogGcState("alloc-probe pop", Shared::RobloxState);
		return FinishPartialInit("allocation probe");
	}

	if (g_InitStage == 9)
	{
		// Same call as stage 2, but with the collector masked off so our compiled
		// luaC_step cannot run against the client heap. Clean here means a GC step
		// inside the sandbox is what corrupts the client.
		global_State* G = Shared::RobloxState->global;
		const size_t SavedThreshold = G->GCthreshold;
		const uint8_t SavedDisabled = G->gcdisabled;

		LogGcState("pre-sandbox-suppressed", Shared::RobloxState);
		G->GCthreshold = static_cast<size_t>(-1);
		G->gcdisabled = 1;
		luaL_sandboxthread(Shared::ExploitThread);
		LogGcState("post-sandbox-suppressed", Shared::RobloxState);
		G->GCthreshold = SavedThreshold;
		G->gcdisabled = SavedDisabled;
		LogGcState("post-sandbox-restored", Shared::RobloxState);

		return FinishPartialInit("sandbox thread (collector suppressed)");
	}

	LogGcState("pre-sandbox", Shared::RobloxState);
	luaL_sandboxthread(Shared::ExploitThread);
	LogGcState("post-sandbox", Shared::RobloxState);

	if (g_InitStage <= 2)
		return FinishPartialInit("sandbox thread");

	InitializationPhase = "client global import";
	Environment->ImportClientGlobals(Shared::ExploitThread, Shared::ClientGlobals);

	if (g_InitStage <= 3)
		return FinishPartialInit("client global import");

	InitializationPhase = "environment initialization";
	Environment->initalizeEnvironment(Shared::ExploitThread);

	if (g_InitStage <= 4)
		return FinishPartialInit("environment libraries");
	if (g_InitStage <= 5)
		return FinishPartialInit("instance fast paths");
	if (g_InitStage <= 6)
		return FinishPartialInit("instance hooks");

	InitializationPhase = "post-init probes";
	{
		GlobalProbe TaskProbe{};
		RunGlobalProbe(Shared::ExploitThread, "task", &TaskProbe);
		LogGlobalProbe("post-init", "task", TaskProbe);

		GlobalProbe PcallProbe{};
		RunGlobalProbe(Shared::ExploitThread, "pcall", &PcallProbe);
		LogGlobalProbe("post-init", "pcall", PcallProbe);

		LogRawGlobal("post-init", "task", Shared::ExploitThread->gt, Shared::ExploitThread);
	}

	Shared::IsInitializedOnGame = true;
	LastSchedulerError.clear();
	LOGF("TaskScheduler", "Execution initialized: context=%p main=%p executor=%p",
		reinterpret_cast<void*>(ScriptContext), MainThread, Shared::ExploitThread);

	// Queue the marker through RequestExecution so it travels exactly the same
	// path as a user script. Kept out of this function because the std::string
	// temporary would make MSVC reject the __try below (C2712).
	QueueSelfTestMarker();

	return true;
}

static bool InitializeGame()
{
	__try
	{
		return InitializeGameImpl();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Shared::IsInitializedOnGame = false;
		Shared::RobloxState = nullptr;
		Shared::ExploitThread = nullptr;
		char Message[256]{};
		snprintf(Message, sizeof(Message), "Initialization exception 0x%08lX during %s", GetExceptionCode(), InitializationPhase);
		ReportSchedulerError(Message);
		return false;
	}
}

static bool ExecuteGameImpl()
{
    // Periodic: log any object named/ClassName'd "Actor" in the game tree.
    Actor::ScanForActorScripts();
	// Partial init stages park the executor entirely: queued scripts, drawing,
	// RakNet and yields all need the fully built environment.
	if (g_PartialInit || g_InitStage < kFullInitStage || !Shared::ExploitThread)
		return true;

	std::vector<std::string> Pending;

	Diagnostics::SetStage("dispatch: queue drain");
	{
		std::lock_guard<std::mutex> lock(Shared::ExecutionRequestsMutex);

		if (Shared::ExploitThread && !Shared::ExecutionRequests.empty())
		{
			Pending.reserve(Shared::ExecutionRequests.size());

			for (std::string& Script : Shared::ExecutionRequests)
				Pending.emplace_back(std::move(Script));

			Shared::ExecutionRequests.clear();
		}
	}

	// Taking one script per step made every queued execution cost a frame, so a
	// batch of N scripts took N frames before the last one even loaded.
	for (std::string& Script : Pending)
	{
		if (!Script.empty())
		{
			Diagnostics::SetStage("dispatch: ExecuteScript");
			Execution::ExecuteScript(Shared::ExploitThread, std::move(Script));
		}
	}

	Diagnostics::SetStage("dispatch: DrawingImmediate::Render");
	DrawingImmediate::Render(Shared::ExploitThread);
	if (!Raknet::hooked)
	{
		if (uintptr_t peerJob = task_scheduler::GetJobByTypeName(ClientOffsets::RakNet::JobNameSend, 0))
			Raknet::InitializeHook(peerJob);
	}
	Diagnostics::SetStage("dispatch: Raknet::DiagScan");
	Raknet::DiagScan(Shared::ExploitThread, task_scheduler::GetJobByTypeName(ClientOffsets::RakNet::JobNameRecv, 0));
	Diagnostics::SetStage("dispatch: Raknet::ProcessCallbacks");
	Raknet::ProcessCallbacks(Shared::ExploitThread);
	Diagnostics::SetStage("dispatch: Yielding::RunYield");
	Yielding::RunYield();

	if (g_SelfTestState == 1 && Shared::ExploitThread)
	{
		++g_SelfTestSteps;

		bool Found = false;
		lua_getglobal(Shared::ExploitThread, "__glx_selftest_marker");
		if (lua_type(Shared::ExploitThread, -1) == LUA_TSTRING)
		{
			const char* Value = lua_tostring(Shared::ExploitThread, -1);
			Found = Value && strcmp(Value, "ran") == 0;
		}
		lua_pop(Shared::ExploitThread, 1);

		if (Found)
		{
			g_SelfTestState = 2;
			LOGF("TaskScheduler", "Self-test PASSED: injected script executed after %d step(s)", g_SelfTestSteps);
		}
		else if (g_SelfTestSteps >= 240)
		{
			g_SelfTestState = 3;
			LOGF("TaskScheduler", "Self-test FAILED: marker still missing after %d steps", g_SelfTestSteps);
		}
	}

	Diagnostics::SetStage("dispatch: idle");
	return true;
}

static bool ExecuteGame()
{
	__try
	{
		return ExecuteGameImpl();
	}
	__except (
		(LastDispatchFault.Code = GetExceptionCode()),
		(LastDispatchFault.Address = GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
		(LastDispatchFault.Stage = Diagnostics::GetStage()),
		(LastDispatchFault.Callback = Diagnostics::GetCallback()),
		EXCEPTION_EXECUTE_HANDLER)
	{
		// The handler keeps the exception without running C++ destructors for
		// the frames between here and the fault, so g_StepHookMutex would stay
		// locked forever. Park the executor instead: stepHook skips our work
		// while JobStepLocked is set and the client keeps running normally.
		char Message[384]{};
		const char* Callback = LastDispatchFault.Callback;
		if (Callback && Callback[0])
			snprintf(Message, sizeof(Message),
				"Script dispatch FAULT 0x%08lX at %p during '%s' inside '%s'; executor parked",
				LastDispatchFault.Code, LastDispatchFault.Address,
				LastDispatchFault.Stage ? LastDispatchFault.Stage : "?", Callback);
		else
			snprintf(Message, sizeof(Message),
				"Script dispatch FAULT 0x%08lX at %p during '%s'; executor parked",
				LastDispatchFault.Code, LastDispatchFault.Address,
				LastDispatchFault.Stage ? LastDispatchFault.Stage : "?");
		ReportSchedulerError(Message);
		Shared::JobStepLocked = true;
		return false;
	}
}

// ══════════════════════════════════════════════════════════════════════════════
// stepHook — WHSJ vtable[8] replacement
// ══════════════════════════════════════════════════════════════════════════════

static uintptr_t stepHook(uintptr_t Job, uintptr_t Stats)
{
	if (!Shared::OriginalStepFunction)
		return 0;

	if (!Shared::JobStepLocked)
	{
		std::lock_guard<std::mutex> stepLock(g_StepHookMutex);
		if (!Shared::JobStepLocked)
		{
			if (!Shared::IsInitializedOnGame)
			{
				InitializeGame();
			}
			else
			{
				ExecuteGame();
			}
		}
	}

	return Shared::OriginalStepFunction(Job, Stats);
}

// ══════════════════════════════════════════════════════════════════════════════
// TeleportHandler — WHSJ vtable hook & DataModel lifecycle
// ══════════════════════════════════════════════════════════════════════════════

void task_scheduler::TeleportHandler()
{
	constexpr size_t kJobVtableSlots = 9;

	while (true)
	{
		LOGF("TaskScheduler", "Waiting for a loaded DataModel");
		Shared::IsInitializedOnGame = false;

		uintptr_t DataModel = task_scheduler::GetDataModel();
		while (!IsGameLoaded(DataModel)) {
			Sleep(100);
			DataModel = task_scheduler::GetDataModel();
		}

		Shared::LastDataModel = DataModel;

		uintptr_t GameWHSJ = 0;
		LOGF("TaskScheduler", "Loaded DataModel=%p; waiting for hybrid script job", reinterpret_cast<void*>(Shared::LastDataModel));
		while (!(GameWHSJ = task_scheduler::GetJobByTypeName("WaitingHybridScriptsJob", Shared::LastDataModel))) {
			if (task_scheduler::GetDataModel() != Shared::LastDataModel || !IsGameLoaded(Shared::LastDataModel))
				break;
			Sleep(50);
		}

		if (!GameWHSJ)
		{
			Reset();
			Shared::LastDataModel = 0;
			continue;
		}

		Shared::LastWaitingHybridScriptsJob = GameWHSJ;
		LOGF("TaskScheduler", "Hybrid script job=%p", reinterpret_cast<void*>(GameWHSJ));

		const auto jobVFtable = *reinterpret_cast<uintptr_t**>(Shared::LastWaitingHybridScriptsJob);
		if (Shared::HybridJobVtable == nullptr || Shared::OriginalHybridJobVtable != jobVFtable)
		{
			auto* newVFtable = new uintptr_t[kJobVtableSlots]{};
			for (size_t i = 0; i < kJobVtableSlots; ++i)
			{
				if (!CheckMemory(reinterpret_cast<uintptr_t>(jobVFtable + i)))
					break;
				newVFtable[i] = jobVFtable[i];
			}

			Shared::OriginalHybridJobVtable = jobVFtable;
			Shared::OriginalStepFunction = reinterpret_cast<Shared::JobStep_t>(jobVFtable[8]);
			newVFtable[8] = reinterpret_cast<uintptr_t>(&stepHook);

			delete[] Shared::HybridJobVtable;
			Shared::HybridJobVtable = newVFtable;
		}

		Shared::JobStepLocked = false;

		*reinterpret_cast<uintptr_t**>(Shared::LastWaitingHybridScriptsJob) = static_cast<uintptr_t*>(Shared::HybridJobVtable);
		LOGF("TaskScheduler", "Installed job step callback; original=%p", Shared::OriginalStepFunction);

		while (true)
		{
			uintptr_t currentDatamodel = task_scheduler::GetDataModel();

			if (currentDatamodel != Shared::LastDataModel || !IsGameLoaded(currentDatamodel)) {
				LOGF("TaskScheduler", "DataModel changed/left (old=%p new=%p); resetting",
					reinterpret_cast<void*>(Shared::LastDataModel),
					reinterpret_cast<void*>(currentDatamodel));
				Reset();
				Shared::LastDataModel = 0;
				Shared::LastWaitingHybridScriptsJob = 0;
				break;
			}

			Sleep(10);
		}
	}
}

// ══════════════════════════════════════════════════════════════════════════════
// Execution Queue
// ══════════════════════════════════════════════════════════════════════════════

void task_scheduler::RequestExecution(std::string Script)
{
	std::lock_guard<std::mutex> lock(Shared::ExecutionRequestsMutex);
	Shared::ExecutionRequests.push_back(Script);
	LOGF("Execution", "Queued %zu bytes; pending=%zu initialized=%d", Script.size(), Shared::ExecutionRequests.size(), Shared::IsInitializedOnGame.load() ? 1 : 0);
}

// ══════════════════════════════════════════════════════════════════════════════
// GetJobByTypeName — iterates scheduler job list
// ══════════════════════════════════════════════════════════════════════════════

uintptr_t task_scheduler::GetJobByTypeName(const std::string& TypeName, uintptr_t DataModel)
{
    __try
    {
	uintptr_t task_scheduler =
		*reinterpret_cast<uintptr_t*>(
			Offsets::Scheduler::TaskScheduler
			);

	if (!task_scheduler)
		return 0;

	uintptr_t JobsStart =
		*reinterpret_cast<uintptr_t*>(
			task_scheduler + Offsets::Scheduler::JobStart
			);

	uintptr_t JobsEnd =
		*reinterpret_cast<uintptr_t*>(
			task_scheduler + Offsets::Scheduler::JobEnd
			);

	if (!JobsStart || !JobsEnd || JobsStart >= JobsEnd ||
		(JobsEnd - JobsStart) % 0x10 != 0 || (JobsEnd - JobsStart) / 0x10 > 65536)
		return 0;

	const size_t nameLen = TypeName.size();
	if (nameLen == 0 || nameLen >= 0x100)
		return 0;


	for (uintptr_t Current = JobsStart;
		Current < JobsEnd;
		Current += 0x10)
	{
		uintptr_t Job =
			*reinterpret_cast<uintptr_t*>(Current);

		if (!Job)
			continue;

		const uintptr_t nameAddress = Job + Offsets::Scheduler::JobTypeName;
		if (!CheckMemory(nameAddress + 0x18))
			continue;
		const size_t size = *reinterpret_cast<const size_t*>(nameAddress + 0x10);
		const size_t capacity = *reinterpret_cast<const size_t*>(nameAddress + 0x18);
		if (size != nameLen || capacity < size)
			continue;
		const char* candidate = capacity < 16
			? reinterpret_cast<const char*>(nameAddress)
			: *reinterpret_cast<const char* const*>(nameAddress);
		if (candidate && CheckMemory(reinterpret_cast<uintptr_t>(candidate)) &&
			CheckMemory(reinterpret_cast<uintptr_t>(candidate) + nameLen))
		{
			if (memcmp(candidate, TypeName.c_str(), nameLen) != 0)
				continue;
			if (DataModel)
			{
				const uintptr_t Context = *reinterpret_cast<uintptr_t*>(Job + ClientOffsets::jobs_offsets::scriptContext);
				if (!Context || !CheckMemory(Context + ClientOffsets::ScriptContext::DataModel))
					continue;
				const uintptr_t Owner = *reinterpret_cast<uintptr_t*>(Context + ClientOffsets::ScriptContext::DataModel);
				if (Owner != DataModel && Owner + ClientOffsets::DataModel::InstanceSubobject != DataModel)
					continue;
			}
			return Job;
		}
	}

	return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}
