// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#include "log.h"

#include <cstdarg>
#include <cstdio>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace xhyn {

void logf(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#ifdef __ANDROID__
    static const int priority[] = {ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_vprint(priority[level], "XHYN-UPSCALE", fmt, args);
#else
    static const char* const label[] = {"info", "warn", "error"};
    char text[1024];
    vsnprintf(text, sizeof text, fmt, args);
    fprintf(stderr, "XHYN-UPSCALE %s: %s\n", label[level], text);
#endif
    va_end(args);
}

}  // namespace xhyn
