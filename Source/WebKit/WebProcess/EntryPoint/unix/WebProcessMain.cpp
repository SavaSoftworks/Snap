/*
 * Copyright (C) 2014 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebProcessMain.h"

#if PLATFORM(WPE) && OS(LINUX)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <link.h>
#include <sys/mman.h>

static void snapAdviseTextHugePages()
{
    const char* setting = getenv("WEBKIT_SNAP_TEXT_HUGE_PAGES");
    if (setting && !strcmp(setting, "0"))
        return;

    dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) -> int {
        if (!info->dlpi_name || !strstr(info->dlpi_name, "libWPEWebKit-2.0.so"))
            return 0;

        constexpr uintptr_t hugePageSize = 2 * 1024 * 1024;
        for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
            const auto& segment = info->dlpi_phdr[i];
            if (segment.p_type != PT_LOAD || segment.p_flags != (PF_R | PF_X))
                continue;
            uintptr_t segmentStart = info->dlpi_addr + segment.p_vaddr;
            uintptr_t start = (segmentStart + hugePageSize - 1) & ~(hugePageSize - 1);
            uintptr_t end = (segmentStart + segment.p_memsz) & ~(hugePageSize - 1);
            if (end <= start)
                continue;
            int result = madvise(reinterpret_cast<void*>(start), end - start, MADV_HUGEPAGE);
            if (getenv("WEBKIT_SNAP_EXPERIMENT_TRACE"))
                fprintf(stderr, "Snap text huge pages: advised %zu bytes result=%d\n", static_cast<size_t>(end - start), result);
        }
        return 1;
    }, nullptr);
}
#endif

int main(int argc, char** argv)
{
#if PLATFORM(WPE) && OS(LINUX)
    snapAdviseTextHugePages();
#endif
    return WebKit::WebProcessMain(argc, argv);
}
