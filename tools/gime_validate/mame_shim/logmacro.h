// GIME validation suite: stand-in for MAME's emu/logmacro.h. Same macros;
// may be included more than once per translation unit (mame_gime.cpp builds
// mc6847.cpp and gime.cpp together). VERBOSE is 0 in both, so nothing logs.
#ifndef VERBOSE
#define VERBOSE 0
#endif
#ifndef LOG_OUTPUT_FUNC
#define LOG_OUTPUT_FUNC logerror
#endif
#ifndef LOG_GENERAL
#define LOG_GENERAL (1U << 0)
#endif
#undef LOGMASKED
#undef LOG
#define LOGMASKED(mask, ...) do { if (VERBOSE & (mask)) (LOG_OUTPUT_FUNC)(__VA_ARGS__); } while (false)
#define LOG(...) LOGMASKED(LOG_GENERAL, __VA_ARGS__)
