#include <Utilitys/Globals.hpp>
#include <RBX/diagnostics.hpp>
#include <RBX/execution/execution.hpp>
#include <Utilitys/Log.hpp>
#include <RBX/task_scheduler/task_scheduler.hpp>
#include <ltable.h>
#include <lstring.h>
#include <cstring>
#include <cstdio>

class BytecodeEncoder : public Luau::BytecodeEncoder
{
	inline void encode(uint32_t* data, size_t count) override
	{
		for (auto i = 0; i < count;)
		{
			uint8_t Opcode = LUAU_INSN_OP(data[i]);
			const auto LookupTable = reinterpret_cast<BYTE*>(Offsets::OpcodeLookupTable);
			uint8_t FinalOpcode = static_cast<uint8_t>(Opcode * ClientOffsets::Bytecode::OpcodeWireMultiplier);
			FinalOpcode = LookupTable[FinalOpcode];

			data[i] = (FinalOpcode) | (data[i] & ~0xFF);
			i += Luau::getOpLength(static_cast<LuauOpcode>(Opcode));
		}
	}
};

std::string Execution::CompileScript(std::string Source)
{
	auto BytecodeEncoding = BytecodeEncoder();
	static const char* CommonGlobals[] = { "Game", "Workspace", "game", "plugin", "script", "shared", "workspace", "_G", "_ENV", nullptr };

	Luau::CompileOptions Options;
	Options.debugLevel = (Source.size() >= 65536) ? 1 : 2;
	Options.optimizationLevel = 1;
	Options.mutableGlobals = CommonGlobals;
	Options.vectorLib = "Vector3";
	Options.vectorCtor = "new";
	Options.vectorType = "Vector3";

	return Luau::compile(Source, Options, {}, &BytecodeEncoding);
}

namespace
{
	const char* TypeName(int Type)
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

	// Push `Name` straight out of a raw global table. The direct rawget is
	// tried first; when it misses, a linear lua_next scan compares the string
	// contents, so a hash/atom difference between our VM and the client cannot
	// hide a global. Always leaves exactly one value on the stack.
	void PushRawGlobal(lua_State* L, LuaTable* Table, const char* Name)
	{
		if (!Table)
		{
			lua_pushnil(L);
			return;
		}

		luaC_threadbarrier(L);
		lua_checkstack(L, 4);
		sethvalue(L, L->top, Table);
		L->top++;

		lua_pushstring(L, Name);
		if (lua_rawget(L, -2) != LUA_TNIL)
		{
			lua_remove(L, -2);
			return;
		}
		lua_pop(L, 1);

		const size_t NameLen = strlen(Name);
		lua_pushnil(L);
		while (lua_next(L, -2) != 0)
		{
			if (lua_type(L, -2) == LUA_TSTRING)
			{
				size_t KeyLen = 0;
				const char* Key = lua_tolstring(L, -2, &KeyLen);
				if (Key && KeyLen == NameLen && memcmp(Key, Name, NameLen) == 0)
				{
					lua_remove(L, -3); // table
					lua_remove(L, -2); // key
					return;
				}
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1); // table
		lua_pushnil(L);
	}

	// Snapshot of everything that decides whether `task` is visible to a script
	// thread. POD only: the reader touches client-owned pointers and must not
	// need C++ unwinding.
	struct DispatchProbe
	{
		void* ExecutorGlobals = nullptr;
		void* ClientGlobals = nullptr;
		void* SavedGlobals = nullptr;
		void* RobloxStateGlobals = nullptr;
		void* ProxyMetatable = nullptr;
		void* ProxyIndex = nullptr;
		int ProxyTaskType = LUA_TNONE;
		void* ProxyTask = nullptr;
		int ClientTaskType = LUA_TNONE;
		void* ClientTask = nullptr;
		bool Faulted = false;
		unsigned long FaultCode = 0;
	};

	bool RunDispatchProbe(lua_State* L, DispatchProbe* Out)
	{
		__try
		{
			LuaTable* Executor = L ? L->gt : nullptr;
			LuaTable* Client = Shared::RobloxState ? Shared::RobloxState->gt : nullptr;

			Out->ExecutorGlobals = Executor;
			Out->ClientGlobals = Client;
			Out->SavedGlobals = Shared::ClientGlobals;
			Out->RobloxStateGlobals = Client;

			TString* TaskKey = luaS_new(L, "task");
			if (Executor)
			{
				Out->ProxyMetatable = Executor->metatable;
				if (Executor->metatable)
				{
					TString* IndexKey = luaS_new(L, "__index");
					const TValue* Index = luaH_getstr(Executor->metatable, IndexKey);
					if (Index && ttistable(Index))
						Out->ProxyIndex = hvalue(Index);
				}
				const TValue* Task = luaH_getstr(Executor, TaskKey);
				Out->ProxyTaskType = Task ? ttype(Task) : LUA_TNIL;
				if (Task && ttistable(Task))
					Out->ProxyTask = hvalue(Task);
			}
			if (Client)
			{
				const TValue* Task = luaH_getstr(Client, TaskKey);
				Out->ClientTaskType = Task ? ttype(Task) : LUA_TNIL;
				if (Task && ttistable(Task))
					Out->ClientTask = hvalue(Task);
			}
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

	// Everything the guarded dispatch touches. Plain data on purpose: the
	// function holding the __try below must not require C++ object unwinding
	// (MSVC rejects that combination with C2712).
	struct DispatchState
	{
		lua_State* L;
		lua_State* Thread;
		const char* Bytecode;
		size_t BytecodeSize;
		Closure* Chunk;
		int LoadStatus;
		int CallStatus;
		int TaskType;
		int DeferType;
		void* TaskPointer;
		void* DeferPointer;
		// result of the raw client-globals fallback, when the proxy chain missed
		int FallbackType;
		void* FallbackPointer;
		bool DeferIsC;
		void* DeferFunction;
		// 0 = proxy lookup failed, 1 = registry cache, 2 = proxy lookup
		int DeferSource;
		// 0 = never reached, 1 = dispatched via task.defer, 2 = called inline
		int Dispatched;
		bool Faulted;
		unsigned long FaultCode;
		void* FaultAddress;
		const char* FaultStage;
	};

	// Runs the Lua-touching half of ExecuteScript under SEH. This catches
	// access violations from the vendored VM as well as C++ exceptions thrown
	// by client C functions we call (task.defer is client code).
	bool RunDispatchGuard(DispatchState* State)
	{
		__try
		{
			Diagnostics::SetStage("execution: luaL_sandboxthread");
			luaL_sandboxthread(State->Thread);

			Diagnostics::SetStage("execution: SetThreadCapabilities");
			task_scheduler::SetThreadCapabilities(State->Thread, 8, MaxCapabilities);

			Diagnostics::SetStage("execution: luau_load");
			State->LoadStatus = luau_load(State->Thread, "@Script", State->Bytecode, State->BytecodeSize, NULL);
			if (State->LoadStatus != LUA_OK)
				return true;

			State->Chunk = clvalue(luaA_toobject(State->Thread, -1));

			Diagnostics::SetStage("execution: SetProtoCapabilities");
			if (State->Chunk)
				task_scheduler::SetProtoCapabilities(State->Chunk->l.p, &MaxCapabilities);

			Diagnostics::SetStage("execution: resolve task.defer");
			if (Shared::ClientTaskDeferRef != LUA_NOREF)
			{
				// Preferred path: the reference was taken from the client's
				// globals before sandboxing, so it cannot be missing here.
				lua_rawgeti(State->Thread, LUA_REGISTRYINDEX, Shared::ClientTaskDeferRef);
				State->DeferSource = 1;
			}
			else
			{
				lua_getglobal(State->Thread, "task");
				State->TaskType = lua_type(State->Thread, -1);
				State->TaskPointer = const_cast<void*>(lua_topointer(State->Thread, -1));

				if (State->TaskType != LUA_TTABLE)
				{
					// The sandbox proxy chain missed. Go straight to the
					// client's current global table (its pointer can change
					// between game phases, so refresh it first).
					lua_pop(State->Thread, 1);
					if (Shared::RobloxState && Shared::RobloxState->gt)
						Shared::ClientGlobals = Shared::RobloxState->gt;

					Diagnostics::SetStage("execution: raw client task lookup");
					PushRawGlobal(State->Thread, Shared::ClientGlobals, "task");
					State->FallbackType = lua_type(State->Thread, -1);
					State->FallbackPointer = const_cast<void*>(lua_topointer(State->Thread, -1));
				}

				if (lua_type(State->Thread, -1) == LUA_TTABLE)
				{
					Diagnostics::SetStage("execution: lua_getfield(task, defer)");
					lua_getfield(State->Thread, -1, "defer");

					// Cache the function the first time so later dispatches
					// never have to walk any chain again.
					if (Shared::ClientTaskDeferRef == LUA_NOREF && lua_type(State->Thread, -1) == LUA_TFUNCTION)
					{
						Shared::ClientTaskRef = lua_ref(State->Thread, -2);
						Shared::ClientTaskDeferRef = lua_ref(State->Thread, -1);
					}

					lua_remove(State->Thread, -2);
					State->DeferSource = 2;
				}
				else
				{
					// task is nil here: never index it (that raises). Fall back
					// to inline execution below.
					lua_pop(State->Thread, 1);
					lua_pushnil(State->Thread);
					State->DeferSource = 0;
				}
			}

			State->DeferType = lua_type(State->Thread, -1);
			State->DeferPointer = const_cast<void*>(lua_topointer(State->Thread, -1));

			if (State->DeferType == LUA_TFUNCTION)
			{
				Closure* DeferClosure = clvalue(luaA_toobject(State->Thread, -1));
				State->DeferIsC = DeferClosure && DeferClosure->isC;
				State->DeferFunction = (DeferClosure && DeferClosure->isC)
					? reinterpret_cast<void*>(DeferClosure->c.f)
					: nullptr;

				// chunk is below the function; swap for pcall(fn, chunk)
				lua_insert(State->Thread, -2);

				Diagnostics::SetStage("execution: lua_pcall(task.defer)");
				State->CallStatus = lua_pcall(State->Thread, 1, 0, 0);
				State->Dispatched = 1;
			}
			else
			{
				// no usable task.defer in this state - run the chunk inline so
				// the script still executes instead of silently doing nothing
				lua_pop(State->Thread, 1);

				Diagnostics::SetStage("execution: lua_pcall(direct)");
				State->CallStatus = lua_pcall(State->Thread, 0, 0, 0);
				State->Dispatched = 2;
			}

			Diagnostics::SetStage("execution: dispatch done");
			return true;
		}
		__except (
			(State->FaultCode = GetExceptionCode()),
			(State->FaultAddress = GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
			(State->FaultStage = Diagnostics::Stage),
			(State->Faulted = true),
			EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Renders the head of a script as printable text. Small snippets are logged
	// verbatim so a compile failure can be traced to the exact bytes that
	// reached the compiler; escape anything that would break the log line.
	std::string PreviewSource(const std::string& Source, size_t Limit = 200)
	{
		std::string Out;
		Out.reserve(Limit + 8);
		for (size_t i = 0; i < Source.size() && Out.size() < Limit; ++i)
		{
			const unsigned char Ch = static_cast<unsigned char>(Source[i]);
			if (Ch == '\n')
				Out += "\\n";
			else if (Ch == '\r')
				Out += "\\r";
			else if (Ch == '\t')
				Out += "\\t";
			else if (Ch < 0x20 || Ch >= 0x7F)
			{
				char Buf[8];
				snprintf(Buf, sizeof(Buf), "\\x%02X", Ch);
				Out += Buf;
			}
			else
				Out += static_cast<char>(Ch);
		}
		if (Source.size() > Limit)
			Out += "...";
		return Out;
	}
}

void Execution::ExecuteScript(lua_State* L, std::string Script)
{
	if (!L || Script.empty())
		return;

	int OriginalTop = lua_gettop(L);
	if (Script.size() <= 256)
		LOGF("Execution", "Compiling %zu bytes: \"%s\"", Script.size(), PreviewSource(Script).c_str());
	else
		LOGF("Execution", "Compiling %zu bytes", Script.size());

	{
		DispatchProbe Probe{};
		if (RunDispatchProbe(L, &Probe))
			LOGF("Execution",
				"Globals: executor=%p client=%p saved=%p mt=%p index=%p proxyTask=%s@%p clientTask=%s@%p",
				Probe.ExecutorGlobals, Probe.ClientGlobals, Probe.SavedGlobals,
				Probe.ProxyMetatable, Probe.ProxyIndex,
				TypeName(Probe.ProxyTaskType), Probe.ProxyTask,
				TypeName(Probe.ClientTaskType), Probe.ClientTask);
		else
			LOGF("Execution", "Globals probe faulted 0x%08lX", Probe.FaultCode);
	}

	DispatchState State{};
	State.L = L;
	State.LoadStatus = LUA_ERRRUN;

	State.Thread = lua_newthread(L);
	if (!State.Thread)
	{
		LOGF("Execution", "lua_newthread returned null");
		lua_settop(L, OriginalTop);
		return;
	}

	LOGF("Execution", "New thread=%p gt=%p global=%p userdata=%p status=%d",
		reinterpret_cast<void*>(State.Thread),
		reinterpret_cast<void*>(State.Thread->gt),
		reinterpret_cast<void*>(State.Thread->global),
		reinterpret_cast<void*>(State.Thread->userdata),
		static_cast<int>(State.Thread->status));

	std::string Bytecode = Execution::CompileScript(Script);
	if (!Bytecode.empty() && Bytecode[0] == '\0')
	{
		// The compiler encodes failure as a leading zero byte followed by the
		// message; luau_load would only echo it back as a bogus "load failed".
		LOGF("Execution", "Compile error: %s", Bytecode.c_str() + 1);
		LOGF("Execution", "Failing source (%zu bytes): \"%s\"", Script.size(), PreviewSource(Script).c_str());
		lua_settop(L, OriginalTop);
		return;
	}
	State.Bytecode = Bytecode.c_str();

	State.BytecodeSize = Bytecode.length();

	if (!RunDispatchGuard(&State))
	{
		LOGF("Execution",
			"FAULT 0x%08lX at %p during '%s'%s%s (script=%zu B, bytecode=%zu B, thread=%p, source=%d task=%d@%p fallback=%d@%p defer=%d@%p)",
			State.FaultCode, State.FaultAddress, State.FaultStage ? State.FaultStage : "?",
			Diagnostics::GetCallback()[0] ? " inside '" : "",
			Diagnostics::GetCallback()[0] ? Diagnostics::GetCallback() : "",
			Script.size(), Bytecode.size(), reinterpret_cast<void*>(State.Thread),
			State.DeferSource, State.TaskType, State.TaskPointer,
			State.FallbackType, State.FallbackPointer, State.DeferType, State.DeferPointer);
		if (Diagnostics::GetCallback()[0])
			LOGF("Execution", "Faulting embedder callback: %s", Diagnostics::GetCallback());
		lua_settop(L, OriginalTop);
		return;
	}

	if (State.LoadStatus != LUA_OK)
	{
		const char* Error = lua_tostring(State.Thread, -1);
		LOGF("Execution", "Bytecode load failed (%d): %s", State.LoadStatus, Error ? Error : "non-string error");
		LOGF("Execution", "Failing source (%zu bytes): \"%s\"", Script.size(), PreviewSource(Script).c_str());
		lua_pop(State.Thread, 1);
		lua_settop(L, OriginalTop);
		return;
	}

	LOGF("Execution", "Loaded %zu byte bytecode (chunk=%p source=%d task=%d@%p fallback=%d@%p defer=%d@%p isC=%d fn=%p)",
		Bytecode.size(), reinterpret_cast<void*>(State.Chunk), State.DeferSource,
		State.TaskType, State.TaskPointer, State.FallbackType, State.FallbackPointer,
		State.DeferType, State.DeferPointer, State.DeferIsC ? 1 : 0, State.DeferFunction);

	LOGF("Execution", "Thread before call: ci=%p base_ci=%p nCcalls=%u baseCcalls=%u stacksize=%d top-base=%td",
		reinterpret_cast<void*>(State.Thread->ci),
		reinterpret_cast<void*>(State.Thread->base_ci),
		static_cast<unsigned>(State.Thread->nCcalls),
		static_cast<unsigned>(State.Thread->baseCcalls),
		static_cast<int>(State.Thread->stacksize),
		State.Thread->top - State.Thread->base);

	if (State.CallStatus != LUA_OK)
	{
		const char* Error = lua_tostring(State.Thread, -1);
		LOGF("Execution", "%s failed (%d): %s",
			State.Dispatched == 1 ? "task.defer" : "Direct execution",
			State.CallStatus, Error ? Error : "non-string error");
		lua_pop(State.Thread, 1);
	}
	else
	{
		LOGF("Execution", "%s", State.Dispatched == 1
			? "Dispatched through task.defer"
			: "Direct execution completed");
	}

	lua_settop(L, OriginalTop);
}
