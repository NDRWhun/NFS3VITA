#include <platform/config.h>
#include <platform/log.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace pf
{

namespace
{

const int MaxEntries = 32;
const int MaxKey     = 32;
const int MaxValue   = 64;
const int MaxLine    = 256;

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

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Trims spaces and line ends in place; returns the trimmed start.
char* trim(char* text)
{
    while (isSpace(*text))
        text++;
    char* end = text + strlen(text);
    while (end > text && isSpace(end[-1]))
        *--end = 0;
    return text;
}

// Reads one line into line; false at the end of the file. A line longer than the buffer is consumed whole and
// returned as too long.
bool readLine(FILE* file, char* line, int size, bool* tooLong)
{
    if (!fgets(line, size, file))
        return false;
    *tooLong = false;
    if (!strchr(line, '\n') && !feof(file))
    {
        *tooLong = true;
        int c;
        while ((c = fgetc(file)) != EOF && c != '\n')
        {
        }
    }
    return true;
}

}

void Config::load(const char* path)
{
    s_count = 0;
    FILE* file = fopen(path, "rt");
    if (!file)
        return;
    char buffer[MaxLine];
    bool tooLong = false;
    for (int number = 1; readLine(file, buffer, sizeof(buffer), &tooLong); ++number)
    {
        char* line = buffer;
        if (number == 1 && strncmp(line,"\xef\xbb\xbf", 3) == 0)
            line += 3;
        if (tooLong)
        {
            logError("config line %d: longer than %d characters, ignored", number, MaxLine - 2);
            continue;
        }
        line = trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        char* separator = strchr(line, '=');
        if (!separator)
        {
            logError("config line %d: no '=', ignored", number);
            continue;
        }
        *separator = 0;
        const char* key = trim(line);
        const char* value = trim(separator + 1);
        if (!key[0] || strlen(key) >= MaxKey || strlen(value) >= MaxValue)
        {
            logError("config line %d: empty key, key over %d or value over %d characters, ignored", number,
                     MaxKey - 1, MaxValue - 1);
            continue;
        }
        if (find(key))
        {
            logError("config line %d: %s repeated, first value kept", number, key);
            continue;
        }
        if (s_count == MaxEntries)
        {
            logError("config line %d: more than %d entries, %s ignored", number, MaxEntries, key);
            continue;
        }
        Entry& entry = s_entries[s_count++];
        strcpy(entry.key, key);
        strcpy(entry.value, value);
        logInfo("config %s=%s", entry.key, entry.value);
    }
    fclose(file);
}

bool Config::tryGetBool(const char* key, bool* value)
{
    const char* text = find(key);
    if (!text)
        return false;
    static const char* const On[] = { "1", "true", "yes", "on" };
    static const char* const Off[] = { "0", "false", "no", "off" };
    for (int i = 0; i < 4; ++i)
    {
        if (strcasecmp(text, On[i]) == 0 || strcasecmp(text, Off[i]) == 0)
        {
            *value = strcasecmp(text, On[i]) == 0;
            return true;
        }
    }
    logError("config %s=%s is not a boolean (1/0, true/false, yes/no, on/off), ignored", key, text);
    return false;
}

bool Config::tryGetInt(const char* key, int* value)
{
    const char* text = find(key);
    if (!text)
        return false;
    char* end = nullptr;
    errno = 0;
    const long number = strtol(text, &end, 10);
    if (end == text || *end || errno == ERANGE || number < INT_MIN || number > INT_MAX)
    {
        logError("config %s=%s is not an integer, ignored", key, text);
        return false;
    }
    *value = int(number);
    return true;
}

bool Config::getBool(const char* key, bool defaultValue)
{
    bool value = defaultValue;
    return tryGetBool(key, &value) ? value : defaultValue;
}

int Config::getInt(const char* key, int defaultValue)
{
    int value = defaultValue;
    return tryGetInt(key, &value) ? value : defaultValue;
}

const char* Config::getString(const char* key, const char* defaultValue)
{
    const char* value = find(key);
    return value ? value : defaultValue;
}

}
