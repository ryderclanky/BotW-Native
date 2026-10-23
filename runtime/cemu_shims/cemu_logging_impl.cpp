// BotW-Native: backend for Cemu's logging API (Cemu/Logging/CemuLogging.h) -> runtime log.
// Only LogType::Force messages (and APIErrors) are printed; the rest is filtered like a release Cemu.
#include "Cemu/Logging/CemuLogging.h"

void log_write(const char* fmt, ...);

void cemuLog_writeLineToLog(std::string_view text, bool date, bool new_line) {
    std::string s(text);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    log_write("[cemu] %s", s.c_str());
}
// Force and APIErrors are always on (the formatted cemuLog_log overload checks this mask first)
uint64 s_loggingFlagMask = cemuLog_getFlag(LogType::Force) | cemuLog_getFlag(LogType::APIErrors);
void cemuLog_setActiveLoggingFlags(uint64 flagMask) {
    s_loggingFlagMask = flagMask | cemuLog_getFlag(LogType::Force) | cemuLog_getFlag(LogType::APIErrors);
}
bool cemuLog_log(LogType type, std::string_view text) {
    if (type != LogType::Force && type != LogType::APIErrors && !(s_loggingFlagMask & cemuLog_getFlag(type))) return false;
    cemuLog_writeLineToLog(text);
    return true;
}
bool cemuLog_log(LogType type, std::u8string_view text) {
    return cemuLog_log(type, std::string_view((const char*)text.data(), text.size()));
}
void cemuLog_waitForFlush() {}
void cemuLog_logHexDump(LogType type, const void* data, size_t size, size_t lineSize) {}
bool cemuLog_advancedPPCLoggingEnabled() { return false; }
fs::path cemuLog_GetLogFilePath() { return {}; }
void cemuLog_createLogFile(bool) {}
