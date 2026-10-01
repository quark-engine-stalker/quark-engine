#include "stdafx.h"
#pragma hdrstop

#include <time.h>
#include "resource.h"
#include "log.h"
#ifdef _EDITOR
#include "malloc.h"
#endif

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
#else // PROFILE_CRITICAL_SECTIONS
static xrCriticalSection logCS;
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
	PROF_EVENT("Flushing");
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

#ifdef DEBUG
	OutputDebugString(split);
	OutputDebugString("\n");
#endif

	std::string t = split;
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
		if (!LogFile.empty())
			LogFile.back() = std::move(compacted);
		else
		{
			LogFile.push_back(std::move(compacted));
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
		LogFile.emplace_back(t.c_str());
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
	LogFile.reserve(log_history_target);
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

	LogFile.clear_not_free();
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
	LogFile.clear_and_free();
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
