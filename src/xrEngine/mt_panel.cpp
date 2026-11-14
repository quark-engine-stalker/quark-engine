#include "stdafx.h"
#include "imgui_base.h"
#include "device.h"
#include "mt_panel.h"
#include <imgui_internal.h>
#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"
#include "igame_level.h"
#include "../Include/xrRender/RenderDeviceRender.h"

// QUARK-PANEL (console quark_panel), the engine's settings for players: what the engine runs in parallel, what each task
// and thread costs, the console switch of every task, the experimental switches of serial work on the main thread that
// can run on the workers, and the low latency modes with their measured latency.
extern bool IsMainMenuActive();
extern int ps_framelimiter;
int ps_mt_panel_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? 1 : 0;

namespace xr_imgui
{
namespace quark_mt
{
ImFont* big_font() { return Device.imgui().GetFont("$big"); }

bool block_button(const char* label, bool selected, const ImVec2& size)
{
	if (selected)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, accent);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent_hover);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent_press);
		ImGui::PushStyleColor(ImGuiCol_Border, accent);
		ImGui::PushStyleColor(ImGuiCol_Text, on_accent);
	}
	const bool clicked = ImGui::Button(label, size);
	if (selected)
		ImGui::PopStyleColor(5);
	return clicked;
}

void bar(float fraction, float width, float height)
{
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const float top = at.y + (ImGui::GetTextLineHeight() - height) * 0.5f;
	ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRect(ImVec2(at.x, top), ImVec2(at.x + width, top + height), ImGui::GetColorU32(stroke));
	const float fill = (width - 2.f) * _min(_max(fraction, 0.f), 1.f);
	if (fill > 0.f)
		draw->AddRectFilled(ImVec2(at.x + 1.f, top + 1.f), ImVec2(at.x + 1.f + fill, top + height - 1.f),
			ImGui::GetColorU32(accent));
}
} // namespace quark_mt

namespace
{
using namespace quark_mt;
constexpr mt_tasks::task no_time = mt_tasks::count;

enum group : u8 { no_group, group_job, group_animation, group_render, group_world, group_experimental };
enum place : u8 { beside_render, workers, frame_job, own_worker, main_thread, one_thread, in_render };

const char* group_name(group g)
{
	switch (g)
	{
	case group_job: return L("GAME LOGIC", u8"ЛОГИКА ИГРЫ");
	case group_animation: return L("ANIMATION", u8"АНИМАЦИЯ");
	case group_render: return L("RENDER", u8"РЕНДЕР");
	case group_world: return L("WORLD", u8"МИР");
	case group_experimental: return L("EXPERIMENTAL", u8"ЭКСПЕРИМЕНТ");
	default: return "";
	}
}

const char* place_name(place p)
{
	switch (p)
	{
	case beside_render: return L("beside the render", u8"рядом с рендером");
	case workers: return L("workers", u8"воркеры");
	case frame_job: return L("game logic worker", u8"воркер логики игры");
	case own_worker: return L("separate worker", u8"отдельный воркер");
	case main_thread: return L("main thread", u8"главный поток");
	case one_thread: return L("one thread", u8"один поток");
	case in_render: return L("main, in the render", u8"главный, в рендере");
	default: return "";
	}
}

struct row
{
	group starts; // a group starts at this row
	const char* command;
	mt_tasks::task task;
	bool part; // a part of the frame job
	bool experimental; // off by default, warns before it is switched on, never saved
	place on, off;
	const char* name_en;
	const char* name_ru;
	const char* help_en;
	const char* help_ru;
};

const row rows[] = {
	{group_job, "mt_frame_job", mt_tasks::frame_job, false, false, beside_render, main_thread,
		"Game logic beside the render", u8"Логика игры рядом с рендером",
		"NPC and A-Life scheduler, actor and hear Lua callbacks, physics and the Lua GC pass run on a worker while the "
		"main thread renders. Off: all of it runs on the main thread before the render, FPS drops in fights. The rows "
		"below are parts of this job.",
		u8"Планировщик NPC и A-Life, Lua-колбэки актёра и слуха, физика и сборка мусора Lua идут на воркере, пока "
		u8"главный поток рисует. Выкл: всё это на главном потоке перед рендером, в боях FPS падает. Строки ниже - части "
		u8"этой задачи."},
	{no_group, "mt_frame_independent", mt_tasks::frame_independent, true, false, workers, frame_job,
		"Extra workers of the job", u8"Дополнительные воркеры задачи",
		"Occlusion, grass visibility and NPC route searches start on their own workers as soon as the camera of the "
		"frame is set, before the bone poses, so the render finds them ready. Off: the game logic runs them itself after "
		"the NPCs, the render computes occlusion and grass when it needs them.",
		u8"Окклюзия, видимость травы и поиск маршрутов NPC стартуют на своих воркерах, как только задана камера кадра, "
		u8"ещё до поз костей, и рендер находит их готовыми. Выкл: логика игры делает их сама после NPC, рендер считает "
		u8"окклюзию и траву, когда они ему нужны."},
	{no_group, "r2_mt", mt_tasks::occlusion_grass, true, false, workers, in_render,
		"Occlusion and grass ahead", u8"Окклюзия и трава заранее",
		"Occlusion (HOM) and grass visibility are computed on the extra workers before the render asks; with a large "
		"grass radius the grass visibility is split over up to four of them. Then up to four workers, as many as the "
		"render needs, pack the shader data of the visible grass from its end back while the render draws from its "
		"start: the render copies what is packed and writes the rest itself, it never waits for the packing. Off: "
		"the main thread computes them inside the render on its own.",
		u8"Окклюзия (HOM) и видимость травы считаются на доп. воркерах до того, как их спросит рендер; при большом "
		u8"радиусе травы её видимость делится на четыре воркера. Затем до четырёх воркеров, сколько нужно рендеру, "
		u8"пакуют данные шейдера видимой травы с конца, пока рендер рисует с начала: рендер копирует упакованное, "
		u8"остальное пишет сам и никогда не ждёт упаковку. Выкл: их считает главный поток внутри рендера сам."},
	{no_group, "mt_level_path", mt_tasks::level_path, true, false, workers, frame_job,
		"NPC routes", u8"Маршруты NPC",
		"Route search (A*) of NPCs on the extra workers. Off: inside the NPC's own update.",
		u8"Поиск маршрута (A*) NPC на доп. воркерах. Выкл: внутри обновления самого NPC."},
	{no_group, "mt_detail_path", mt_tasks::detail_path, true, false, frame_job, frame_job,
		"NPC detail paths", u8"Детальные пути NPC",
		"Detail paths are built after the NPC pass, a limited number a frame. Off: inside the NPC's own update. Both on "
		"the game logic worker beside the render, this switch adds no thread.",
		u8"Детальные пути строятся после прохода NPC, ограниченное число за кадр. Выкл: внутри обновления самого NPC. "
		u8"Оба варианта на воркере логики игры рядом с рендером, лишнего потока этот переключатель не даёт."},
	{no_group, "mt_bullets", mt_tasks::bullets, true, false, frame_job, main_thread,
		"Bullets", u8"Пули",
		"Bullet flight and hits on the game logic worker. Off: on the main thread.",
		u8"Полёт и попадания пуль на воркере логики игры. Выкл: на главном потоке."},
	{no_group, "mt_map", mt_tasks::map, true, false, frame_job, main_thread,
		"Map markers", u8"Метки карты",
		"PDA map and minimap markers update on the game logic worker. Off: on the main thread.",
		u8"Метки карты КПК и мини-карты обновляются на воркере логики игры. Выкл: на главном потоке."},
	{no_group, "mt_level_sounds", mt_tasks::level_sounds, true, false, frame_job, main_thread,
		"Level ambient sounds", u8"Звуки уровня",
		"Static and ambient sounds of the level update on the game logic worker. Off: on the main thread.",
		u8"Статичные и фоновые звуки уровня обновляются на воркере логики игры. Выкл: на главном потоке."},
	{no_group, "mt_update_weapon_sounds", mt_tasks::weapon_sounds, true, false, frame_job, main_thread,
		"Weapon sound positions", u8"Позиции звуков оружия",
		"Sound positions of every weapon on the game logic worker, once the poses of the frame are built. Off: on the main "
		"thread.",
		u8"Позиции звуков каждого оружия на воркере логики игры, когда позы кадра построены. Выкл: на главном потоке."},
	{no_group, "mt_frame_audio", mt_tasks::frame_audio, true, false, own_worker, frame_job,
		"Sound beside Lua GC", u8"Звук рядом со сборкой Lua",
		"The sound engine update runs on its own worker beside the Lua GC pass at the end of the game logic. Off, or "
		"when that pass is not on the game logic worker: the game logic runs it itself.",
		u8"Обновление звукового движка идёт на своём воркере рядом со сборкой мусора Lua в конце логики игры. Выкл, "
		u8"или когда сборка идёт не на воркере логики игры: логика игры делает его сама."},
	{no_group, "mt_frame_tail", mt_tasks::frame_tail, true, false, own_worker, frame_job,
		"Frame tail rays", u8"Лучи хвоста кадра",
		"NPC foot rays, can_kill rays and the client export run on up to four workers beside the Lua GC pass, the foot "
		"rays take no global lock. Off: one after another in the job before the GC pass.",
		u8"Лучи ног NPC, лучи can_kill и экспорт клиента идут на четырёх воркерах рядом со сборкой Lua, лучи ног без "
		u8"общей блокировки. Выкл: по очереди в задаче перед сборкой."},
	{no_group, "mt_frame_lua_gc", mt_tasks::lua_gc, true, false, frame_job, main_thread,
		"Lua garbage collection", u8"Сборка мусора Lua",
		"The bounded Lua GC pass runs at the end of the game logic, beside the render. Off: on the main thread before "
		"the render.",
		u8"Ограниченный проход сборки мусора Lua идёт в конце логики игры, рядом с рендером. Выкл: на главном потоке "
		u8"перед рендером."},
	{group_animation, "mt_skeleton", mt_tasks::skeletons, false, false, workers, main_thread,
		"Bone poses", u8"Позы костей",
		"Skeletons of NPCs and objects are posed on the workers after the game update. Off: each pose is built on the "
		"main thread when the render first needs it.",
		u8"Скелеты NPC и объектов позируются на воркерах после обновления игры. Выкл: каждая поза строится на главном "
		u8"потоке, когда она впервые нужна рендеру."},
	{no_group, "mt_skeleton_callbacks", no_time, false, false, workers, main_thread,
		"Worker-safe bone callbacks", u8"Безопасные колбэки костей",
		"Skeletons with bone callbacks known to be safe (stalker head, spine, shoulders) are posed on the workers too. "
		"Off: such skeletons stay on the main thread. Its time is in Bone poses.",
		u8"Скелеты с проверенными колбэками костей (голова, спина, плечи сталкера) тоже позируются на воркерах. Выкл: "
		u8"такие скелеты остаются на главном потоке. Время - в строке «Позы костей»."},
	{group_render, "mt_light_faces", mt_tasks::light_faces, false, false, workers, main_thread,
		"Lamp shadow faces", u8"Грани теней ламп",
		"Static geometry of each shadow-casting lamp face is collected on the workers. Off: on the main thread.",
		u8"Статическая геометрия каждой грани тени лампы собирается на воркерах. Выкл: на главном потоке."},
	{no_group, "mt_view_static", mt_tasks::main_static, false, false, workers, main_thread,
		"Main view statics", u8"Статика основного вида",
		"The static geometry of every visible sector is culled (view, occlusion tests, LODs) on the workers while the "
		"main thread finds the visible dynamic objects. The results go into the scene in the original order, the "
		"picture is the same. Off: on the main thread before the dynamic objects.",
		u8"Статическая геометрия видимых секторов отсекается (вид, тесты окклюзии, LOD) на воркерах, пока главный поток "
		u8"ищет видимые динамические объекты. Результаты кладутся в сцену в исходном порядке, картинка та же. Выкл: на "
		u8"главном потоке до динамических объектов."},
	{no_group, "mt_opaque_sort", mt_tasks::opaque_sort, false, false, workers, main_thread,
		"Opaque sort", u8"Сортировка непрозрачного",
		"Long lists of opaque geometry are sorted on the workers before they are drawn. Off: each list is sorted on the "
		"main thread while it is drawn, that time is not shown.",
		u8"Длинные списки непрозрачной геометрии сортируются на воркерах до отрисовки. Выкл: каждый список сортируется "
		u8"на главном потоке при отрисовке, это время не показано."},
	{no_group, "mt_particle_render", mt_tasks::particle_render, false, false, workers, main_thread,
		"Particle vertices", u8"Вершины частиц",
		"Vertices of large particle effects are built on the workers. Off: on the main thread.",
		u8"Вершины больших эффектов частиц строятся на воркерах. Выкл: на главном потоке."},
	{no_group, "mt_wallmark_skin", mt_tasks::wallmark_skin, false, false, workers, main_thread,
		"Wallmarks on bodies", u8"Следы на телах",
		"Blood and bullet marks on bodies are skinned on the workers beside the main view. Off: on the main thread "
		"while they are drawn.",
		u8"Следы крови и пуль на телах скинятся на воркерах рядом с основным видом. Выкл: на главном потоке при "
		u8"отрисовке."},
	{group_world, "mt_physics", mt_tasks::physics, false, false, workers, one_thread,
		"Physics islands", u8"Острова физики",
		"Separate groups of physics bodies are solved on several workers when there is enough work. Off: one after "
		"another, and from the next level or save load the whole physics world steps on the main thread instead of "
		"the game logic worker.",
		u8"Независимые группы физических тел решаются на нескольких воркерах, когда работы достаточно. Выкл: по одной, "
		u8"а со следующей загрузки уровня или сохранения весь мир физики считается на главном потоке, а не на воркере "
		u8"логики игры."},
	{no_group, "mt_particle_turbulence", mt_tasks::particle_turbulence, false, false, workers, one_thread,
		"Particle turbulence", u8"Турбулентность частиц",
		"Turbulence of large particle effects is computed on several workers. Off: on one thread.",
		u8"Турбулентность больших эффектов частиц считается на нескольких воркерах. Выкл: на одном потоке."},
	{no_group, "mt_ai_vision", mt_tasks::ai_vision, false, false, workers, one_thread,
		"NPC vision rays", u8"Лучи зрения NPC",
		"Part of the vision rays of NPCs, those through the level geometry only, is traced on the workers ahead of their "
		"update. The rest, and every ray a moving object may cross, inside the NPC's own update. Off: all of them there.",
		u8"Часть лучей зрения NPC, только сквозь геометрию уровня, трассируется на воркерах до их обновления. Остальные и "
		u8"каждый луч, который может пересечь движущийся объект, - внутри обновления самого NPC. Выкл: все там."},
};

// r__low_latency, in the order of its token
struct latency_mode
{
	const char* token;
	const char* name_en;
	const char* name_ru;
	const char* help_en;
	const char* help_ru;
};

const latency_mode latency_modes[] = {
	{"off", "OFF", u8"ВЫКЛ",
		"As in the original game: the driver queues up to 3 frames. When the video card is the limit, the input waits "
		"for all of them: 47 ms at 1080p on the test PC.",
		u8"Как в оригинальной игре: драйвер копит в очереди до 3 кадров. Когда упор в видеокарту, ввод ждёт их все: "
		u8"47 мс в 1080p на тестовом ПК."},
	{"on", "ON", u8"ВКЛ",
		"The engine's own low latency: a queue of 1 frame and a short wait before the input is read, so the video card "
		"keeps no frames in reserve. The latency of NVIDIA Reflex (17 ms against 47 on the test PC) at the same FPS, on "
		"any video card. With VSync only the queue of 1 frame.",
		u8"Своя низкая задержка движка: очередь в 1 кадр и короткое ожидание перед чтением ввода, чтобы видеокарта не "
		u8"копила кадры впрок. Задержка как у NVIDIA Reflex (17 мс против 47 на тестовом ПК) при том же FPS, на любой "
		u8"видеокарте. С вертикальной синхронизацией - только очередь в 1 кадр."},
	{"reflex", "NVIDIA REFLEX", "NVIDIA REFLEX",
		"Low latency by the NVIDIA driver (Reflex Low Latency): 16.5 ms on the test PC, but the driver leaves the video "
		"card idle about 1 ms a frame, up to -10 % FPS at a high frame rate. Needs a GeForce GTX 900 or newer and a recent "
		"driver. The frame limit is then kept by the driver.",
		u8"Низкая задержка от драйвера NVIDIA (Reflex Low Latency): 16.5 мс на тестовом ПК, но драйвер оставляет "
		u8"видеокарту простаивать около 1 мс за кадр, при высоком FPS до -10 %. Нужна GeForce GTX 900 или новее и свежий "
		u8"драйвер. Ограничение FPS тогда держит драйвер."},
	{"reflex_boost", "REFLEX + BOOST", "REFLEX + BOOST",
		"NVIDIA Reflex, and the driver keeps the clocks of the video card up even when the processor is the limit: a "
		"little less latency there, more power and heat.",
		u8"NVIDIA Reflex, и драйвер держит частоты видеокарты высокими, даже когда упор в процессор: там чуть меньше "
		u8"задержка, но больше потребление и нагрев."},
};
constexpr u32 latency_mode_count = sizeof(latency_modes) / sizeof(latency_modes[0]);

// r__upscaler, r__dlss_model, r__upscaler_mode: one button per token, in the order of the token
struct choice
{
	const char* token;
	const char* name_en;
	const char* name_ru;
	const char* help_en;
	const char* help_ru;
};

const choice upscaler_methods[] = {
	{"off", "OFF", u8"ВЫКЛ", "The scene renders at the resolution of the screen, as in the original game.",
		u8"Сцена рисуется в разрешении экрана, как в оригинальной игре."},
	{"dlss", "NVIDIA DLSS", "NVIDIA DLSS",
		"The scene renders at a lower resolution and NVIDIA DLSS rebuilds the full resolution from it and the frames "
		"before: more FPS when the video card is the limit, and its own anti-aliasing instead of SSS TAA and SMAA. Needs a "
		"GeForce RTX and the motion vectors of Screen Space Shaders 23 (GAMMA has them).",
		u8"Сцена рисуется в меньшем разрешении, и NVIDIA DLSS восстанавливает полное разрешение по ней и прошлым "
		u8"кадрам: больше FPS, когда упор в видеокарту, и своё сглаживание вместо SSS TAA и SMAA. Нужны GeForce RTX и "
		u8"векторы движения Screen Space Shaders 23 (в GAMMA они есть)."},
	{"fsr", "AMD FSR 3.1", "AMD FSR 3.1",
		"The scene renders at a lower resolution and AMD FSR 3.1 rebuilds the full resolution from it and the frames "
		"before, with shaders on any video card (AMD, NVIDIA, Intel): more FPS when the video card is the limit, its own "
		"anti-aliasing instead of SSS TAA and SMAA. Softer than DLSS on fine details such as grass and wires. Needs the "
		"motion vectors of Screen Space Shaders 23 (GAMMA has them).",
		u8"Сцена рисуется в меньшем разрешении, и AMD FSR 3.1 восстанавливает полное разрешение по ней и прошлым "
		u8"кадрам шейдерами на любой видеокарте (AMD, NVIDIA, Intel): больше FPS, когда упор в видеокарту, и своё "
		u8"сглаживание вместо SSS TAA и SMAA. Мягче DLSS на мелких деталях вроде травы и проводов. Нужны векторы "
		u8"движения Screen Space Shaders 23 (в GAMMA они есть)."},
};

const choice dlss_models[] = {
	{"auto", "AUTO", u8"АВТО",
		"RTX 20 and 30: DLSS 3, the fastest. RTX 40 and 50: DLSS 4 for DLAA, quality and balance, DLSS 4.5 for "
		"performance and max FPS, as NVIDIA recommends.",
		u8"RTX 20 и 30: DLSS 3, самый быстрый. RTX 40 и 50: DLSS 4 для DLAA, качества и баланса, DLSS 4.5 для "
		u8"производительности и макс. FPS, как советует NVIDIA."},
	{"dlss45", "DLSS 4.5", "DLSS 4.5",
		"The second transformer model (presets M and L): the sharpest and steadiest image, the least ghosting. Fast on "
		"RTX 40 and 50. RTX 20 and 30 have no FP8, there it costs 2-3.5 ms a frame at 1080p and can be slower than no "
		"upscaler at all.",
		u8"Вторая трансформерная модель (пресеты M и L): самая чёткая и стабильная картинка, меньше всего шлейфов. "
		u8"Быстрая на RTX 40 и 50. У RTX 20 и 30 нет FP8, там она стоит 2-3.5 мс за кадр в 1080p и может быть медленнее, "
		u8"чем без апскейлера."},
	{"dlss4", "DLSS 4", "DLSS 4",
		"The first transformer model (preset K): sharper than DLSS 3, about 1.2-1.5 ms a frame at 1080p on RTX 20 and 30.",
		u8"Первая трансформерная модель (пресет K): чётче DLSS 3, около 1.2-1.5 мс за кадр в 1080p на RTX 20 и 30."},
	{"dlss3", "DLSS 3", "DLSS 3",
		"The convolutional model (preset E): the cheapest, 0.4-0.6 ms a frame at 1080p on RTX 20 and 30, the most FPS "
		"on these cards, softer and with more ghosting than the transformers.",
		u8"Свёрточная модель (пресет E): самая дешёвая, 0.4-0.6 мс за кадр в 1080p на RTX 20 и 30, больше всего FPS на "
		u8"этих картах, мягче и с более заметными шлейфами, чем трансформеры."},
};

const choice upscaler_modes[] = {
	{"dlaa", "DLAA", "DLAA", "The scene at the full resolution of the screen: no FPS gain, the best anti-aliasing.",
		u8"Сцена в полном разрешении экрана: FPS не растёт, лучшее сглаживание."},
	{"quality", "QUALITY", u8"КАЧЕСТВО", "The scene at 67 % of the width and height of the screen.",
		u8"Сцена в 67 % ширины и высоты экрана."},
	{"balanced", "BALANCED", u8"БАЛАНС", "The scene at 58 % of the width and height of the screen.",
		u8"Сцена в 58 % ширины и высоты экрана."},
	{"performance", "PERFORMANCE", u8"ПРОИЗВОДИТЕЛЬНОСТЬ", "The scene at 50 % of the width and height of the screen.",
		u8"Сцена в 50 % ширины и высоты экрана."},
	{"ultra_performance", "MAX FPS [BLURRY]", u8"МАКС. FPS [МЫЛО]",
		"The scene at 33 % of the width and height of the screen: the most FPS and the softest image, for 4K.",
		u8"Сцена в 33 % ширины и высоты экрана: больше всего FPS и самая мягкая картинка, для 4K."},
};

const choice fsr_modes[] = {
	{"dlaa", "NATIVE AA", u8"СГЛАЖИВАНИЕ",
		"The scene at the full resolution of the screen: no FPS gain, FSR only anti-aliases.",
		u8"Сцена в полном разрешении экрана: FPS не растёт, FSR только сглаживает."},
	{"quality", "QUALITY", u8"КАЧЕСТВО", "The scene at 67 % of the width and height of the screen.",
		u8"Сцена в 67 % ширины и высоты экрана."},
	{"balanced", "BALANCED", u8"БАЛАНС", "The scene at 59 % of the width and height of the screen.",
		u8"Сцена в 59 % ширины и высоты экрана."},
	{"performance", "PERFORMANCE", u8"ПРОИЗВОДИТЕЛЬНОСТЬ", "The scene at 50 % of the width and height of the screen.",
		u8"Сцена в 50 % ширины и высоты экрана."},
	{"ultra_performance", "MAX FPS [BLURRY]", u8"МАКС. FPS [МЫЛО]",
		"The scene at 33 % of the width and height of the screen: the most FPS and the softest image, for 4K.",
		u8"Сцена в 33 % ширины и высоты экрана: больше всего FPS и самая мягкая картинка, для 4K."},
};

enum tab { tab_tasks, tab_threads, tab_latency, tab_upscaler };
tab current_tab = tab_tasks;

const char* tab_help()
{
	switch (current_tab)
	{
	case tab_threads:
		return L("Click a thread to see what it does. CPU cycles of every thread come from Windows once a second, LOAD "
			"is the share of one processor. WAIT shows how fast the job pool starts work.",
			u8"Нажмите на поток, чтобы увидеть, что он делает. Такты CPU каждого потока берутся у Windows раз в секунду, "
			u8"НАГРУЗКА - доля одного процессора. ОЖИДАНИЕ показывает, как быстро пул задач начинает работу.");
	case tab_latency:
		return L("Latency: from the start of a frame, when the game reads the mouse and the keyboard, to the moment the "
			"video card has finished it. The mouse and the monitor add their own part. Measured while this tab is open, "
			"once a second, per mode: switch the modes to compare them. A new frame limit or VSync starts the measuring "
			"anew. Hover a mode to read what it does.",
			u8"Задержка: от начала кадра, когда игра читает мышь и клавиатуру, до момента, когда видеокарта его закончила. "
			u8"Мышь и монитор добавляют свою часть. Измеряется, пока открыта эта вкладка, раз в секунду, для каждого "
			u8"режима отдельно: переключайте режимы, чтобы сравнить. Новое ограничение FPS или VSync начинает замеры заново. "
			u8"Наведите на режим, чтобы прочитать, что он делает.");
	case tab_upscaler:
		return L("A new choice works from the next frame. Only the first choice of DLSS in a game session loads the library "
			"of NVIDIA, about 2 seconds. Hover a button to read what it does.",
			u8"Новый выбор действует со следующего кадра. Только первый выбор DLSS за запуск игры загружает библиотеку "
			u8"NVIDIA, около 2 секунд. Наведите на кнопку, чтобы прочитать, что она делает.");
	default:
		return L("Hover a task to read what it does. MS/FRAME is CPU time on all its threads, the average "
			"of the last second, LOAD is its share of the frame. Every switch is a console command saved in user.ltx.",
			u8"Наведите на задачу, чтобы прочитать, что она делает. МС/КАДР - время CPU на всех её потоках, среднее "
			u8"за последнюю секунду, НАГРУЗКА - её доля кадра. Каждый переключатель - консольная команда, она "
			u8"сохраняется в user.ltx.");
	}
}

constexpr float name_width = 300.f, place_width = 150.f, number_width = 66.f, load_width = 104.f, threads_width = 64.f,
	state_width = 62.f;

// One second of frames
struct seconds
{
	u64 sum[mt_tasks::count];
	u32 lanes[mt_tasks::count];
	u32 frames;
	u64 start;
};
seconds current = {};

struct shown_numbers
{
	float ms[mt_tasks::count];
	u32 lanes[mt_tasks::count];
	float fps, frame_ms;
};
shown_numbers shown = {};

const row* pending = nullptr; // the experimental row waiting for the warning

void start_counting()
{
	for (mt_tasks::counter& counter : mt_tasks::counters)
	{
		InterlockedExchange64(&counter.ticks, 0);
		InterlockedExchange(&counter.lanes, 0);
	}
	current = {};
	current.start = CPU::QPC();
	shown = {};
	mt_tasks::counting = true;
}

// Here nothing of the engine runs in parallel: the frame job was joined at the end of the previous frame.
void take_frame()
{
	for (u32 task = 0; task < mt_tasks::count; ++task)
	{
		const u64 ticks = u64(InterlockedExchange64(&mt_tasks::counters[task].ticks, 0));
		const u32 lanes = u32(InterlockedExchange(&mt_tasks::counters[task].lanes, 0));
		current.sum[task] += ticks;
		current.lanes[task] = _max(current.lanes[task], lanes ? lanes : ticks ? 1u : 0u);
	}
	++current.frames;

	const u64 now = CPU::QPC();
	if (now - current.start < CPU::qpc_freq)
		return;
	const double frames = double(current.frames);
	const double tick_ms = 1000.0 / double(CPU::qpc_freq);
	for (u32 task = 0; task < mt_tasks::count; ++task)
	{
		shown.ms[task] = float(double(current.sum[task]) * tick_ms / frames);
		shown.lanes[task] = current.lanes[task];
	}
	shown.frame_ms = float(double(now - current.start) * tick_ms / frames);
	shown.fps = 1000.f / shown.frame_ms;
	current = {};
	current.start = now;
}

void set(const row& r, bool on)
{
	string128 line;
	xr_sprintf(line, "%s %d", r.command, on ? 1 : 0);
	Console->Execute(line);
}

void push_style()
{
	// Soft window corners, square frames: the block buttons of a row touch each other
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f, 14.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f, 2.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 6.f));
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.f, 3.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 3.f);
	ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 2.f);

	const ImGuiCol_ colours[] = {ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_PopupBg, ImGuiCol_Border, ImGuiCol_Text,
		ImGuiCol_TextDisabled, ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive, ImGuiCol_Header,
		ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive, ImGuiCol_Separator, ImGuiCol_TableBorderLight, ImGuiCol_TableHeaderBg,
		ImGuiCol_ModalWindowDimBg, ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab,
		ImGuiCol_SliderGrabActive, ImGuiCol_ScrollbarBg, ImGuiCol_ScrollbarGrab, ImGuiCol_ScrollbarGrabHovered,
		ImGuiCol_ScrollbarGrabActive};
	const ImVec4 values[] = {rgb(0x161616, 0.96f), rgb(0, 0.f), paper, stroke, ink,
		muted, paper, rgb(0xFFFFFF, 0.07f), rgb(0xFFFFFF, 0.12f), rgb(0xFFFFFF, 0.035f),
		rgb(0xFFFFFF, 0.06f), rgb(0xFFFFFF, 0.09f), stroke, stroke, surface,
		rgb(0, 0.6f), surface, rgb(0x2C2C2C), rgb(0x333333), accent,
		accent_press, paper, stroke, muted,
		accent};
	static_assert(sizeof(colours) / sizeof(colours[0]) == sizeof(values) / sizeof(values[0]), "a value per colour");
	for (u32 index = 0; index < sizeof(colours) / sizeof(colours[0]); ++index)
		ImGui::PushStyleColor(colours[index], values[index]);
}

void pop_style()
{
	ImGui::PopStyleColor(25);
	ImGui::PopStyleVar(14);
}

void number(float value, bool known)
{
	if (known)
		ImGui::Text("%6.2f", value);
	else
		ImGui::TextDisabled("%6s", "-");
}

void warning()
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(500.f, 0.f));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ink);
	ImGui::PushStyleColor(ImGuiCol_Text, paper);
	ImGui::PushStyleColor(ImGuiCol_Border, stroke);
	ImGui::PushStyleColor(ImGuiCol_Separator, muted);
	if (ImGui::BeginPopupModal("WARNING", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
	{
		ImGui::Text("%s: %s", L("EXPERIMENTAL", u8"ЭКСПЕРИМЕНТ"), pending ? L(pending->name_en, pending->name_ru) : "");
		ImGui::Separator();
		ImGui::TextWrapped("%s", L("This work does not run in parallel by default. Switching it on may crash or hang "
			"the game, lose the progress since the last save or draw the scene wrong. It stays on until you quit the "
			"game. Switch it off here if anything looks wrong.",
			u8"По умолчанию эта работа не выполняется параллельно. Включение может привести к крашу или зависанию игры, "
			u8"потере прогресса с последнего сохранения или неправильной картинке. Оно действует до выхода из игры. "
			u8"Если что-то не так, выключите его здесь."));
		ImGui::Spacing();
		if (block_button(L("SWITCH ON", u8"ВКЛЮЧИТЬ"), true) && pending)
		{
			set(*pending, true);
			pending = nullptr;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, ink);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, surface);
		if (ImGui::Button(L("CANCEL", u8"ОТМЕНА")))
		{
			pending = nullptr;
			ImGui::CloseCurrentPopup();
		}
		ImGui::PopStyleColor(2);
		ImGui::EndPopup();
	}
	ImGui::PopStyleColor(4);
}

// A small flag drawn by hand: the font has no emoji
void flag(ImDrawList* draw, const ImVec2& a, const ImVec2& b, bool russian)
{
	const float h = b.y - a.y, w = b.x - a.x;
	if (russian)
	{
		const ImU32 stripes[3] = {IM_COL32(255, 255, 255, 255), IM_COL32(0, 57, 166, 255), IM_COL32(213, 43, 30, 255)};
		for (int index = 0; index < 3; ++index)
			draw->AddRectFilled(ImVec2(a.x, a.y + h * index / 3.f), ImVec2(b.x, a.y + h * (index + 1) / 3.f), stripes[index]);
		return;
	}
	for (int index = 0; index < 7; ++index)
		draw->AddRectFilled(ImVec2(a.x, a.y + h * index / 7.f), ImVec2(b.x, a.y + h * (index + 1) / 7.f),
			index % 2 ? IM_COL32(255, 255, 255, 255) : IM_COL32(178, 34, 52, 255));
	draw->AddRectFilled(a, ImVec2(a.x + w * 0.42f, a.y + h * 4.f / 7.f), IM_COL32(60, 59, 110, 255));
	for (int y = 0; y < 2; ++y)
		for (int x = 0; x < 3; ++x)
			draw->AddRectFilled(ImVec2(a.x + 2.f + x * 3.6f, a.y + 2.f + y * 4.f), ImVec2(a.x + 3.f + x * 3.6f, a.y + 3.f + y * 4.f),
				IM_COL32(255, 255, 255, 255));
}

// A button of the top right corner, the captions of XRAY-TWEAKER: no frame, hovering eases in over 0.15 s. Placed by hand
// and not laid out, so the corner does not widen the content of the window.
bool corner_button(const char* id, const ImRect& box, float& hover)
{
	const ImGuiID item = ImGui::GetID(id);
	bool hovered = false, held = false;
	const bool clicked = ImGui::ItemAdd(box, item) && ImGui::ButtonBehavior(box, item, &hovered, &held);
	hover = _min(_max(hover + (hovered ? 1.f : -1.f) * ImGui::GetIO().DeltaTime / 0.15f, 0.f), 1.f);
	return clicked;
}

// Muted text that turns to ink under the mouse
ImU32 ghost_ink(float hover) { return ImGui::GetColorU32(ImLerp(muted, ink, hover)); }

// Languages, RESET and X in the top right corner of the window
void corner(ide& owner)
{
	static float hover[4] = {};
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 win = ImGui::GetWindowPos(), win_size = ImGui::GetWindowSize();
	const float side = 38.f, top = win.y + 4.f, middle = top + side * 0.5f, text_h = ImGui::GetTextLineHeight();
	float x = win.x + win_size.x - 4.f;
	ImGui::PushClipRect(win, ImVec2(win.x + win_size.x, win.y + win_size.y), false); // the corner is in the padding

	// X: a thin cross, a dark plate under the mouse
	x -= side;
	const ImRect close(x, top, x + side, top + side);
	if (corner_button("close", close, hover[0]))
		owner.Show(false);
	ImGui::SetItemTooltip("%s", L("Close", u8"Закрыть"));
	if (hover[0] > 0.f)
	{
		ImVec4 plate = ImGui::IsItemActive() ? stroke : surface;
		plate.w = hover[0];
		draw->AddRectFilled(close.Min, close.Max, ImGui::GetColorU32(plate), 4.f);
	}
	const ImU32 bright = ImGui::GetColorU32(ink);
	draw->AddLine(ImVec2(x + side * 0.5f - 5.f, middle - 5.f), ImVec2(x + side * 0.5f + 5.f, middle + 5.f), bright);
	draw->AddLine(ImVec2(x + side * 0.5f + 5.f, middle - 5.f), ImVec2(x + side * 0.5f - 5.f, middle + 5.f), bright);

	// RESET: muted text, ink under the mouse
	const char* const reset = L("RESET", u8"СБРОС");
	const float reset_w = ImGui::CalcTextSize(reset).x + 20.f;
	x -= reset_w + 4.f;
	if (corner_button("reset", ImRect(x, top, x + reset_w, top + side), hover[1]))
	{
		for (const row& r : rows)
			if (Console->GetCommand(r.command))
				set(r, !r.experimental);
		Console->Execute("r__low_latency off");
	}
	ImGui::SetItemTooltip("%s", L("Every task and the low latency mode back to their defaults, the frame limit stays",
		u8"Все задачи и режим задержки в состояние по умолчанию, ограничение FPS остаётся"));
	draw->AddText(ImVec2(x + 10.f, middle - text_h * 0.5f), ghost_ink(hover[1]), reset);

	// the languages: a flag and its name, the chosen one underlined in the accent
	x -= 10.f;
	for (int language = 0; language < 2; ++language) // right to left: English, Russian
	{
		const bool russian = language == 1;
		const char* const name = russian ? "RU" : "ENG";
		const float name_w = ImGui::CalcTextSize(name).x, button_w = 8.f + 24.f + 8.f + name_w + 8.f;
		x -= button_w;
		if (corner_button(name, ImRect(x, top, x + button_w, top + side), hover[2 + language]))
			ps_mt_panel_lang = russian ? 1 : 0;
		const bool chosen = (ps_mt_panel_lang != 0) == russian;
		flag(draw, ImVec2(x + 8.f, middle - 8.f), ImVec2(x + 32.f, middle + 8.f), russian);
		draw->AddText(ImVec2(x + 40.f, middle - text_h * 0.5f), chosen ? bright : ghost_ink(hover[2 + language]), name);
		if (chosen)
			draw->AddLine(ImVec2(x + 8.f, top + side - 6.f), ImVec2(x + button_w - 8.f, top + side - 6.f),
				ImGui::GetColorU32(accent), 2.f);
	}
	ImGui::PopClipRect();
}

void title(ide& owner)
{
	ImFont* big = big_font();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const char* const name = "QUARK-PANEL"; // the same in every language
	const float size = big ? big->FontSize : ImGui::GetFontSize();
	const ImVec2 text = big ? big->CalcTextSizeA(size, FLT_MAX, 0.f, name) : ImGui::CalcTextSize(name);
	const ImVec2 block(text.x + 20.f, text.y + 8.f);
	draw->AddRectFilled(at, ImVec2(at.x + block.x, at.y + block.y), ImGui::GetColorU32(accent), 3.f);
	draw->AddText(big, size, ImVec2(at.x + 10.f, at.y + 4.f), ImGui::GetColorU32(on_accent), name);
	ImGui::Dummy(block);
	ImGui::SameLine(0.f, 10.f);
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (block.y - ImGui::GetTextLineHeight()) * 0.5f);
	ImGui::TextDisabled("v0.1");
	corner(owner);
}

// LABEL value unit, the value in the big font, all of a row on the baseline y; null value: not known yet
void metric(float y, const char* label, const char* value, const char* unit, const char* help, bool first)
{
	ImFont* big = big_font();
	const float lift = big ? big->FontSize - ImGui::GetFontSize() - 3.f : 0.f;
	if (!first)
		ImGui::SameLine(0.f, 34.f);
	ImGui::SetCursorPosY(y + lift);
	ImGui::TextDisabled("%s", label);
	const bool hovered = ImGui::IsItemHovered();
	ImGui::SameLine(0.f, 10.f);
	ImGui::SetCursorPosY(y);
	ImGui::PushFont(big);
	ImGui::TextUnformatted(value ? value : "...");
	ImGui::PopFont();
	if (unit[0])
	{
		ImGui::SameLine(0.f, 5.f);
		ImGui::SetCursorPosY(y + lift);
		ImGui::TextDisabled("%s", unit);
	}
	if (hovered || ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", help);
}

// FPS 189   FRAME 5.28 ms
void metrics()
{
	const bool known = shown.fps > 0.f;
	const float y = ImGui::GetCursorPosY();
	string32 value;
	xr_sprintf(value, "%.0f", shown.fps);
	metric(y, "FPS", known ? value : nullptr, "", L("Frames a second, the last second", u8"Кадров в секунду за последнюю секунду"),
		true);
	xr_sprintf(value, "%.2f", shown.frame_ms);
	metric(y, L("FRAME", u8"КАДР"), known ? value : nullptr, L("ms", u8"мс"),
		L("Average frame time of the last second", u8"Среднее время кадра за последнюю секунду"), false);
}

void tabs()
{
	const char* const names[] = {L("TASKS", u8"ЗАДАЧИ"), L("THREADS", u8"ПОТОКИ"), L("LATENCY", u8"ЗАДЕРЖКА"),
		L("UPSCALER", u8"АПСКЕЙЛЕР")};
	for (int index = 0; index < 4; ++index)
	{
		if (index)
			ImGui::SameLine(0.f, 0.f);
		if (block_button(names[index], current_tab == index, ImVec2(130.f, 0.f)))
			current_tab = tab(index);
	}
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y - 3.f), ImVec2(at.x + content_width, at.y - 3.f),
		ImGui::GetColorU32(stroke));
	ImGui::Dummy(ImVec2(0.f, 2.f));
}

const char* tasks_tab(bool& open_warning)
{
	const char* help = nullptr;
	const bool job_on = Console->GetBool("mt_frame_job");
	const bool independent_on = Console->GetBool("mt_frame_independent");
	const bool lua_gc_on = Console->GetBool("mt_frame_lua_gc");
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
	ImGui::TextDisabled("%s", L("MT ON: the task runs in parallel, RUNS ON shows on which threads. MT OFF: the same work on "
		"one thread.",
		u8"MT ВКЛ: задача выполняется параллельно, в колонке ГДЕ РАБОТАЕТ видно, на каких потоках. MT ВЫКЛ: та же работа "
		u8"на одном потоке."));
	ImGui::PopTextWrapPos();

	if (!ImGui::BeginTable("tasks", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH))
		return help;
	ImGui::TableSetupColumn(L("TASK", u8"ЗАДАЧА"), ImGuiTableColumnFlags_WidthFixed, name_width);
	ImGui::TableSetupColumn(L("RUNS ON", u8"ГДЕ РАБОТАЕТ"), ImGuiTableColumnFlags_WidthFixed, place_width);
	ImGui::TableSetupColumn(L("MS/FRAME", u8"МС/КАДР"), ImGuiTableColumnFlags_WidthFixed, number_width);
	ImGui::TableSetupColumn(L("LOAD", u8"НАГРУЗКА"), ImGuiTableColumnFlags_WidthFixed, load_width);
	ImGui::TableSetupColumn(L("THREADS", u8"ПОТОКИ"), ImGuiTableColumnFlags_WidthFixed, threads_width);
	ImGui::TableSetupColumn("MT", ImGuiTableColumnFlags_WidthFixed, state_width);
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	for (int column = 0; column < 6; ++column)
	{
		ImGui::TableSetColumnIndex(column);
		ImGui::TextDisabled("%s", ImGui::TableGetColumnName(column));
		if (ImGui::IsItemHovered())
		{
			const char* const tips[] = {
				L("What the engine does", u8"Что делает движок"),
				L("Where it runs now", u8"Где выполняется сейчас"),
				L("CPU time a frame, all its threads", u8"Время CPU за кадр, все её потоки"),
				L("Share of the frame time", u8"Доля времени кадра"),
				L("Threads one run took", u8"Сколько потоков заняла одна раздача"),
				L("Multithreading: ON runs the task in parallel, OFF on one thread",
					u8"Многопоточность: ВКЛ - задача выполняется параллельно, ВЫКЛ - на одном потоке")};
			ImGui::SetTooltip("%s", tips[column]);
		}
	}

	for (u32 index = 0; index < sizeof(rows) / sizeof(rows[0]); ++index)
	{
		const row& r = rows[index];
		if (r.starts != no_group)
		{
			ImGui::TableNextRow();
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(surface));
			ImGui::TableSetColumnIndex(0);
			ImGui::PushStyleColor(ImGuiCol_Text, accent);
			ImGui::TextUnformatted(group_name(r.starts));
			ImGui::PopStyleColor();
		}

		ImGui::TableNextRow();
		const bool exists = Console->GetCommand(r.command) != nullptr;
		if (row_hidden(exists ? ImGui::GetFrameHeight() : ImGui::GetTextLineHeight()))
			continue;
		ImGui::PushID(int(index));
		const bool on = exists && Console->GetBool(r.command);

		ImGui::TableSetColumnIndex(0);
		ImGui::PushStyleColor(ImGuiCol_Text, (r.part && !job_on) || !exists ? muted : ink);
		string128 label;
		xr_sprintf(label, "%s%s%s", r.part ? "  " : "", r.experimental ? "! " : "", L(r.name_en, r.name_ru));
		ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			help = L(r.help_en, r.help_ru);

		// a task on the extra workers' batch or beside the GC pass runs elsewhere without them
		place where = on ? r.on : r.off;
		if (on && r.task == mt_tasks::frame_audio && !lua_gc_on)
			where = frame_job;
		if (on && !independent_on && r.task == mt_tasks::level_path)
			where = frame_job;
		if (on && !independent_on && r.task == mt_tasks::occlusion_grass)
			where = job_on ? in_render : main_thread; // the render asks before the game logic gets to them
		// with the frame job off its parts run on the main thread
		if (r.part && !job_on && (where == frame_job || where == own_worker))
			where = main_thread;
		ImGui::TableSetColumnIndex(1);
		if (on && where != main_thread && where != one_thread && where != in_render)
			ImGui::TextUnformatted(place_name(where));
		else
			ImGui::TextDisabled("%s", place_name(where));

		const bool timed = r.task != no_time && shown.fps > 0.f;
		const bool ran = timed && (shown.ms[r.task] > 0.f || shown.lanes[r.task]);
		ImGui::TableSetColumnIndex(2);
		number(ran ? shown.ms[r.task] : 0.f, ran);
		ImGui::TableSetColumnIndex(3);
		if (ran)
		{
			const float share = shown.frame_ms > 0.f ? shown.ms[r.task] / shown.frame_ms : 0.f;
			bar(share, 60.f, ImGui::GetTextLineHeight() - 6.f);
			ImGui::SameLine();
			ImGui::Text("%3.0f%%", share * 100.f);
		}
		ImGui::TableSetColumnIndex(4);
		if (ran)
			ImGui::Text("%3u", shown.lanes[r.task]);
		else
			ImGui::TextDisabled("%3s", "-");

		ImGui::TableSetColumnIndex(5);
		if (!exists)
			ImGui::TextDisabled("n/a");
		else if (block_button(on ? L("ON", u8"ВКЛ") : L("OFF", u8"ВЫКЛ"), on, ImVec2(state_width - 12.f, 0.f)))
		{
			if (!on && r.experimental)
			{
				pending = &r;
				open_warning = true;
			}
			else
				set(r, !on);
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
	if (!job_on && !help)
		help = L("The game logic is off: its parts run on the main thread before the render.",
			u8"Логика игры выключена: её части выполняются на главном потоке перед рендером.");
	return help;
}

// An accent title on a dark strip: the group rows of TASKS outside a table
void section(const char* name)
{
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const float height = ImGui::GetTextLineHeight() + 6.f;
	draw->AddRectFilled(at, ImVec2(at.x + content_width, at.y + height), ImGui::GetColorU32(surface));
	draw->AddText(ImVec2(at.x + 6.f, at.y + 3.f), ImGui::GetColorU32(accent), name);
	ImGui::Dummy(ImVec2(content_width, height));
}

const char* latency_tab()
{
	const char* help = nullptr;
	const bool in_level = g_pGameLevel && g_pGameLevel->bReady && !IsMainMenuActive();
	IRenderDeviceRender::latency_report report;
	Device.m_pRender->LatencyReport(report, in_level);
	const u32 chosen = _min(report.mode, latency_mode_count - 1);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);

	section(L("LOW LATENCY", u8"НИЗКАЯ ЗАДЕРЖКА"));
	for (u32 mode = 0; mode < latency_mode_count; ++mode)
	{
		const latency_mode& m = latency_modes[mode];
		if (mode)
			ImGui::SameLine(0.f, 0.f);
		if (block_button(L(m.name_en, m.name_ru), mode == chosen, ImVec2(content_width / latency_mode_count, 0.f)) &&
			mode != chosen)
		{
			string64 line;
			xr_sprintf(line, "r__low_latency %s", m.token);
			Console->Execute(line);
		}
		if (ImGui::IsItemHovered())
			help = L(m.help_en, m.help_ru);
	}
	if (report.reflex_missing)
		ImGui::TextColored(accent, "%s", L("NVIDIA Reflex is not available on this system (not an NVIDIA video card or an old "
			"driver): the mode works as OFF.",
			u8"NVIDIA Reflex недоступен на этой системе (видеокарта не NVIDIA или старый драйвер): режим работает как ВЫКЛ."));
	ImGui::Spacing();

	section(L("FRAME LIMIT", u8"ОГРАНИЧЕНИЕ FPS"));
	ImGui::SetNextItemWidth(content_width);
	ImGui::SliderInt("##frame_limit", &ps_framelimiter, 0, 500, ps_framelimiter ? "%d FPS" : L("no limit", u8"без ограничения"),
		ImGuiSliderFlags_AlwaysClamp);
	if (ImGui::IsItemHovered())
		help = L("The highest frame rate the game draws, 0 is no limit. Ctrl+click to type the number. With NVIDIA Reflex "
			"the driver keeps it, otherwise the engine. Saved as the console command r__framelimit.",
			u8"Наибольший FPS, который рисует игра, 0 - без ограничения. Ctrl+клик - ввести число. С NVIDIA Reflex его "
			u8"держит драйвер, иначе движок. Сохраняется как консольная команда r__framelimit.");
	ImGui::Spacing();

	section(L("MEASURED", u8"ИЗМЕРЕНИЯ"));
	if (report.cannot_measure)
		ImGui::TextDisabled("%s", L("This system cannot measure the latency: it needs Windows 10 or newer.",
			u8"Эта система не умеет измерять задержку: нужна Windows 10 или новее."));
	else
	{
		if (!in_level)
			ImGui::TextDisabled("%s", L("Measured in a level only: the main menu has its own frame limit.",
				u8"Измеряется только на уровне: у главного меню свой предел FPS."));
		const float y = ImGui::GetCursorPosY();
		string32 value;
		xr_sprintf(value, "%.1f", report.ms[chosen]);
		metric(y, L("LATENCY", u8"ЗАДЕРЖКА"), report.ms[chosen] > 0.f ? value : nullptr, L("ms", u8"мс"),
			L("Of the mode in use, the last second", u8"Текущего режима, за последнюю секунду"), true);
		xr_sprintf(value, "%.0f", report.fps[chosen]);
		metric(y, "FPS", report.ms[chosen] > 0.f ? value : nullptr, "",
			L("Frames the video card finished a second in this mode", u8"Кадров в секунду, законченных видеокартой в этом режиме"),
			false);

		if (ImGui::BeginTable("latency", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH))
		{
			ImGui::TableSetupColumn(L("MODE", u8"РЕЖИМ"), ImGuiTableColumnFlags_WidthFixed, 300.f);
			ImGui::TableSetupColumn(L("LATENCY", u8"ЗАДЕРЖКА"), ImGuiTableColumnFlags_WidthFixed, 150.f);
			ImGui::TableSetupColumn("FPS", ImGuiTableColumnFlags_WidthFixed, 100.f);
			ImGui::TableSetupColumn(L("VS OFF", u8"РАЗНИЦА С ВЫКЛ"), ImGuiTableColumnFlags_WidthFixed, 250.f);
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			for (int column = 0; column < 4; ++column)
			{
				ImGui::TableSetColumnIndex(column);
				ImGui::TextDisabled("%s", ImGui::TableGetColumnName(column));
			}
			const float off_ms = report.ms[0];
			for (u32 mode = 0; mode < latency_mode_count; ++mode)
			{
				const float ms = report.ms[mode];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushStyleColor(ImGuiCol_Text, mode == chosen ? ink : muted);
				ImGui::TextUnformatted(L(latency_modes[mode].name_en, latency_modes[mode].name_ru));
				ImGui::TableSetColumnIndex(1);
				if (ms > 0.f)
					ImGui::Text("%5.1f %s", ms, L("ms", u8"мс"));
				else
					ImGui::TextUnformatted(L("not measured", u8"не измерен"));
				ImGui::TableSetColumnIndex(2);
				if (ms > 0.f)
					ImGui::Text("%4.0f", report.fps[mode]);
				ImGui::TableSetColumnIndex(3);
				if (mode && ms > 0.f && off_ms > 0.f)
				{
					// what rounds to zero is shown as +0, not -0
					const float diff = ms - off_ms, share = diff * 100.f / off_ms;
					ImGui::Text("%+5.1f %s (%+.0f%%)", fabsf(diff) < 0.05f ? 0.f : diff, L("ms", u8"мс"),
						fabsf(share) < 0.5f ? 0.f : share);
				}
				ImGui::PopStyleColor();
			}
			ImGui::EndTable();
		}
	}
	ImGui::PopTextWrapPos();
	return help;
}

// One row of buttons for a console token; returns the help of the hovered button
const char* choices(const char* command, const choice* list, u32 count, u32 chosen)
{
	const char* help = nullptr;
	for (u32 index = 0; index < count; ++index)
	{
		if (index)
			ImGui::SameLine(0.f, 0.f);
		if (block_button(L(list[index].name_en, list[index].name_ru), index == chosen, ImVec2(content_width / count, 0.f)) &&
			index != chosen)
		{
			string64 line;
			xr_sprintf(line, "%s %s", command, list[index].token);
			Console->Execute(line);
		}
		if (ImGui::IsItemHovered())
			help = L(list[index].help_en, list[index].help_ru);
	}
	return help;
}

const char* upscaler_tab()
{
	const char* help = nullptr;
	IRenderDeviceRender::upscaler_report report = {};
	Device.m_pRender->UpscalerReport(report);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);

	section(L("UPSCALER", u8"АПСКЕЙЛЕР"));
	if (const char* h = choices("r__upscaler", upscaler_methods, 3, report.method))
		help = h;
	ImGui::Spacing();

	if (report.method == 1)
	{
		section(L("DLSS MODEL", u8"МОДЕЛЬ DLSS"));
		if (const char* h = choices("r__dlss_model", dlss_models, 4, report.model))
			help = h;
		ImGui::Spacing();
		section(L("QUALITY", u8"КАЧЕСТВО"));
		if (const char* h = choices("r__upscaler_mode", upscaler_modes, 5, report.mode))
			help = h;
		ImGui::Spacing();
	}
	else if (report.method == 2)
	{
		section(L("QUALITY", u8"КАЧЕСТВО"));
		if (const char* h = choices("r__upscaler_mode", fsr_modes, 5, report.mode))
			help = h;
		ImGui::Spacing();
	}
	if (report.method)
	{
		section(L("SHARPNESS", u8"РЕЗКОСТЬ"));
		float sharpness = report.sharpness;
		ImGui::SetNextItemWidth(content_width);
		ImGui::SliderFloat("##sharpness", &sharpness, 0.f, 1.f, sharpness > 0.f ? "%.2f" : L("off", u8"выкл"),
			ImGuiSliderFlags_AlwaysClamp);
		if (ImGui::IsItemEdited())
		{
			string64 line;
			xr_sprintf(line, "%s %.2f", report.method == 1 ? "r__dlss_sharpness" : "r__fsr_sharpness", sharpness);
			Console->Execute(line);
		}
		if (ImGui::IsItemHovered())
			help = report.method == 1 ?
				L("Sharpening after DLSS, 0 is off: AMD's RCAS, the same as FSR's (NVIDIA removed DLSS's own sharpening in "
					"DLSS 2.5.1). DLSS 4 and 4.5 are sharp already, DLSS 3 is softer. Ctrl+click to type the number. Applied "
					"at once, without rebuilding the targets.",
					u8"Повышение резкости после DLSS, 0 - выключено: AMD RCAS, как у FSR (свою резкость NVIDIA убрала из "
					u8"DLSS в версии 2.5.1). DLSS 4 и 4.5 и так чёткие, DLSS 3 мягче. Ctrl+клик - ввести число. Действует "
					u8"сразу, без пересоздания буферов.") :
				L("The sharpening of FSR (RCAS) after the upscale, 0 is off. Ctrl+click to type the number. Applied at "
					"once, without rebuilding the targets.",
					u8"Повышение резкости FSR (RCAS) после апскейла, 0 - выключено. Ctrl+клик - ввести число. Действует "
					u8"сразу, без пересоздания буферов.");
		ImGui::Spacing();
	}

	section(L("STATE", u8"СОСТОЯНИЕ"));
	if (report.nvidia && report.method == 1)
		ImGui::TextDisabled("%s", !report.gpu_known ? L("Video card: NVIDIA, generation unknown (AUTO takes DLSS 3).",
			u8"Видеокарта: NVIDIA, поколение неизвестно (АВТО берёт DLSS 3).") : report.new_gpu ?
			L("Video card: GeForce RTX 40 or newer, with FP8: every DLSS model is fast.",
				u8"Видеокарта: GeForce RTX 40 или новее, с FP8: все модели DLSS быстрые.") :
			L("Video card: GeForce RTX 20 or 30, without FP8: DLSS 3 gives the most FPS.",
				u8"Видеокарта: GeForce RTX 20 или 30, без FP8: больше всего FPS даёт DLSS 3."));
	switch (report.state)
	{
	case IRenderDeviceRender::upscaler_active:
		if (report.preset)
			ImGui::Text(L("Works: the scene %ux%u, the screen %ux%u, %s (NVIDIA's preset %c).",
				u8"Работает: сцена %ux%u, экран %ux%u, %s (пресет NVIDIA %c)."), report.render_w, report.render_h,
				report.display_w, report.display_h,
				report.preset == 'E' ? "DLSS 3" : report.preset == 'K' ? "DLSS 4" : "DLSS 4.5", report.preset);
		else
			ImGui::Text(L("Works: the scene %ux%u, the screen %ux%u, AMD FSR 3.1.",
				u8"Работает: сцена %ux%u, экран %ux%u, AMD FSR 3.1."), report.render_w, report.render_h,
				report.display_w, report.display_h);
		break;
	case IRenderDeviceRender::upscaler_no_nvidia:
		ImGui::TextColored(accent, "%s", L("DLSS needs an NVIDIA GeForce RTX video card: the scene renders at full resolution.",
			u8"DLSS нужна видеокарта NVIDIA GeForce RTX: сцена рисуется в полном разрешении."));
		break;
	case IRenderDeviceRender::upscaler_old_driver:
		ImGui::TextColored(accent, "%s", L("The video card driver is too old for DLSS: update it.",
			u8"Драйвер видеокарты слишком старый для DLSS: обновите его."));
		break;
	case IRenderDeviceRender::upscaler_no_dlss:
		ImGui::TextColored(accent, "%s", L("This video card cannot run DLSS (it needs a GeForce RTX), or nvngx_dlss.dll is "
			"missing beside the game's executable.",
			u8"Эта видеокарта не умеет DLSS (нужна GeForce RTX), или рядом с exe игры нет nvngx_dlss.dll."));
		break;
	case IRenderDeviceRender::upscaler_no_motion_vectors:
		ImGui::TextColored(accent, "%s", L("Screen Space Shaders 23 are not installed: the upscaler has no motion vectors, "
			"the scene renders at full resolution.",
			u8"Не установлены Screen Space Shaders 23: у апскейлера нет векторов движения, сцена рисуется в полном "
			u8"разрешении."));
		break;
	case IRenderDeviceRender::upscaler_msaa:
		ImGui::TextColored(accent, "%s", L("The upscaler does not work together with MSAA: turn MSAA off in the video settings.",
			u8"Апскейлер не работает вместе с MSAA: выключите MSAA в настройках видео."));
		break;
	case IRenderDeviceRender::upscaler_failed:
		ImGui::TextColored(accent, "%s", L("The upscaler stopped with an error, the details are in the log.",
			u8"Апскейлер остановился с ошибкой, подробности в логе."));
		break;
	default:
		ImGui::TextDisabled("%s", L("Off: the scene renders at the resolution of the screen.",
			u8"Выключен: сцена рисуется в разрешении экрана."));
	}
	ImGui::PopTextWrapPos();
	return help;
}

// The wheel moves the tab three lines a notch and the tab glides there (ImGui jumps at once). Whole pixels: a step never
// rounds to nothing.
void smooth_scroll()
{
	static float target = 0.f;
	static bool gliding = false;
	const ImGuiIO& io = ImGui::GetIO();
	const float now = ImGui::GetScrollY();
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) // the scrollbar was grabbed
		gliding = false;
	if (io.MouseWheel != 0.f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
	{
		const float notches = _min(_max(io.MouseWheel, -2.f), 2.f);
		target = roundf((gliding ? target : now) - notches * 3.f * ImGui::GetTextLineHeightWithSpacing());
		gliding = true;
	}
	if (!gliding)
		return;
	target = _min(_max(target, 0.f), floorf(ImGui::GetScrollMaxY()));
	const float step = (target - now) * _min(io.DeltaTime * 12.f, 1.f);
	const float next = now + (step > 0.f ? ceilf(step) : floorf(step));
	gliding = next != target;
	ImGui::SetScrollY(next);
}

// The tab's content fades out over 32 px where it goes on past the top or bottom edge of the scroll, as XRAY-TWEAKER's
// ScrollContentFade: the alpha of the vertices drawn since first_vertex, so the background shows through
void fade_scroll_edges(int first_vertex)
{
	const float scroll = ImGui::GetScrollY();
	const bool top = scroll > 0.5f, bottom = ImGui::GetScrollMaxY() - scroll > 0.5f;
	if (!top && !bottom)
		return;
	const ImRect clip = ImGui::GetCurrentWindow()->InnerClipRect;
	const float length = _min(32.f, clip.GetHeight() * 0.25f);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	for (int index = first_vertex; index < draw->VtxBuffer.Size; ++index)
	{
		ImDrawVert& v = draw->VtxBuffer[index];
		float k = 1.f;
		if (top)
			k = _min(k, (v.pos.y - clip.Min.y) / length);
		if (bottom)
			k = _min(k, (clip.Max.y - v.pos.y) / length);
		if (k >= 1.f)
			continue;
		const u32 alpha = u32(float((v.col >> IM_COL32_A_SHIFT) & 0xFF) * _max(k, 0.f) + 0.5f);
		v.col = (v.col & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
	}
}

// The logo behind everything, faint: at its own pixel size when the window has its full height
void logo(ide& owner)
{
	u32 width, height;
	void* texture = owner.PanelLogo(width, height);
	if (!texture)
		return;
	const ImVec2 at = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
	const float side = _min(float(height), size.y * 0.73f);
	const ImVec2 centre(at.x + size.x * 0.5f, at.y + size.y * 0.5f);
	ImGui::GetWindowDrawList()->AddImage(ImTextureID(uintptr_t(texture)), ImVec2(centre.x - side * 0.5f, centre.y - side * 0.5f),
		ImVec2(centre.x + side * 0.5f, centre.y + side * 0.5f), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), IM_COL32(255, 255, 255, 14));
}

void draw(ide& owner)
{
	push_style();
	// One size for FHD, QHD and 4K, lower only on a smaller screen; the tab scrolls inside it.
	// Top right: overlays such as RTSS take the top left corner.
	const ImGuiStyle& style = ImGui::GetStyle();
	const ImVec2 screen = ImGui::GetMainViewport()->WorkSize;
	const ImVec2 size(content_width + style.ScrollbarSize + 2.f * style.WindowPadding.x, _min(660.f, screen.y - 80.f));
	ImGui::SetNextWindowPos(ImVec2(screen.x - 40.f, 40.f), ImGuiCond_FirstUseEver, ImVec2(1.f, 0.f));
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::Begin("QUARK-PANEL", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	// kept on the screen: a position saved at another resolution or dragged past an edge would hide the title
	const ImVec2 at = ImGui::GetWindowPos();
	const ImVec2 kept(_max(_min(at.x, screen.x - size.x), 0.f), _max(_min(at.y, screen.y - size.y), 0.f));
	if (kept.x != at.x || kept.y != at.y)
		ImGui::SetWindowPos(kept);

	logo(owner);
	title(owner);
	ImGui::Spacing();
	metrics();
	ImGui::Spacing();
	tabs();

	const char* help = nullptr;
	bool open_warning = false;
	// The tab between the fixed header and the help lines; the scrollbar is always there, so the width never changes
	const float footer = 4.f * ImGui::GetTextLineHeight() + 2.f * style.ItemSpacing.y + 1.f;
	ImGui::BeginChild("tab", ImVec2(0.f, -footer), ImGuiChildFlags_None,
		ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	const int first_vertex = ImGui::GetWindowDrawList()->VtxBuffer.Size; // after the scrollbar
	switch (current_tab)
	{
	case tab_tasks: help = tasks_tab(open_warning); break;
	case tab_threads: threads_tab(shown.fps, shown.frame_ms); break;
	case tab_latency: help = latency_tab(); break;
	case tab_upscaler: help = upscaler_tab(); break;
	}
	smooth_scroll();
	fade_scroll_edges(first_vertex);
	ImGui::EndChild();
	if (open_warning)
		ImGui::OpenPopup("WARNING");
	warning();

	ImGui::Separator();
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
	ImGui::TextUnformatted(help ? help : tab_help());
	ImGui::PopTextWrapPos();

	ImGui::End();
	pop_style();
}
} // namespace

namespace quark_mt
{
const char* task_name(mt_tasks::task task)
{
	switch (task)
	{
	case mt_tasks::job_scheduler: return L("NPC and A-Life scheduler", u8"Планировщик NPC и A-Life");
	case mt_tasks::job_lua_prelude: return L("Actor and hear Lua", u8"Lua актёра и слуха");
	case mt_tasks::job_frame_mt: return L("Physics world and network", u8"Мир физики и сеть");
	default: break;
	}
	for (const row& r : rows)
		if (r.task == task)
			return L(r.name_en, r.name_ru);
	return "?";
}
} // namespace quark_mt

void mt_panel_frame(ide& owner, bool open)
{
	quark_mt::threads_frame(open);
	if (!open)
	{
		mt_tasks::counting = false;
		pending = nullptr;
		return;
	}
	if (mt_tasks::counting)
		take_frame();
	else
		start_counting();
	draw(owner);
}
} // namespace xr_imgui
