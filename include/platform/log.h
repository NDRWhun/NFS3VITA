#ifndef PLATFORM_LOG_H_
#define PLATFORM_LOG_H_

namespace pf
{

// Console output plus an optional log file under the data directory.
void logInit(const char* logFilePath);
void logShutdown();

void logDebug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logInfo(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Logs, flushes and terminates the process.
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}

#endif
