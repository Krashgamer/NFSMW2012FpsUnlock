#pragma once

// Session log: MW2012FpsUnlock.log next to the .asi (or in %TEMP% when that
// folder is not writable). The previous session's log is kept as
// MW2012FpsUnlock.log.old. Lines go straight to the file without buffering,
// so nothing is lost when the game crashes.
//
// INI LogLevel: 0 = off, 1 = normal (default), 2 = verbose.

enum class LogLevel { Error, Warn, Info, Verbose };

void LogOpen(int level);
bool LogEnabled(LogLevel level);
void Log(LogLevel level, const char* fmt, ...);

// Unprefixed text for multi-line reports (crash reports).
void LogRaw(const char* text);

#define LOG_ERROR(...)   Log(LogLevel::Error, __VA_ARGS__)
#define LOG_WARN(...)    Log(LogLevel::Warn, __VA_ARGS__)
#define LOG_INFO(...)    Log(LogLevel::Info, __VA_ARGS__)
#define LOG_VERBOSE(...) Log(LogLevel::Verbose, __VA_ARGS__)
