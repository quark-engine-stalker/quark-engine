#pragma once

// MODS-INFO of QUARK-PANEL: what the mods' scripts cost, by mod, script and function, while the tab is shown.
//
// The whole Lua time is exact: every call the engine makes into Lua (luabind pcall/resume and the level script threads) is
// timed on its thread. Its split is sampled: LuaJIT's profile timer (luajit_hostprof.h) about every 1.5 ms and an
// allocation sample about every 256 KB the scripts allocate, both only inside those calls, record the Lua stack. Once a
// second each stack goes to the mod whose function is nearest its top: the vanilla scripts and the engine functions a
// mod calls count as that mod's. A mod is a folder of MO2 (<MO2>\mods\<mod>\..., overwrite); without MO2 the loose
// scripts of the game folder are one "gamedata" mod. A stack with no mod's function is vanilla. Nothing runs while the
// tab is closed.
struct lua_State;

namespace mods_info
{
// The VM of the game's scripts from CScriptStorage once it exists, nullptr before it is closed
ENGINE_API void set_vm(lua_State* L);

// Main thread, every frame of QUARK-PANEL (nothing of the frame job runs then): sample or not, take the second
void frame(bool sample);

constexpr u32 max_functions = 4096, max_files = 1024, max_mods = 512;

// Rates of the last seconds (exponential average over ~4 s), per second; time in samples
struct rates
{
	float self; // samples given to it: its code, the vanilla code and the engine functions it called
	float native; // ... of them inside engine functions (C)
	float vanilla; // ... of them inside vanilla scripts it called
	float main; // ... of them on the main thread
	float bytes; // bytes of the allocation samples given to it
};

struct mod_row
{
	const char* name;
	rates r;
};

struct file_row
{
	const char* path; // the script as Lua knows it
	const char* name; // its file name
	int mod; // index of its mod, -1 vanilla, -2 not known yet
	rates r;
};

struct function_row
{
	u32 file;
	int line;
	rates r;
};

struct summary
{
	bool running; // the sampler runs
	bool refused; // jit.profile or jit.allocprof of a script holds LuaJIT's timer
	bool full; // a table is full: what does not fit is not counted
	float lua_main_ms, lua_worker_ms; // exact Lua time a second on the main thread and the others, ms
	float calls; // calls of the engine into Lua a second
	float samples, gc, jit, bytes; // time samples a second, of them in the collector and in the JIT compiler; bytes a second
	rates mods, vanilla; // all the mods, the stacks without a mod
	u32 mods_count, files, functions; // seen since the tab was opened
	u32 generation; // new tables: every opening of the tab
	u32 refreshes; // seconds taken: the numbers changed
};

const summary& totals();
const mod_row& mod(u32 index);
const file_row& file(u32 index);
const function_row& function(u32 index);
// A function's name read from its line in the script, "line N" for an anonymous one; read once per script
const char* function_name(u32 function_index);
} // namespace mods_info
