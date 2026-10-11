#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
HANDLE  g_file = INVALID_HANDLE_VALUE;
int     g_level = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

HANDLE OpenIn(const char* dir)
{
    char path[MAX_PATH], old[MAX_PATH];
    if (_snprintf_s(path, _TRUNCATE, "%sMW2012FpsUnlock.log", dir) < 0 ||
        _snprintf_s(old, _TRUNCATE, "%sMW2012FpsUnlock.log.old", dir) < 0)
        return INVALID_HANDLE_VALUE;
    MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
    return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Write(const char* text, size_t n)
{
    if (g_file == INVALID_HANDLE_VALUE || n == 0)
        return;
    DWORD written = 0;
    AcquireSRWLockExclusive(&g_lock);
    WriteFile(g_file, text, static_cast<DWORD>(n), &written, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
}

const char* Tag(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Error:   return "ERROR";
    case LogLevel::Warn:    return "WARN ";
    case LogLevel::Info:    return "INFO ";
    default:                return "DEBUG";
    }
}
} // namespace

void LogOpen(int level)
{
    g_level = level;
    if (level <= 0 || g_file != INVALID_HANDLE_VALUE)
        return;
    char dir[MAX_PATH]{};
    HMODULE self{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LogOpen), &self);
    GetModuleFileNameA(self, dir, MAX_PATH);
    if (char* slash = strrchr(dir, '\\'))
    {
        slash[1] = 0;
        g_file = OpenIn(dir);
    }
    if (g_file == INVALID_HANDLE_VALUE)
    {
        // Program Files is usually not writable for a game running unelevated.
        char tmp[MAX_PATH]{};
        if (GetTempPathA(MAX_PATH, tmp))
            g_file = OpenIn(tmp);
    }
}

bool LogEnabled(LogLevel level)
{
    if (g_file == INVALID_HANDLE_VALUE)
        return false;
    return level == LogLevel::Verbose ? g_level >= 2 : g_level >= 1;
}

void Log(LogLevel level, const char* fmt, ...)
{
    if (!LogEnabled(level))
        return;
    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf_s(line, _TRUNCATE, "%02u:%02u:%02u.%03u [%s] ", st.wHour, st.wMinute,
                        st.wSecond, st.wMilliseconds, Tag(level));
    if (n < 0)
        return;
    va_list args;
    va_start(args, fmt);
    const int m = _vsnprintf_s(line + n, sizeof(line) - n - 2, _TRUNCATE, fmt, args);
    va_end(args);
    n = m < 0 ? static_cast<int>(strlen(line)) : n + m;
    line[n++] = '\r';
    line[n++] = '\n';
    Write(line, static_cast<size_t>(n));
    if (level <= LogLevel::Warn)
    {
        line[n] = 0;
        OutputDebugStringA(line);
    }
}

void LogRaw(const char* text)
{
    if (!LogEnabled(LogLevel::Error))
        return;
    Write(text, strlen(text));
}
