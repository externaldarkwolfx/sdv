#pragma once

// Lightweight crash-location breadcrumbs shared between the module's RBX layer
// and the vendored Luau VM.
//
// The scheduler's step hook runs our code inside a bare `__try/__except`, so an
// access violation (or a C++ exception escaping a client C function we call)
// is swallowed without a debugger ever seeing it. Each thread keeps its own
// "current stage" string; the handlers log it so a fault can be attributed to
// the exact call in flight.
namespace Diagnostics
{
    inline thread_local const char* Stage = "idle";

    // Set only while control is inside an embedder (Roblox) callback that the
    // vendored VM invoked. Empty the rest of the time.
    inline thread_local const char* Callback = "";

    inline void SetStage(const char* stage)
    {
        Stage = stage;
    }

    inline const char* GetStage()
    {
        return Stage;
    }

    inline void SetCallback(const char* callback)
    {
        Callback = callback ? callback : "";
    }

    inline const char* GetCallback()
    {
        return Callback;
    }
}
