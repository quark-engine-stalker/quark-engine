#include "stdafx.h"
#pragma hdrstop

#include <time.h>
#include "resource.h"
#include "log.h"
#include "engine_error_logger.h"
#ifdef _EDITOR
#include "malloc.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "profiler.h"

extern BOOL LogExecCB = TRUE;
static string_path logFName = "engine.log";
static string_path log_file_name = "engine.log";
static BOOL no_log = TRUE;
#ifdef PROFILE_CRITICAL_SECTIONS
static xrCriticalSection logCS(MUTEX_PROFILE_ID(log));
static xrCriticalSection gameLogCS(MUTEX_PROFILE_ID(game_log_history));
#else // PROFILE_CRITICAL_SECTIONS
static xrCriticalSection logCS;
static xrCriticalSection gameLogCS;
#endif // PROFILE_CRITICAL_SECTIONS
xr_vector<xr_string> LogFile;
static LogCallback LogCB = 0;
static FILE* LogStream = nullptr;
static bool LogIoFailed = false;
static xr_string LastLogLine;
static u32 LastLogLineCount = 0;
static bool DiskLinePending = false;

namespace
{
constexpr size_t log_history_target = 8192;
constexpr size_t log_history_limit = 12288;

// The first lines of the session (system, renderer) for the error logger: LogFile drops its oldest lines.
constexpr u32 log_head_limit = 64; // up to the video card and its memory
char log_head[32768] = {};
size_t log_head_used = 0;
u32 log_head_lines = 0;
bool log_head_full = false;
u64 log_lines_total = 0; // lines LogFile got since the start (or clear_log): LogFile[0] is line total - size

void note_log_line_locked(const char* line)
{
	++log_lines_total;
	if (log_head_full)
		return;
	const size_t length = strlen(line);
	const size_t remaining = sizeof(log_head) - log_head_used;
	if (remaining < 5 || length > remaining - 5)
	{
		log_head_full = true;
		return; // Keep complete startup lines; a long line must not be clipped.
	}
	memcpy(log_head + log_head_used, "  ", 2);
	memcpy(log_head + log_head_used + 2, line, length);
	memcpy(log_head + log_head_used + 2 + length, "\r\n", 2);
	log_head_used += length + 4;
	log_head[log_head_used] = 0;
	++log_head_lines;
	if (log_head_lines == log_head_limit)
		log_head_full = true;
}

void reset_game_log_history_locked()
{
	log_head_used = 0;
	log_head_lines = 0;
	log_head_full = false;
	log_lines_total = 0;
	*log_head = 0;
}

void put_log_text(char* output, size_t capacity, size_t& used, const char* text, size_t length)
{
	if (used + 1 >= capacity)
		return;
	if (length > capacity - used - 1)
		length = capacity - used - 1;
	memcpy(output + used, text, length);
	used += length;
	output[used] = 0;
}

// The head, a gap note, then the newest lines that fit: the end of the log is what a crash needs most.
void copy_game_log(char* output, size_t capacity, unsigned int tail)
{
	if (capacity < 128)
		return;
	const size_t size = LogFile.size();
	const u64 first = log_lines_total - size;
	const bool include_head = log_head_used + 128 < capacity;
	const size_t head_used = include_head ? log_head_used : 0;
	const u32 included_head_lines = include_head ? log_head_lines : 0;
	size_t budget = capacity - head_used - 64;
	const size_t line_limit = (std::min)(size_t{4096}, budget / 2);
	size_t begin = size;
	while (begin > 0 && size - begin < tail && first + begin > included_head_lines)
	{
		const size_t length = (std::min)(LogFile[begin - 1].size(), line_limit) + 4;
		if (length > budget)
			break;
		budget -= length;
		--begin;
	}
	size_t used = 0;
	if (include_head)
		put_log_text(output, capacity, used, log_head, log_head_used);
	if (tail && first + begin > included_head_lines)
	{
		char gap[64];
		_snprintf_s(gap, sizeof(gap), _TRUNCATE, "  ... %llu lines ...\r\n", first + begin - included_head_lines);
		put_log_text(output, capacity, used, gap, strlen(gap));
	}
	for (size_t index = begin; index < size; ++index)
	{
		const xr_string& line = LogFile[index];
		put_log_text(output, capacity, used, "  ", 2);
		if (line.size() > line_limit)
		{
			constexpr char shortened[] = " ...";
			size_t length = line_limit - (sizeof(shortened) - 1);
			while (length && (static_cast<unsigned char>(line[length]) & 0xC0) == 0x80)
				--length;
			put_log_text(output, capacity, used, line.c_str(), length);
			put_log_text(output, capacity, used, shortened, sizeof(shortened) - 1);
		}
		else
			put_log_text(output, capacity, used, line.c_str(), line.size());
		put_log_text(output, capacity, used, "\r\n", 2);
	}
}

// Disk writes and callbacks hold logCS. The crash snapshot only needs the history lock.
void write_game_log(char* output, size_t capacity, unsigned int tail)
{
	*output = 0;
	if (!gameLogCS.TryEnter())
		return;
	__try { copy_game_log(output, capacity, tail); }
	__except (EXCEPTION_EXECUTE_HANDLER) { *output = 0; }
	gameLogCS.Leave();
}

void write_pending_log_line_locked()
{
	if (!DiskLinePending)
		return;

	if (!no_log && LogStream && !LogIoFailed)
	{
		xr_string compacted;
		LPCSTR line = LastLogLine.c_str();
		if (LastLogLineCount > 1)
		{
			compacted = LastLogLine;
			compacted += " [";
			compacted += std::to_string(LastLogLineCount).c_str();
			compacted += "]";
			line = compacted.c_str();
		}

		const size_t length = xr_strlen(line);
		if ((length && fwrite(line, 1, length, LogStream) != length) ||
			fwrite("\r\n", 1, 2, LogStream) != 2)
			LogIoFailed = true;
	}

	DiskLinePending = false;
}

void reset_log_run_locked()
{
	LastLogLine.clear();
	LastLogLineCount = 0;
	DiskLinePending = false;
}

void trim_log_history_locked()
{
	if (LogFile.size() <= log_history_limit)
		return;

	const size_t remove_count = LogFile.size() - log_history_target;
	LogFile.erase(LogFile.begin(), LogFile.begin() + remove_count);
}
}

void FlushLog()
{
	PROF_EVENT();
	logCS.Enter();
	write_pending_log_line_locked();
	if (!no_log && LogStream && !LogIoFailed && fflush(LogStream) != 0)
		LogIoFailed = true;

	// A flush is a durable boundary. Start duplicate collapsing from scratch so
	// later repeats never require rewriting an already committed disk line.
	reset_log_run_locked();
	logCS.Leave();
}

std::string getCurrentTimeStamp(LPCSTR format = "%d.%m.%Y %H:%M:%S") {
	using namespace std::chrono;

	// get current time
	auto now = system_clock::now();

	// get number of milliseconds for the current second
	// (remainder after division into seconds)
	auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

	// convert to std::time_t in order to convert to std::tm (broken time)
	auto timer = system_clock::to_time_t(now);

	// convert to broken time
	std::tm bt = *std::localtime(&timer);

	std::ostringstream oss;

	oss << std::put_time(&bt, format); // HH:MM:SS
	oss << '.' << std::setfill('0') << std::setw(3) << ms.count();

	return oss.str();
}

std::string timeInDMYHMSMMM()
{
	return getCurrentTimeStamp("%d.%m.%Y %H:%M:%S");
}

std::string timeInHMSMMM()
{
	return getCurrentTimeStamp("%H:%M:%S");
}

BOOL logTimestamps = FALSE;
enum Console_mark;
extern bool is_console_mark(Console_mark type);

void AddOne(const char* split)
{
	logCS.Enter();

    // Keep LogCB's original text for script/console compatibility, but never
    // persist it into the session history, disk log or crash attachments.
    const size_t safeLength = EngineErrorLogger::SanitizeText(split, nullptr, 0);
    std::string t(safeLength + 1, '\0');
    EngineErrorLogger::SanitizeText(split, &t[0], t.size());
    t.resize(safeLength);
#ifdef DEBUG
    OutputDebugString(t.c_str());
    OutputDebugString("\n");
#endif
	if (logTimestamps)
	{
		std::string c;
		if (!t.empty() && is_console_mark((Console_mark)t[0]))
		{
			c += t[0];
			c += " ";
			t.erase(0, 1);
		}
		t = c + "[" + timeInHMSMMM() + "] " + t;
	}

	if (!LastLogLine.empty() && LastLogLine == t.c_str())
	{
		++LastLogLineCount;
		DiskLinePending = true;

		xr_string compacted = LastLogLine;
		compacted += " [";
		compacted += std::to_string(LastLogLineCount).c_str();
		compacted += "]";
		xrCriticalSectionGuard history(gameLogCS);
		if (!LogFile.empty())
			LogFile.back() = std::move(compacted);
		else
		{
			LogFile.push_back(std::move(compacted));
			note_log_line_locked(LogFile.back().c_str());
			trim_log_history_locked();
		}
	}
	else
	{
		// Commit the previous run before reusing its small staging string. The
		// persistent writer is stdio-buffered, so the full session log no longer
		// needs to be mirrored in RAM.
		write_pending_log_line_locked();
		LastLogLine = t.c_str();
		LastLogLineCount = 1;
		DiskLinePending = true;
		xrCriticalSectionGuard history(gameLogCS);
		LogFile.emplace_back(t.c_str());
		note_log_line_locked(t.c_str());
		trim_log_history_locked();
	}

	// exec CallBack
	if (LogExecCB && LogCB)
		LogCB(split);

	logCS.Leave();
}

void Log(const char* s)
{
	int i, j;

	u32 length = xr_strlen(s);
#ifndef _EDITOR
	PSTR split = (PSTR)_alloca((length + 1) * sizeof(char));
#else
    PSTR split = (PSTR)alloca((length + 1) * sizeof(char));
#endif
	for (i = 0, j = 0; s[i] != 0; i++)
	{
		if (s[i] == '\n')
		{
			split[j] = 0; // end of line
			if (split[0] == 0)
			{
				split[0] = ' ';
				split[1] = 0;
			}
			AddOne(split);
			j = 0;
		}
		else
		{
			split[j++] = s[i];
		}
	}
	split[j] = 0;
	AddOne(split);
}

void __cdecl Msg(const char* format, ...)
{
	va_list mark;
	string2048 buf;
	va_start(mark, format);
	int sz = _vsnprintf(buf, sizeof(buf) - 1, format, mark);
	buf[sizeof(buf) - 1] = 0;
	va_end(mark);
	if (sz) Log(buf);
}

void Log(const char* msg, const char* dop)
{
	if (!dop)
	{
		Log(msg);
		return;
	}

	u32 buffer_size = (xr_strlen(msg) + 1 + xr_strlen(dop) + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);
	strconcat(buffer_size, buf, msg, " ", dop);
	Log(buf);
}

void Log(const char* msg, u32 dop)
{
	u32 buffer_size = (xr_strlen(msg) + 1 + 10 + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s %d", msg, dop);
	Log(buf);
}

void Log(const char* msg, int dop)
{
	u32 buffer_size = (xr_strlen(msg) + 1 + 11 + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s %i", msg, dop);
	Log(buf);
}

void Log(const char* msg, float dop)
{
	// actually, float string representation should be no more, than 40 characters,
	// but we will count with slight overhead
	u32 buffer_size = (xr_strlen(msg) + 1 + 64 + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s %f", msg, dop);
	Log(buf);
}

void Log(const char* msg, const Fvector& dop)
{
	u32 buffer_size = (xr_strlen(msg) + 2 + 3 * (64 + 1) + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s (%f,%f,%f)", msg, VPUSH(dop));
	Log(buf);
}

void Log(const char* msg, const Fmatrix& dop)
{
	u32 buffer_size = (xr_strlen(msg) + 2 + 4 * (4 * (64 + 1) + 1) + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s:\n%f,%f,%f,%f\n%f,%f,%f,%f\n%f,%f,%f,%f\n%f,%f,%f,%f\n",
	           msg,
	           dop.i.x, dop.i.y, dop.i.z, dop._14_,
	           dop.j.x, dop.j.y, dop.j.z, dop._24_,
	           dop.k.x, dop.k.y, dop.k.z, dop._34_,
	           dop.c.x, dop.c.y, dop.c.z, dop._44_
	);
	Log(buf);
}

void LogWinErr(const char* msg, long err_code)
{
	Msg("%s: %s", msg, Debug.error2string(err_code));
}

LogCallback SetLogCB(LogCallback cb)
{
	LogCallback result = LogCB;
	LogCB = cb;
	return (result);
}

LPCSTR log_name()
{
	return (log_file_name);
}

void InitLog()
{
	{
		xrCriticalSectionGuard history(gameLogCS);
		LogFile.reserve(log_history_target);
	}
	EngineErrorLogger::SetGameLogWriter(&write_game_log);
}

void CreateLog(BOOL nl)
{
	no_log = nl;
	strconcat(sizeof(log_file_name), log_file_name, Core.ApplicationName, "_", Core.UserName, ".log");
	if (FS.path_exist("$logs$"))
		FS.update_path(logFName, "$logs$", log_file_name);
	if (!no_log)
	{
		//Alun: Backup existing log
		xr_string backup_logFName = EFS.ChangeFileExt(logFName, ".bkp");
		FS.file_rename(logFName, backup_logFName.c_str(), true);
		//-Alun
		VerifyPath(logFName);
		LogStream = fopen(logFName, "wb");
		LogIoFailed = (LogStream == nullptr);
		if (!LogStream)
		{
			MessageBox(NULL, "Can't create log file.", "Error", MB_ICONERROR);
			abort();
		}
		// Keep disk writes amortized without mirroring the full session in RAM.
		setvbuf(LogStream, nullptr, _IOFBF, 256u * 1024u);
	}
}


void ClearLog()
{
	// FILE I/O cannot call back into Msg(), so keep the whole truncate/reopen
	// transition under logCS. This closes the race where another thread could
	// append while no stream was installed.
	bool reopen_failed = false;
	logCS.Enter();
	if (LogStream)
	{
		fclose(LogStream);
		LogStream = nullptr;
	}

	{
		xrCriticalSectionGuard history(gameLogCS);
		LogFile.clear_not_free();
		reset_game_log_history_locked();
	}
	reset_log_run_locked();
	LogIoFailed = false;

	if (!no_log)
	{
		LogStream = fopen(logFName, "wb");
		if (LogStream)
			setvbuf(LogStream, nullptr, _IOFBF, 256u * 1024u);
		else
		{
			LogIoFailed = true;
			reopen_failed = true;
		}
	}
	logCS.Leave();

	if (reopen_failed)
		MessageBox(NULL, "Can't recreate log file.", "Error", MB_ICONERROR);
}

void CloseLog(void)
{
	FlushLog();

	FILE* stream = nullptr;
	logCS.Enter();
	stream = LogStream;
	LogStream = nullptr;
	{
		xrCriticalSectionGuard history(gameLogCS);
		LogFile.clear_and_free();
		reset_game_log_history_locked();
	}
	reset_log_run_locked();
	logCS.Leave();

	if (stream)
		fclose(stream);
}

xr_string FormatString(LPCSTR fmt, ...)
{
	va_list mark;
	string2048 buf;
	va_start(mark, fmt);
	int sz = _vsnprintf(buf, sizeof(buf) - 1, fmt, mark);
	buf[sizeof(buf) - 1] = 0;
	va_end(mark);
	if (sz) return xr_string(buf);
	return xr_string("");
}
