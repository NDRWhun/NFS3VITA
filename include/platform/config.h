#ifndef PLATFORM_CONFIG_H_
#define PLATFORM_CONFIG_H_

namespace pf
{

// Optional key=value settings file; missing file or key means the default. Lines starting with # are comments.
// Invalid lines and values are logged and ignored.
class Config
{
public:
    static void load(const char* path);
    // False if the key is missing or its value invalid (logged); value is then unchanged.
    // Booleans: 1/0, true/false, yes/no, on/off in any case. Integers: decimal.
    static bool tryGetBool(const char* key, bool* value);
    static bool tryGetInt(const char* key, int* value);
    static bool getBool(const char* key, bool defaultValue);
    static int  getInt(const char* key, int defaultValue);
    static const char* getString(const char* key, const char* defaultValue);
};

}

#endif
