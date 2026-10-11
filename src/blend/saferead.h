#pragma once
#include <cstddef>
#include <cstdint>

namespace blend
{
// Reads game memory without faulting. A __try alone is not enough: every
// fault, even a handled one, passes through the crash guard's exception
// handler first, and on render worker threads that is real cost. Fill
// patterns are rejected up front and page residency is checked (cached per
// 4 KB page, rechecked every 600 frames).
bool SafeRead(uintptr_t address, void* out, size_t n);
} // namespace blend
