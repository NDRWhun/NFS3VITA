#include <platform/config.h>
#include <platform/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pf
{

namespace
{

const int MaxEntries = 32;
const int MaxKey     = 32;
const int MaxValue   = 64;

struct Entry
{
    char key[MaxKey];
    char value[MaxValue];
};

Entry s_entries[MaxEntries];
int   s_count = 0;

const char* find(const char* key)
{
    for (int i = 0; i < s_count; ++i)
    {
        if (strcmp(s_entries[i].key, key) == 0)
            return s_entries[i].value;
    }
    return nullptr;
}

// Trims spaces and line ends in place.
void trim(char* text)
{
    char* end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = 0;
    char* start = text;
    while (*start == ' ' || *start == '\t')
        start++;
    if (start != text)
        memmove(text, start, strlen(start) + 1);
}

}

void Config::load(const char* path)
{
    s_count = 0;
    FILE* file = fopen(path, "rt");
    if (!file)
        return;
    char line[MaxKey + MaxValue + 8];
    while (s_count < MaxEntries && fgets(line, sizeof(line), file))
    {
        char* separator = strchr(line, '=');
        if (!separator || line[0] == '#')
            continue;
        *separator = 0;
        trim(line);
        trim(separator + 1);
        if (!line[0] || strlen(line) >= MaxKey || strlen(separator + 1) >= MaxValue)
            continue;
        Entry& entry = s_entries[s_count++];
        strcpy(entry.key, line);
        strcpy(entry.value, separator + 1);
        logInfo("config %s=%s", entry.key, entry.value);
    }
    fclose(file);
}

bool Config::getBool(const char* key, bool defaultValue)
{
    const char* value = find(key);
    if (!value)
        return defaultValue;
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0;
}

int Config::getInt(const char* key, int defaultValue)
{
    const char* value = find(key);
    return value ? atoi(value) : defaultValue;
}

const char* Config::getString(const char* key, const char* defaultValue)
{
    const char* value = find(key);
    return value ? value : defaultValue;
}

bool Config::has(const char* key)
{
    return find(key) != nullptr;
}

}
