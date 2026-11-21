#include "stdafx.h"
#include "mods_info.h"
#include "device.h"
#include "../xrCore/mt_tasks.h"
extern "C" {
#include <lua.h>
}
#include <luajit_hostprof.h>
#include <luabind/detail/pcall.hpp>
#include <intrin.h>

namespace mods_info
{
namespace
{
constexpr u32 max_stacks = 8192;
constexpr u32 function_slot_count = 8192, file_slot_count = 2048, stack_slot_count = 16384; // at most half full
constexpr int walk_depth = 64; // stack levels walked for a sample
constexpr u32 stack_depth = 24; // Lua functions kept from the top: the nearest mod is well within them
constexpr int sample_interval_ms = 1; // LuaJIT's timer: random sleeps of at least this
constexpr unsigned alloc_mean = 256 * 1024;
constexpr u32 resolves_per_frame = 4; // scripts whose file Windows is asked about in one frame

struct counts
{
	u64 samples, native, main, bytes;
};

// Filled by the sample hook on the thread that runs Lua (one at a time); read by the panel on the main thread when
// nothing of the frame job runs. The fields marked main are the panel's own.
struct function_entry
{
	const char* source; // Lua's chunk name: the same pointer for every function of a script
	int line;
	u32 file;
	char name[64]; // main
	bool named; // main
};
struct file_entry
{
	const char* source;
	char path[MAX_PATH];
	const char* name;
	int mod; // main: index of its mod, -1 vanilla, -2 not asked yet
};
struct stack_entry // the Lua functions of a sampled stack, top first
{
	u32 depth;
	u16 functions[stack_depth];
	counts c;
};
function_entry functions[max_functions];
file_entry files[max_files];
stack_entry stacks[max_stacks];
u32 function_count = 0, file_count = 0, stack_count = 0;
u16 function_slots[function_slot_count]; // index + 1, 0: free
u16 file_slots[file_slot_count];
u16 stack_slots[stack_slot_count];
struct
{
	u64 samples, gc, jit, bytes, lost;
} sampled;

// The calls the engine makes into the game's VM, timed on their thread. Lua runs on one thread at a time.
volatile long gate = 0; // nonzero inside such a call, a new value for each: LuaJIT samples only then
long last_call = 0;
thread_local u32 call_depth = 0;
thread_local u64 call_start = 0;
u64 lua_ticks[2] = {}; // TSC ticks on the main thread, on the others
u64 lua_calls = 0;

void call_enter(lua_State* L)
{
	if (L == Device.ShaderScriptsVM || call_depth++)
		return;
	last_call = last_call == LONG_MAX ? 1 : last_call + 1;
	gate = last_call;
	call_start = __rdtsc();
}

void call_leave(lua_State* L)
{
	if (L == Device.ShaderScriptsVM || --call_depth)
		return;
	gate = 0;
	lua_ticks[mt_tasks::current_thread_slot() == 0 ? 0 : 1] += __rdtsc() - call_start;
	++lua_calls;
}

const luabind::detail::lua_call_observer observer = {&call_enter, &call_leave};

u32 hash(const void* source, int line) { return u32(uintptr_t(source) >> 3) * 0x9E3779B1u ^ u32(line) * 0x85EBCA6Bu; }

int file_index(const char* source)
{
	u32 slot = hash(source, 0) & (file_slot_count - 1);
	for (u32 probe = 0; probe < file_slot_count; ++probe, slot = (slot + 1) & (file_slot_count - 1))
	{
		const u32 at = file_slots[slot];
		if (at)
		{
			if (files[at - 1].source == source)
				return int(at - 1);
			continue;
		}
		if (file_count == max_files)
			return -1;
		file_entry& f = files[file_count];
		f.source = source;
		strncpy_s(f.path, source[0] == '@' ? source + 1 : source, _TRUNCATE); // a loadstring chunk is named by its code
		const char* slash = _max(strrchr(f.path, '\\'), strrchr(f.path, '/'));
		f.name = slash ? slash + 1 : f.path;
		f.mod = -2;
		file_slots[slot] = u16(++file_count);
		return int(file_count - 1);
	}
	return -1;
}

int function_index(const char* source, int line)
{
	u32 slot = hash(source, line) & (function_slot_count - 1);
	for (u32 probe = 0; probe < function_slot_count; ++probe, slot = (slot + 1) & (function_slot_count - 1))
	{
		const u32 at = function_slots[slot];
		if (at)
		{
			const function_entry& f = functions[at - 1];
			if (f.source == source && f.line == line)
				return int(at - 1);
			continue;
		}
		if (function_count == max_functions)
			return -1;
		const int file = file_index(source);
		if (file < 0)
			return -1;
		function_entry& f = functions[function_count];
		f = {};
		f.source = source;
		f.line = line;
		f.file = u32(file);
		function_slots[slot] = u16(++function_count);
		return int(function_count - 1);
	}
	return -1;
}

stack_entry* stack_of(const u16* list, u32 depth)
{
	u32 h = 2166136261u;
	for (u32 index = 0; index < depth; ++index)
		h = (h ^ list[index]) * 16777619u;
	u32 slot = h & (stack_slot_count - 1);
	for (u32 probe = 0; probe < stack_slot_count; ++probe, slot = (slot + 1) & (stack_slot_count - 1))
	{
		const u32 at = stack_slots[slot];
		if (at)
		{
			stack_entry& s = stacks[at - 1];
			if (s.depth == depth && !memcmp(s.functions, list, depth * sizeof(u16)))
				return &s;
			continue;
		}
		if (stack_count == max_stacks)
			return nullptr;
		stack_entry& s = stacks[stack_count];
		s = {};
		s.depth = depth;
		memcpy(s.functions, list, depth * sizeof(u16));
		stack_slots[slot] = u16(++stack_count);
		return &s;
	}
	return nullptr;
}

// LuaJIT's hook, on the thread that runs Lua, inside a call of the engine: the stack of one or more samples
void sample(void*, lua_State* L, int time_samples, unsigned long long bytes, int vmstate)
{
	u64 samples = u64(time_samples);
	sampled.samples += samples;
	sampled.bytes += bytes;
	if (vmstate == 'G') // a step of the collector inside the call: the garbage of every script, not this one's work
	{
		sampled.gc += samples;
		samples = 0;
	}
	else if (vmstate == 'J')
	{
		sampled.jit += samples;
		samples = 0;
	}
	if (!samples && !bytes)
		return;

	u16 list[stack_depth];
	u32 depth = 0;
	lua_Debug ar;
	for (int level = 0; level < walk_depth && depth < stack_depth && lua_getstack(L, level, &ar); ++level)
	{
		// C functions (the engine's, the builtins) belong to the Lua function that called them
		if (!lua_getinfo(L, "S", &ar) || !ar.source || ar.what[0] == 'C')
			continue;
		const int f = function_index(ar.source, ar.linedefined);
		if (f >= 0)
			list[depth++] = u16(f);
	}
	stack_entry* s = depth ? stack_of(list, depth) : nullptr;
	if (!s)
	{
		sampled.lost += samples;
		return;
	}
	s->c.samples += samples;
	s->c.native += vmstate == 'C' ? samples : 0;
	s->c.main += mt_tasks::current_thread_slot() == 0 ? samples : 0;
	s->c.bytes += bytes;
}

// The panel's side
lua_State* vm = nullptr;
bool running = false;
summary shown = {};
char mod_names[max_mods][80];
mod_row mod_rows[max_mods];
file_row file_rows[max_files];
function_row function_rows[max_functions];
counts previous_stacks[max_stacks];
u64 previous_ticks[2], previous_calls, second_start;
decltype(sampled) previous_sampled;
// One second given out: to the mod functions nearest the top of their stacks, the rest to vanilla
struct given
{
	u64 samples, native, vanilla, main, bytes;
};
given function_seconds[max_functions], vanilla_second;

void reset()
{
	function_count = file_count = stack_count = 0;
	ZeroMemory(function_slots, sizeof(function_slots));
	ZeroMemory(file_slots, sizeof(file_slots));
	ZeroMemory(stack_slots, sizeof(stack_slots));
	sampled = {};
	previous_sampled = {};
	ZeroMemory(previous_stacks, sizeof(previous_stacks));
	ZeroMemory(mod_rows, sizeof(mod_rows));
	ZeroMemory(file_rows, sizeof(file_rows));
	ZeroMemory(function_rows, sizeof(function_rows));
	const u32 generation = shown.generation;
	shown = {};
	shown.generation = generation;
}

void stop()
{
	if (!running)
		return;
	luaJIT_hostprof_stop(vm);
	luabind::detail::set_lua_call_observer(nullptr);
	gate = 0;
	running = false;
	shown.running = false;
}

// Every start begins with empty tables: the scripts since the tab was opened, never more than the tables hold
void start()
{
	reset();
	++shown.generation;
	luabind::detail::set_lua_call_observer(&observer);
	shown.refused = !luaJIT_hostprof_start(vm, sample_interval_ms, alloc_mean, &gate, &sample, nullptr);
	running = true;
	shown.running = true;
	previous_ticks[0] = lua_ticks[0];
	previous_ticks[1] = lua_ticks[1];
	previous_calls = lua_calls;
	second_start = CPU::QPC();
}

int mod_index(const char* name)
{
	for (u32 index = 0; index < shown.mods_count; ++index)
		if (!xr_strcmp(mod_names[index], name))
			return int(index);
	if (shown.mods_count == max_mods)
		return -1;
	strncpy_s(mod_names[shown.mods_count], name, _TRUNCATE);
	mod_rows[shown.mods_count].name = mod_names[shown.mods_count];
	return int(shown.mods_count++);
}

// The mod a script comes from: Windows gives the file behind its path, under MO2 the mod's own copy
int resolve_mod(const file_entry& f)
{
	char normal[MAX_PATH], full[MAX_PATH];
	xr_strcpy(normal, f.path);
	for (char* c = normal; *c; ++c)
		if (*c == '/')
			*c = '\\'; // MO2 does not know a path with both kinds of separators
	if (!GetFullPathNameA(normal, MAX_PATH, full, nullptr)) // a relative chunk name is relative to the game's folder
		return -1;
	const HANDLE handle = CreateFileA(full, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE) // packed in the game's archives, or not a file at all: vanilla
		return -1;
	wchar_t real[2048];
	const DWORD length = GetFinalPathNameByHandleW(handle, real, DWORD(_countof(real)), FILE_NAME_NORMALIZED);
	CloseHandle(handle);
	char path[4096], lower[4096];
	if (!length || length >= _countof(real) ||
		!WideCharToMultiByte(CP_UTF8, 0, real, -1, path, sizeof(path), nullptr, nullptr))
		return -1;
	xr_strcpy(lower, path);
	_strlwr_s(lower);
	// ...\mods\<mod>\gamedata\..., ...\overwrite\gamedata\..., <game>\gamedata\...
	const char* gamedata = strstr(lower, "\\gamedata\\");
	if (!gamedata)
		return -1;
	const size_t folder_end = size_t(gamedata - lower);
	size_t folder_start = folder_end;
	while (folder_start && lower[folder_start - 1] != '\\')
		--folder_start;
	const size_t folder = folder_end - folder_start;
	const bool in_mods = folder_start >= 6 && !strncmp(lower + folder_start - 6, "\\mods\\", 6);
	if (in_mods || (folder == 9 && !strncmp(lower + folder_start, "overwrite", 9)))
	{
		char name[80];
		strncpy_s(name, path + folder_start, _min(folder, sizeof(name) - 1));
		return mod_index(name);
	}
	// the game folder's own: vanilla under MO2, the scripts a player put there without it
	return GetModuleHandleA("usvfs_x64.dll") ? -1 : mod_index("gamedata");
}

void resolve_some()
{
	u32 budget = resolves_per_frame;
	for (u32 index = 0; index < file_count && budget; ++index)
		if (files[index].mod == -2)
		{
			files[index].mod = resolve_mod(files[index]);
			--budget;
		}
}

void average(float& rate, u64 value, float seconds, float k) { rate += (float(value) / seconds - rate) * k; }

void average(rates& r, const given& g, float seconds, float k)
{
	average(r.self, g.samples, seconds, k);
	average(r.native, g.native, seconds, k);
	average(r.vanilla, g.vanilla, seconds, k);
	average(r.main, g.main, seconds, k);
	average(r.bytes, g.bytes, seconds, k);
}

void add(rates& to, const rates& r)
{
	to.self += r.self;
	to.native += r.native;
	to.vanilla += r.vanilla;
	to.main += r.main;
	to.bytes += r.bytes;
}

void take_second()
{
	const u64 now = CPU::QPC();
	if (now - second_start < CPU::qpc_freq)
		return;
	const float seconds = float(double(now - second_start) / double(CPU::qpc_freq));
	second_start = now;
	const float k = 1.f - expf(-seconds / 4.f); // ~4 s average: steady numbers, still a live reading

	const double ms_per_tick = 1000.0 / double(_max(CPU::clk_per_second, u64(1)));
	for (int thread = 0; thread < 2; ++thread)
	{
		float& ms = thread ? shown.lua_worker_ms : shown.lua_main_ms;
		ms += (float(double(lua_ticks[thread] - previous_ticks[thread]) * ms_per_tick) / seconds - ms) * k;
		previous_ticks[thread] = lua_ticks[thread];
	}
	average(shown.calls, lua_calls - previous_calls, seconds, k);
	previous_calls = lua_calls;
	average(shown.samples, sampled.samples - previous_sampled.samples, seconds, k);
	average(shown.gc, sampled.gc - previous_sampled.gc, seconds, k);
	average(shown.jit, sampled.jit - previous_sampled.jit, seconds, k);
	average(shown.bytes, sampled.bytes - previous_sampled.bytes, seconds, k);
	previous_sampled = sampled;

	// Each stack's new samples go to the mod function nearest its top; a stack whose scripts are not all known yet
	// waits for them (its counters are taken later, nothing is lost)
	ZeroMemory(function_seconds, sizeof(function_seconds[0]) * function_count);
	vanilla_second = {};
	for (u32 index = 0; index < stack_count; ++index)
	{
		const stack_entry& s = stacks[index];
		counts& before = previous_stacks[index];
		if (s.c.samples == before.samples && s.c.bytes == before.bytes)
			continue;
		int owner = -1;
		u32 at = 0;
		bool unknown = false;
		for (; at < s.depth; ++at)
		{
			const int mod = files[functions[s.functions[at]].file].mod;
			unknown = mod == -2;
			if (unknown || mod >= 0)
			{
				owner = unknown ? -1 : s.functions[at];
				break;
			}
		}
		if (unknown)
			continue;
		const u64 samples = s.c.samples - before.samples, native = s.c.native - before.native;
		given& to = owner >= 0 ? function_seconds[owner] : vanilla_second;
		to.samples += samples;
		to.native += native;
		to.vanilla += owner >= 0 && at > 0 ? samples - native : 0; // in the vanilla scripts it called
		to.main += s.c.main - before.main;
		to.bytes += s.c.bytes - before.bytes;
		before = s.c;
	}

	for (u32 index = 0; index < shown.mods_count; ++index)
		mod_rows[index].r = {};
	for (u32 index = 0; index < file_count; ++index)
	{
		file_rows[index].path = files[index].path;
		file_rows[index].name = files[index].name;
		file_rows[index].mod = files[index].mod;
		file_rows[index].r = {};
	}
	shown.mods = {};
	for (u32 index = 0; index < function_count; ++index)
	{
		function_row& row = function_rows[index];
		row.file = functions[index].file;
		row.line = functions[index].line;
		average(row.r, function_seconds[index], seconds, k);
		file_row& file = file_rows[row.file];
		add(file.r, row.r);
		if (file.mod >= 0)
		{
			add(mod_rows[file.mod].r, row.r);
			add(shown.mods, row.r);
		}
	}
	average(shown.vanilla, vanilla_second, seconds, k);
	shown.files = file_count;
	shown.functions = function_count;
	shown.full = function_count == max_functions || file_count == max_files || stack_count == max_stacks ||
		shown.mods_count == max_mods;
	++shown.refreshes;
	if (!shown.refused && !luaJIT_hostprof_active()) // a script started jit.profile, which stopped ours
		shown.refused = true;
}

// "function a.b:c(", "local function c(", "a.b = function(", "local c = function(": the name, false otherwise
bool name_in_line(const char* line, char* out, size_t size)
{
	auto identifier = [](char c) { return isalnum(u8(c)) || c == '_' || c == '.' || c == ':' || c == '[' || c == ']' ||
		c == '"' || c == '\''; };
	while (*line == ' ' || *line == '\t')
		++line;
	if (!strncmp(line, "local ", 6))
		line += 6;
	while (*line == ' ' || *line == '\t')
		++line;
	if (!strncmp(line, "function", 8) && (line[8] == ' ' || line[8] == '\t'))
	{
		line += 9;
		while (*line == ' ' || *line == '\t')
			++line;
		size_t n = 0;
		while (identifier(line[n]) && n + 1 < size)
			++n;
		if (!n || line[n] != '(' && line[n] != ' ')
			return false;
		strncpy_s(out, size, line, n);
		return true;
	}
	size_t n = 0;
	while (identifier(line[n]) && n + 1 < size)
		++n;
	if (!n)
		return false;
	const char* rest = line + n;
	while (*rest == ' ' || *rest == '\t')
		++rest;
	if (*rest != '=' || rest[1] == '=')
		return false;
	++rest;
	while (*rest == ' ' || *rest == '\t')
		++rest;
	if (strncmp(rest, "function", 8))
		return false;
	strncpy_s(out, size, line, n);
	return true;
}

void name_functions_of(u32 file)
{
	IReader* reader = FS.exist(files[file].path) ? FS.r_open(files[file].path) : nullptr;
	const char* text = reader ? static_cast<const char*>(reader->pointer()) : nullptr;
	const char* end = text ? text + reader->length() : nullptr;
	for (u32 index = 0; index < function_count; ++index)
	{
		function_entry& f = functions[index];
		if (f.file != file || f.named)
			continue;
		f.named = true;
		bool found = false;
		if (text && f.line > 0)
		{
			const char* line = text;
			for (int number = 1; number < f.line && line < end; ++number)
			{
				line = static_cast<const char*>(memchr(line, '\n', size_t(end - line)));
				line = line ? line + 1 : end;
			}
			if (line < end)
			{
				const char* eol = static_cast<const char*>(memchr(line, '\n', size_t(end - line)));
				string256 copy;
				strncpy_s(copy, line, _min(size_t((eol ? eol : end) - line), sizeof(copy) - 1));
				found = name_in_line(copy, f.name, sizeof(f.name));
			}
		}
		if (!found)
			xr_strcpy(f.name, f.line > 0 ? "?" : "main chunk"); // an anonymous function: its line says where
	}
	if (reader)
		FS.r_close(reader);
}
} // namespace

void set_vm(lua_State* L)
{
	stop();
	reset();
	vm = L;
}

void frame(bool sample)
{
	if (sample && vm && !running)
		start();
	else if (!sample)
		stop();
	if (!running)
		return;
	resolve_some();
	take_second();
}

const summary& totals() { return shown; }
const mod_row& mod(u32 index) { return mod_rows[index]; }
const file_row& file(u32 index) { return file_rows[index]; }
const function_row& function(u32 index) { return function_rows[index]; }

const char* function_name(u32 index)
{
	function_entry& f = functions[index];
	if (!f.named)
		name_functions_of(f.file);
	return f.name;
}
} // namespace mods_info
