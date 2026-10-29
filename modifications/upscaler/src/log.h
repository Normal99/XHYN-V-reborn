// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
#pragma once

namespace xhyn {
enum LogLevel { kInfo = 0, kWarn = 1, kError = 2 };
void logf(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
}  // namespace xhyn

#define XLOGI(...) ::xhyn::logf(::xhyn::kInfo, __VA_ARGS__)
#define XLOGW(...) ::xhyn::logf(::xhyn::kWarn, __VA_ARGS__)
#define XLOGE(...) ::xhyn::logf(::xhyn::kError, __VA_ARGS__)
