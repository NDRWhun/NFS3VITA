#include <lib/inputscript.h>
#include <lib/gamepad.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <cstdio>

namespace win32
{

namespace
{

const int      MaxEvents = 64;
const uint32_t HoldMs    = 150;

struct Event
{
    uint32_t timeMs;
    uint64_t pressMs;
    char     key[8];
    int      phase; // 0 waiting, 1 held, 2 done
};

Event     s_events[MaxEvents];
int       s_count = 0;
bool      s_running = false;
uint64_t  s_startMs = 0;
pf::Mutex s_mutex("nfs3_inputscript", false);

}

void InputScript::load(const char* path)
{
    pf::LockGuard guard(s_mutex);
    s_count = 0;
    s_running = false;
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
    pf::logInfo("input script %s: %d events", path, s_count);
}

void InputScript::resume()
{
    pf::LockGuard guard(s_mutex);
    if (s_count == 0 || s_running)
        return;
    s_running = true;
    s_startMs = pf::ticksMs();
    pf::logInfo("input script clock started");
}

void InputScript::pause()
{
    pf::LockGuard guard(s_mutex);
    s_running = false;
}

void InputScript::update()
{
    pf::LockGuard guard(s_mutex);
    const uint64_t ticks = pf::ticksMs();
    const uint32_t now = uint32_t(ticks - s_startMs);
    for (int i = 0; i < s_count; ++i)
    {
        Event& event = s_events[i];
        if (event.phase == 0 && s_running && now >= event.timeMs)
        {
            pf::logInfo("input script: %s", event.key);
            Gamepad::pushNamedKey(event.key, true);
            event.pressMs = ticks;
            event.phase = 1;
        }
        else if (event.phase == 1 && ticks >= event.pressMs + HoldMs)
        {
            // Released on real time, so a paused clock never leaves a key held.
            Gamepad::pushNamedKey(event.key, false);
            event.phase = 2;
        }
    }
}

}
