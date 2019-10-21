#pragma once

/// https://herbsutter.com/2009/10/18/mailbag-shutting-up-compiler-warnings/
namespace core123{
template <class ... T> void unused(const T&...){}
} // namespace core123
