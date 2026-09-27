#include <lib/inputscript.h>
#include <lib/gamepad.h>
#include <platform/log.h>
#include <platform/time.h>
#include <cstdio>
#include <cstring>

namespace win32
{

namespace
{

const int      MaxEvents = 64;
const uint32_t HoldMs    = 150;

struct Event
{
    uint32_t timeMs;
    char     key[8];
    int      phase; // 0 waiting, 1 held, 2 done
};

Event    s_events[MaxEvents];
int      s_count = 0;
uint64_t s_startMs = 0;

}

void InputScript::load(const char* path)
{
    s_count = 0;
    FILE* file = fopen(path, "rt");
    if (!file)
        return;
    char line[64];
    while (s_count < MaxEvents && fgets(line, sizeof(line), file))
    {
        Event& event = s_events[s_count];
        unsigned int time = 0;
        if (sscanf(line, "%u %7s", &time, event.key) != 2)
            continue;
        event.timeMs = time;
        event.phase = 0;
        s_count++;
    }
    fclose(file);
    s_startMs = pf::ticksMs();
    pf::logInfo("input script %s: %d events", path, s_count);
}

void InputScript::update()
{
    if (s_count == 0)
        return;
    uint32_t now = uint32_t(pf::ticksMs() - s_startMs);
    for (int i = 0; i < s_count; ++i)
    {
        Event& event = s_events[i];
        if (event.phase == 0 && now >= event.timeMs)
        {
            pf::logInfo("input script: %s", event.key);
            Gamepad::pushNamedKey(event.key, true);
            event.phase = 1;
        }
        else if (event.phase == 1 && now >= event.timeMs + HoldMs)
        {
            Gamepad::pushNamedKey(event.key, false);
            event.phase = 2;
        }
    }
}

}
