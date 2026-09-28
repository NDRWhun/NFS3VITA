#include <platform/ime.h>
#include <platform/log.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/system_param.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace pf { namespace ime {

namespace
{

enum State
{
    StateIdle,
    StateRequested,     // set by requestOpen(), picked up by the render thread
    StateRunning,       // dialog shown, updated every rendered frame
    StateConfirmed      // s_text ready for takeText()
};

std::atomic<int> s_state{StateIdle};

// Render thread only, except s_text which takeText() reads once the state is StateConfirmed.
bool        s_configured = false;
bool        s_updateFailed = false;
SceWChar16  s_input[MaxTextLength + 1];
SceWChar16  s_initialText[1] = { 0 };
char        s_text[MaxTextLength + 1];

const char16_t s_title[] = u"Enter text";

// AppUtil and the common dialog language / enter button, as the system settings define them.
void configure()
{
    SceAppUtilInitParam initParam;
    SceAppUtilBootParam bootParam;
    memset(&initParam, 0, sizeof(initParam));
    memset(&bootParam, 0, sizeof(bootParam));
    int result = sceAppUtilInit(&initParam, &bootParam);
    if (result < 0)
        logError("sceAppUtilInit failed: 0x%08x", result);

    SceCommonDialogConfigParam config;
    sceCommonDialogConfigParamInit(&config);
    int language = 0;
    int enterButton = 0;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &language) >= 0)
        config.language = SceSystemParamLang(language);
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enterButton) >= 0)
        config.enterButtonAssign = SceSystemParamEnterButtonAssign(enterButton);
    result = sceCommonDialogSetConfigParam(&config);
    if (result < 0)
        logError("sceCommonDialogSetConfigParam failed: 0x%08x", result);
}

bool start()
{
    if (!s_configured)
    {
        configure();
        s_configured = true;
    }
    memset(s_input, 0, sizeof(s_input));

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0;
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.option = 0;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param.title = reinterpret_cast<const SceWChar16*>(s_title);
    param.maxTextLength = MaxTextLength;
    param.initialText = s_initialText;
    param.inputTextBuffer = s_input;
    int result = sceImeDialogInit(&param);
    if (result < 0)
    {
        logError("sceImeDialogInit failed: 0x%08x", result);
        s_state.store(StateIdle);
        return false;
    }
    s_updateFailed = false;
    s_state.store(StateRunning);
    logInfo("ime dialog opened");
    return true;
}

// Reads the result, releases the dialog and publishes confirmed text as printable ASCII.
void finish()
{
    SceImeDialogResult result;
    memset(&result, 0, sizeof(result));
    int error = sceImeDialogGetResult(&result);
    if (error < 0)
        logError("sceImeDialogGetResult failed: 0x%08x", error);
    int termError = sceImeDialogTerm();
    if (termError < 0)
        logError("sceImeDialogTerm failed: 0x%08x", termError);
    if (error < 0 || result.result != SCE_COMMON_DIALOG_RESULT_OK)
    {
        logInfo("ime dialog closed without text");
        s_state.store(StateIdle);
        return;
    }

    uint32_t length = 0;
    for (uint32_t i = 0; i < MaxTextLength && s_input[i]; ++i)
    {
        if (s_input[i] >= 0x20 && s_input[i] <= 0x7e)
            s_text[length++] = char(s_input[i]);
    }
    s_text[length] = 0;
    logInfo("ime text \"%s\"", s_text);
    s_state.store(StateConfirmed, std::memory_order_release);
}

}

bool requestOpen()
{
    int expected = StateIdle;
    return s_state.compare_exchange_strong(expected, StateRequested);
}

bool active()
{
    return s_state.load() != StateIdle;
}

bool takeText(char* text, size_t size)
{
    if (s_state.load(std::memory_order_acquire) != StateConfirmed)
        return false;
    snprintf(text, size, "%s", s_text);
    s_state.store(StateIdle);
    return true;
}

bool pending()
{
    int state = s_state.load();
    return state == StateRequested || state == StateRunning;
}

bool prepareFrame()
{
    int state = s_state.load();
    if (state == StateRequested)
        return start();
    if (state != StateRunning)
        return false;
    switch (sceImeDialogGetStatus())
    {
    case SCE_COMMON_DIALOG_STATUS_RUNNING:
        return true;
    case SCE_COMMON_DIALOG_STATUS_FINISHED:
        finish();
        return false;
    default:
        logError("ime dialog is no longer running");
        sceImeDialogTerm();
        s_state.store(StateIdle);
        return false;
    }
}

void drawFrame(void* colorSurface, uint32_t width, uint32_t height, uint32_t stride, SceGxmSyncObject* displaySync)
{
    SceCommonDialogUpdateParam param;
    memset(&param, 0, sizeof(param));
    param.renderTarget.colorSurfaceData = colorSurface;
    param.renderTarget.depthSurfaceData = nullptr;
    param.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
    param.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
    param.renderTarget.width = width;
    param.renderTarget.height = height;
    param.renderTarget.strideInPixels = stride;
    param.displaySyncObject = displaySync;
    int result = sceCommonDialogUpdate(&param);
    if (result < 0 && !s_updateFailed)
    {
        logError("sceCommonDialogUpdate failed: 0x%08x", result);
        s_updateFailed = true;
    }
}

void shutdown()
{
    if (s_state.load() == StateRunning)
    {
        sceImeDialogAbort();
        sceImeDialogTerm();
    }
    s_state.store(StateIdle);
    if (s_configured)
    {
        sceAppUtilShutdown();
        s_configured = false;
    }
}

}}
