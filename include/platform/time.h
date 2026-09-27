#ifndef PLATFORM_TIME_H_
#define PLATFORM_TIME_H_

#include <cstdint>

namespace pf
{

// Process-relative clocks.
uint64_t ticksUs();
uint32_t ticksMs();

void sleepMs(uint32_t milliseconds);
void sleepUs(uint32_t microseconds);

}

#endif
