#include "stdafx.h"
#include "imgui_base.h"
#include "mt_panel.h"
#include "mods_info.h"

// MODS-INFO of QUARK-PANEL: what the mods of MO2 cost, by mod, script and function (mods_info.cpp measures).
namespace xr_imgui
{
namespace quark_mt
{
namespace
{
u32 generation = 0; // of mods_info's tables the indices below belong to
u32 settled = 0, listed = ~0u; // the refresh of the numbers the rows were last settled for, new rows looked for
bool hovered = false; // the mouse was over the table in the last frame: nothing moves
int open_mod = -1;
u32 open_file = ~0u;
// rows once shown stay where they are, new ones come at the end (steady_order)
steady_order<u32, mods_info::max_mods> mods;
steady_order<u32, mods_info::max_files> scripts; // of the opened mod
steady_order<u32, mods_info::max_functions> functions; // of the opened script
bool mod_listed[mods_info::max_mods], script_listed[mods_info::max_files], function_listed[mods_info::max_functions];
char search[64] = {};

// Fewer samples than this in the last ~4 s: the numbers are rough, the row is grey
constexpr float few_samples = 5.f;

bool measured(const mods_info::rates& r) { return r.self > 0.01f || r.bytes > 1.f; }

// A sample rate (a second) as ms a frame: its share of the samples times the exact Lua time
float frame_ms_of(float sample_rate, float fps)
{
	const mods_info::summary& s = mods_info::totals();
	return s.samples > 0.f && fps > 0.f ? sample_rate / s.samples * (s.lua_main_ms + s.lua_worker_ms) / fps : 0.f;
}

void bytes_text(string32& out, float bytes)
{
	if (bytes >= 1024.f * 1024.f)
		xr_sprintf(out, L("%.1f MB/s", u8"%.1f МБ/с"), bytes / (1024.f * 1024.f));
	else if (bytes >= 1.f)
		xr_sprintf(out, L("%.0f KB/s", u8"%.0f КБ/с"), _max(bytes / 1024.f, 1.f));
	else
		xr_strcpy(out, "-");
}

// Where its time runs: the main thread, the game logic worker, or both with the main thread's share
void thread_text(string64& out, const mods_info::rates& r)
{
	const float main = r.self > 0.f ? r.main / r.self : 0.f;
	if (main >= 0.95f)
		xr_strcpy(out, L("main thread", u8"главный поток"));
	else if (main <= 0.05f)
		xr_strcpy(out, L("game logic worker", u8"воркер логики игры"));
	else
		xr_sprintf(out, L("both, main %.0f%%", u8"оба, главный %.0f%%"), main * 100.f);
}

void wrapped_tooltip(const char* text)
{
	ImGui::BeginTooltip();
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

// What a row's time is made of, for its tooltip
void parts_text(string512& out, const mods_info::rates& r, float fps)
{
	const float self = _max(r.self, 0.0001f);
	xr_sprintf(out, L("CPU %.2f ms a frame. Of it: in engine functions it called %.0f%%, in vanilla scripts it called "
		"%.0f%%, on the main thread %.0f%%. Samples a second: %.0f.", u8"CPU %.2f мс за кадр. Из них: в функциях движка, "
		u8"которые он вызвал, %.0f%%, в ванильных скриптах, которые он вызвал, %.0f%%, на главном потоке %.0f%%. Выборок "
		u8"в секунду: %.0f."), frame_ms_of(r.self, fps), r.native / self * 100.f, r.vanilla / self * 100.f,
		r.main / self * 100.f, r.self);
}

// The cells after the name
void numbers(const mods_info::rates& r, float fps, float frame_ms, ImGuiID glide_id)
{
	const float ms = frame_ms_of(r.self, fps);
	ImGui::TableSetColumnIndex(1);
	ImGui::Text("%5.2f", ms);
	ImGui::TableSetColumnIndex(2);
	const float share = frame_ms > 0.f ? ms / frame_ms : 0.f;
	bar(share, 50.f, ImGui::GetTextLineHeight() - 6.f, glide_id);
	ImGui::SameLine();
	ImGui::Text("%3.0f%%", share * 100.f);
	ImGui::TableSetColumnIndex(3);
	string32 text;
	bytes_text(text, r.bytes);
	ImGui::TextUnformatted(text);
	ImGui::TableSetColumnIndex(4);
	string64 thread;
	thread_text(thread, r);
	ImGui::TextUnformatted(thread);
}

bool begin_table()
{
	if (!ImGui::BeginTable("mods", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH))
		return false;
	ImGui::TableSetupColumn(L("MOD", u8"МОД"), ImGuiTableColumnFlags_WidthStretch);
	fixed_column(L("CPU, MS/FRAME", u8"CPU, МС/КАДР"), 90.f);
	fixed_column(L("SHARE OF FRAME", u8"ДОЛЯ КАДРА"), 96.f);
	fixed_column(L("MAKES GARBAGE", u8"СОЗДАЁТ МУСОРА"), 90.f);
	fixed_column(L("RUNS ON", u8"ГДЕ РАБОТАЕТ"), 150.f);
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	for (int column = 0; column < 5; ++column)
	{
		column_name(column);
		if (ImGui::IsItemHovered())
		{
			const char* const help[] = {
				L("A mod of MO2. Click it to see its scripts, click a script to see its functions.",
					u8"Мод из MO2. Нажмите на мод, чтобы увидеть его скрипты, на скрипт - чтобы увидеть его функции."),
				L("How many milliseconds of the processor the mod takes every frame: its scripts with the vanilla scripts "
					"and the engine functions they call.", u8"Сколько миллисекунд процессора мод забирает на каждый кадр: "
					u8"его скрипты вместе с ванильными скриптами и функциями движка, которые они вызывают."),
				L("Which part of the frame time the mod takes. All the mods with the vanilla scripts make the Lua time in "
					"the line above.", u8"Какую часть времени кадра занимает мод. Все моды вместе с ванильными скриптами "
					u8"дают время Lua из строки выше."),
				L("How much new Lua memory the mod takes a second: tables, strings, objects it makes. It is not the "
					"cleaning: the Lua garbage collector frees this memory later, and the more of it, the more often and "
					"the longer it collects.", u8"Сколько новой памяти Lua мод занимает в секунду: таблицы, строки, "
					u8"объекты, которые он создаёт. Это не уборка: эту память потом освобождает сборщик мусора Lua, и чем "
					u8"её больше, тем чаще и дольше он работает."),
				L("The thread the mod's time runs on: the main thread (the frame waits for it) or the game logic worker "
					"(it runs beside the render).", u8"На каком потоке идёт время мода: главный поток (кадр его ждёт) или "
					u8"воркер логики игры (работает параллельно рендеру).")};
			wrapped_tooltip(help[column]);
		}
	}
	return true;
}

// "Useful Idiots" in "450- Useful Idiots - bellyillish", case of Latin letters ignored
bool matches(const char* name)
{
	if (!search[0])
		return true;
	string128 a, b;
	strncpy_s(a, name, _TRUNCATE);
	xr_strcpy(b, search);
	_strlwr_s(a);
	_strlwr_s(b);
	return strstr(a, b) != nullptr;
}

void list_rows(const mods_info::summary& s)
{
	for (u32 index = 0; index < s.mods_count; ++index)
		if (!mod_listed[index] && measured(mods_info::mod(index).r))
		{
			mod_listed[index] = true;
			mods.push(index);
		}
	for (u32 index = 0; open_mod >= 0 && index < s.files; ++index)
	{
		const mods_info::file_row& f = mods_info::file(index);
		if (!script_listed[index] && f.mod == open_mod && measured(f.r))
		{
			script_listed[index] = true;
			scripts.push(index);
		}
	}
	for (u32 index = 0; open_file != ~0u && index < s.functions; ++index)
	{
		const mods_info::function_row& f = mods_info::function(index);
		if (!function_listed[index] && f.file == open_file && measured(f.r))
		{
			function_listed[index] = true;
			functions.push(index);
		}
	}
}

void close_script()
{
	open_file = ~0u;
	functions.count = 0;
	ZeroMemory(function_listed, sizeof(function_listed));
	listed = ~0u; // look for the rows at once
}

void close_mod()
{
	open_mod = -1;
	scripts.count = 0;
	ZeroMemory(script_listed, sizeof(script_listed));
	close_script();
}

void function_rows(float fps, float frame_ms)
{
	for (u32 order = 0; order < functions.count; ++order)
	{
		const u32 index = functions.keys[order];
		const mods_info::function_row& f = mods_info::function(index);
		ImGui::TableNextRow();
		if (row_hidden(ImGui::GetTextLineHeight()))
			continue;
		ImGui::PushID(int(index + mods_info::max_mods + mods_info::max_files));
		ImGui::PushStyleColor(ImGuiCol_Text, f.r.self * 4.f < few_samples ? muted : ink);
		ImGui::TableSetColumnIndex(0);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 36.f);
		const char* name = mods_info::function_name(index);
		string128 label;
		xr_sprintf(label, L("%s (line %d)", u8"%s (строка %d)"), name[0] == '?' ? L("(no name)", u8"(без имени)") : name,
			f.line);
		ImGui::TextUnformatted(label);
		if (ImGui::IsItemHovered())
		{
			string512 tip;
			parts_text(tip, f.r, fps);
			wrapped_tooltip(tip);
		}
		numbers(f.r, fps, frame_ms, ImGui::GetID("load"));
		ImGui::PopStyleColor();
		ImGui::PopID();
	}
}

void script_rows(float fps, float frame_ms)
{
	for (u32 order = 0; order < scripts.count; ++order)
	{
		const u32 index = scripts.keys[order];
		const mods_info::file_row& f = mods_info::file(index);
		const bool opened = index == open_file;
		ImGui::TableNextRow();
		if (!opened && row_hidden(ImGui::GetTextLineHeight()))
			continue;
		ImGui::PushID(int(index) + int(mods_info::max_mods));
		ImGui::PushStyleColor(ImGuiCol_Text, f.r.self * 4.f < few_samples ? muted : ink);
		ImGui::TableSetColumnIndex(0);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18.f);
		string128 label;
		xr_sprintf(label, "%s %s", opened ? "-" : "+", f.name);
		if (ImGui::Selectable(label, opened, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
		{
			close_script();
			open_file = opened ? ~0u : index;
		}
		if (ImGui::IsItemHovered() && ImGui::TableGetHoveredColumn() == 0)
		{
			string1024 tip;
			string512 parts;
			parts_text(parts, f.r, fps);
			xr_sprintf(tip, "%s\n%s", f.path, parts);
			wrapped_tooltip(tip);
		}
		numbers(f.r, fps, frame_ms, ImGui::GetID("load"));
		ImGui::PopStyleColor();
		ImGui::PopID();
		if (opened)
			function_rows(fps, frame_ms);
	}
}
} // namespace

const char* mods_tab(float fps, float frame_ms)
{
	const mods_info::summary& s = mods_info::totals();
	if (generation != s.generation)
	{
		generation = s.generation;
		mods.count = 0;
		ZeroMemory(mod_listed, sizeof(mod_listed));
		close_mod();
	}
	if (listed != s.refreshes)
	{
		listed = s.refreshes;
		list_rows(s);
	}
	if (settled != s.refreshes && !hovered)
	{
		settled = s.refreshes;
		mods.settle([](u32 index) { return mods_info::mod(index).r.self; }, 0.5f);
		scripts.settle([](u32 index) { return mods_info::file(index).r.self; }, 0.5f);
		functions.settle([](u32 index) { return mods_info::function(index).r.self; }, 0.5f);
	}

	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
	if (!s.running)
		ImGui::TextDisabled("%s", L("The game's scripts are not loaded yet.", u8"Скрипты игры ещё не загружены."));
	else
	{
		const float lua_ms = fps > 0.f ? (s.lua_main_ms + s.lua_worker_ms) / fps : 0.f;
		group_title("LUA");
		ImGui::SameLine();
		ImGui::Text(L("scripts %.2f ms a frame (%.0f%% of the frame): mods of MO2 %.2f ms, vanilla scripts without mods "
			"%.2f ms, garbage collection inside scripts %.2f ms.", u8"скрипты %.2f мс за кадр (%.0f%% кадра): моды MO2 "
			u8"%.2f мс, ванильные скрипты без модов %.2f мс, сборка мусора внутри скриптов %.2f мс."), lua_ms,
			frame_ms > 0.f ? lua_ms / frame_ms * 100.f : 0.f, frame_ms_of(s.mods.self, fps),
			frame_ms_of(s.vanilla.self, fps), frame_ms_of(s.gc, fps));
		string32 garbage;
		bytes_text(garbage, s.bytes);
		ImGui::TextDisabled(L("Main thread %.2f ms, game logic worker %.2f ms. The engine calls the scripts %.0f times a "
			"second, the scripts make %s of garbage.", u8"Главный поток %.2f мс, воркер логики игры %.2f мс. Движок "
			u8"вызывает скрипты %.0f раз в секунду, скрипты создают %s мусора."),
			fps > 0.f ? s.lua_main_ms / fps : 0.f, fps > 0.f ? s.lua_worker_ms / fps : 0.f, s.calls, garbage);
		ImGui::TextDisabled(L("The Lua time is measured exactly, its split by mod comes from %.0f samples of the Lua stack "
			"a second, the average of ~4 s. Grey rows have few samples: their numbers are rough.",
			u8"Время Lua измеряется точно, по модам оно делится по %.0f выборкам стека Lua в секунду, среднее за ~4 с. "
			u8"Серые строки - мало выборок, их цифры приблизительны."), s.samples);
		if (s.refused)
			ImGui::TextColored(accent, "%s", L("A script runs jit.profile or jit.allocprof: LuaJIT's sampler is taken, only "
				"the whole Lua time is known.", u8"Скрипт запустил jit.profile или jit.allocprof: сэмплер LuaJIT занят, "
				u8"известно только общее время Lua."));
		if (s.full)
			ImGui::TextDisabled("%s", L("More scripts ran than the tables hold: the newest are not counted until the tab is "
				"opened again.", u8"Скриптов больше, чем вмещают таблицы: новые не учитываются, пока вкладку не откроют "
				u8"заново."));
	}
	ImGui::PopTextWrapPos();

	ImGui::SetNextItemWidth(_min(320.f, content_width));
	ImGui::InputTextWithHint("##find", L("Find a mod by name", u8"Найти мод по названию"), search, sizeof(search));

	const ImVec2 table_top = ImGui::GetCursorScreenPos();
	if (!begin_table())
		return nullptr;
	u32 shown_mods = 0;
	for (u32 order = 0; order < mods.count; ++order)
	{
		const u32 index = mods.keys[order];
		const mods_info::mod_row& m = mods_info::mod(index);
		if (!matches(m.name))
			continue;
		++shown_mods;
		const bool opened = int(index) == open_mod;
		ImGui::TableNextRow();
		if (!opened && row_hidden(ImGui::GetTextLineHeight()))
			continue;
		ImGui::PushID(int(index));
		ImGui::PushStyleColor(ImGuiCol_Text, m.r.self * 4.f < few_samples ? muted : ink);
		ImGui::TableSetColumnIndex(0);
		string128 label;
		xr_sprintf(label, "%s %s", opened ? "-" : "+", m.name);
		if (ImGui::Selectable(label, opened, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
		{
			close_mod();
			open_mod = opened ? -1 : int(index);
		}
		if (ImGui::IsItemHovered() && ImGui::TableGetHoveredColumn() == 0)
		{
			string512 tip;
			parts_text(tip, m.r, fps);
			wrapped_tooltip(tip);
		}
		numbers(m.r, fps, frame_ms, ImGui::GetID("load"));
		ImGui::PopStyleColor();
		ImGui::PopID();
		if (opened)
			script_rows(fps, frame_ms);
	}
	ImGui::EndTable();
	hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(table_top,
		ImVec2(table_top.x + content_width, ImGui::GetCursorScreenPos().y));
	if (s.running && !shown_mods)
		ImGui::TextDisabled("%s", search[0] ? L("No mod by that name has run a script yet.",
			u8"Мод с таким названием ещё не запускал скриптов.") : L("measuring...", u8"измерение..."));
	return nullptr;
}
} // namespace quark_mt
} // namespace xr_imgui
