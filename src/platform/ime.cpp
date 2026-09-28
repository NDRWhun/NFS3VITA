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
    StateRequested,     // parameters set by requestOpen(), picked up by the render thread
    StateRunning,       // dialog shown, updated every rendered frame
    StateFinished       // s_result and s_text ready for takeResult()
};

std::atomic<int>  s_state{StateIdle};
std::atomic<bool> s_abort{false};       // requestClose() was called for the current request

// Written by requestOpen() while idle, read by the render thread from StateRequested until the dialog is released.
SceWChar16  s_title[MaxTitleLength + 1];
SceWChar16  s_initialText[MaxTextLength + 1];
uint32_t    s_maxLength = MaxTextLength;

// Render thread, except s_result and s_text which takeResult() reads once the state is StateFinished.
bool        s_configured = false;
bool        s_updateFailed = false;
bool        s_abortSent = false;
SceWChar16  s_input[MaxTextLength + 1];
Result      s_result = ResultNone;
char        s_text[MaxTextLength + 1];

void copyText(SceWChar16* target, const char16_t* source, uint32_t maxLength)
{
    uint32_t length = 0;
    for (; source && source[length] && length < maxLength; ++length)
        target[length] = SceWChar16(source[length]);
    target[length] = 0;
}

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

void publish(Result result)
{
    if (result != ResultConfirmed)
        s_text[0] = 0;
    s_result = result;
    s_state.store(StateFinished, std::memory_order_release);
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
    param.title = s_title;
    param.maxTextLength = s_maxLength;
    param.initialText = s_initialText;
    param.inputTextBuffer = s_input;
    int result = sceImeDialogInit(&param);
    if (result < 0)
    {
        logError("sceImeDialogInit failed: 0x%08x", result);
        publish(ResultFailed);
        return false;
    }
    s_updateFailed = false;
    s_abortSent = false;
    s_state.store(StateRunning);
    logInfo("ime dialog opened");
    return true;
}

// Reads the result, releases the dialog and publishes the result; confirmed text as printable ASCII.
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
    if (error < 0 || result.result == SCE_COMMON_DIALOG_RESULT_ABORTED)
    {
        logInfo("ime dialog aborted");
        publish(ResultFailed);
        return;
    }
    if (result.result != SCE_COMMON_DIALOG_RESULT_OK)
    {
        logInfo("ime dialog cancelled");
        publish(ResultCancelled);
        return;
    }

    uint32_t length = 0;
    for (uint32_t i = 0; i < s_maxLength && s_input[i]; ++i)
    {
        if (s_input[i] >= 0x20 && s_input[i] <= 0x7e)
            s_text[length++] = char(s_input[i]);
    }
    s_text[length] = 0;
    logInfo("ime text \"%s\"", s_text);
    publish(ResultConfirmed);
}

}

bool requestOpen(const char16_t* title, const char16_t* text, uint32_t maxLength)
{
    if (s_state.load(std::memory_order_acquire) != StateIdle)
        return false;
    s_maxLength = maxLength < 1 ? 1 : maxLength > MaxTextLength ? MaxTextLength : maxLength;
    copyText(s_title, title, MaxTitleLength);
    copyText(s_initialText, text, s_maxLength);
    s_abort.store(false);
    s_state.store(StateRequested, std::memory_order_release);
    return true;
}

void requestClose()
{
    s_abort.store(true);
}

Result takeResult(char* text, size_t size)
{
    switch (s_state.load(std::memory_order_acquire))
    {
    case StateIdle:
        return ResultNone;
    case StateFinished:
        break;
    default:
        return ResultPending;
    }
    const Result result = s_result;
    if (text && size)
        snprintf(text, size, "%s", s_text);
    s_state.store(StateIdle, std::memory_order_release);
    return result;
}

bool pending()
{
    int state = s_state.load();
    return state == StateRequested || state == StateRunning;
}

bool prepareFrame()
{
    int state = s_state.load(std::memory_order_acquire);
    if (state == StateRequested)
    {
        if (s_abort.load())
        {
            publish(ResultFailed);
            return false;
        }
        return start();
    }
    if (state != StateRunning)
        return false;
    if (s_abort.load() && !s_abortSent)
    {
        s_abortSent = true;
        int result = sceImeDialogAbort();
        if (result < 0)
            logError("sceImeDialogAbort failed: 0x%08x", result);
    }
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
        publish(ResultFailed);
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
