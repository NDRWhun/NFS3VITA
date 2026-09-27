#ifndef PLATFORM_CONFIG_H_
#define PLATFORM_CONFIG_H_

namespace pf
{

// Optional key=value settings file; missing file or key means the default.
class Config
{
public:
    static void load(const char* path);
    static bool getBool(const char* key, bool defaultValue);
    static int  getInt(const char* key, int defaultValue);
};

}

#endif
