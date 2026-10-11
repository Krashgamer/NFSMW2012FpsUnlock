#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#include <cstring>

Config g_config;

namespace
{
int ReadInt(const char* path, const char* key, int def)
{
    char buf[32];
    if (GetPrivateProfileStringA("FpsUnlock", key, "", buf, sizeof(buf), path) == 0)
        return def;
    char* end = nullptr;
    const long v = strtol(buf, &end, 10);
    return end == buf ? def : static_cast<int>(v);
}
} // namespace

void LoadConfig()
{
    char path[MAX_PATH]{};
    HMODULE self{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LoadConfig), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (!slash)
        return;
    strcpy_s(slash + 1, sizeof(path) - (slash + 1 - path), "MW2012FpsUnlock.ini");

    int fps = ReadInt(path, "TargetFPS", 0);
    if (fps < 0 || fps >= 10000)
        fps = 0;
    else if (fps > 0 && fps < 30)
        fps = 30;
    g_config.targetFps = fps;

    int vsync = ReadInt(path, "VsyncInterval", -1);
    if (vsync > 4)
        vsync = 4;
    g_config.vsyncInterval = vsync < 0 ? -1 : vsync;

    g_config.capLoading = ReadInt(path, "LoadingScreenCap", 1) != 0;

    g_config.borderless = ReadInt(path, "Borderless", 1) != 0;

    const int hz = ReadInt(path, "RefreshRate", 0);
    g_config.refreshRate = hz < 0 || hz > 1000 ? 0 : hz;

    const int level = ReadInt(path, "LogLevel", 1);
    g_config.logLevel = level < 0 ? 0 : (level > 2 ? 2 : level);
}
