#include "saferead.h"
#include "../pacer.h"

#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace blend
{
namespace
{
bool Sane(uintptr_t a)
{
    if (a < 0x10000 || a >= 0x7FFE0000)
        return false;
    // Debug-heap and freed-memory fill patterns.
    const uintptr_t hi = a >> 16;
    return hi != 0xCDCD && hi != 0xFEEE && hi != 0xDDDD && hi != 0xBAAD &&
           hi != 0xABAB && hi != 0xFDFD && hi != 0xCCCC;
}

struct PageEntry
{
    uint32_t page;
    long     frame;
    bool     ok;
};
PageEntry g_pages[4096];

bool PageOk(uintptr_t a)
{
    const uint32_t page = static_cast<uint32_t>(a >> 12);
    PageEntry& c = g_pages[page & 4095];
    const long now = FrameCount();
    if (c.page == page && now - c.frame < 600)
        return c.ok;
    MEMORY_BASIC_INFORMATION mbi;
    bool ok = false;
    if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) == sizeof(mbi) &&
        mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
    {
        const DWORD p = mbi.Protect & 0xFF;
        ok = p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
             p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
             p == PAGE_EXECUTE_WRITECOPY;
    }
    c.page = page;
    c.frame = now;
    c.ok = ok;
    return ok;
}
} // namespace

bool SafeRead(uintptr_t a, void* out, size_t n)
{
    if (!Sane(a) || !Sane(a + n - 1) || !PageOk(a) || !PageOk(a + n - 1))
        return false;
    __try
    {
        memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
} // namespace blend
