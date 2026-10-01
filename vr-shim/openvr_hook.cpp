#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <type_traits>

#include "vendor/openvr.h"
#include "vendor/detours/detours.h"

#if !defined(_MSC_VER) || !defined(_M_IX86)
#error This hook requires MSVC x86 to match the game's C++ OpenVR ABI.
#endif

namespace
{
using namespace vr;

using InitFunction = uint32_t (__cdecl *)(EVRInitError *, EVRApplicationType);
using InterfaceFunction = void *(__cdecl *)(const char *, EVRInitError *);
using ShutdownFunction = void (__cdecl *)();

constexpr DWORD kPathCapacity = 32768;
HMODULE g_shimModule = nullptr;
INIT_ONCE g_settingsOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK g_runtimeLock = SRWLOCK_INIT;
SRWLOCK g_logLock = SRWLOCK_INIT;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
wchar_t g_directory[kPathCapacity] = {};
InitFunction g_init = &VR_InitInternal;
InterfaceFunction g_getInterface = &VR_GetGenericInterface;
ShutdownFunction g_shutdown = &VR_ShutdownInternal;
EVRInitError g_loadError = VRInitError_None;
bool g_settingsReady = false;
bool g_importsRestored = false;
IVRSystem *g_nativeSystem = nullptr;
bool g_enabled = true;
bool g_interfaceLogged = false;
std::atomic<unsigned int> g_rawLogged{0};
unsigned long g_session = 0;

class ExclusiveLock
{
public:
    explicit ExclusiveLock(SRWLOCK &lock) noexcept : lock_(lock)
    {
        AcquireSRWLockExclusive(&lock_);
    }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
    ExclusiveLock(const ExclusiveLock &) = delete;
    ExclusiveLock &operator=(const ExclusiveLock &) = delete;
private:
    SRWLOCK &lock_;
};

void WriteLogBytes(const char *text, DWORD length)
{
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(g_logFile, text, length, &written, nullptr);
    }
}

void Log(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0)
        return;
    const DWORD count = static_cast<DWORD>(
        length < static_cast<int>(sizeof(line)) ? length : sizeof(line) - 1);
    ExclusiveLock lock(g_logLock);
    WriteLogBytes(line, count);
    WriteLogBytes("\r\n", 2);
}

void LogPath(const char *label, const wchar_t *path)
{
    // Shared scratch space avoids a large stack buffer and any heap allocation.
    static char utf8[kPathCapacity * 3];
    ExclusiveLock lock(g_logLock);
    const int count = WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8,
        static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    WriteLogBytes(label, static_cast<DWORD>(std::strlen(label)));
    if (count > 0)
        WriteLogBytes(utf8, static_cast<DWORD>(count - 1));
    else
        WriteLogBytes("<path conversion failed>", 24);
    WriteLogBytes("\r\n", 2);
}

bool SiblingPath(wchar_t (&path)[kPathCapacity], const wchar_t *filename)
{
    const size_t directoryLength = std::wcslen(g_directory);
    const size_t filenameLength = std::wcslen(filename);
    if (directoryLength + filenameLength >= kPathCapacity)
        return false;
    std::wmemcpy(path, g_directory, directoryLength);
    std::wmemcpy(path + directoryLength, filename, filenameLength + 1);
    return true;
}

BOOL CALLBACK LoadSettings(PINIT_ONCE, PVOID, PVOID *)
{
    const DWORD length = GetModuleFileNameW(g_shimModule, g_directory, kPathCapacity);
    if (length == 0 || length >= kPathCapacity)
    {
        g_loadError = VRInitError_Init_Internal;
        OutputDebugStringW(L"CCD OpenVR hook: cannot determine hook module path.\n");
        return TRUE;
    }
    wchar_t *slash = std::wcsrchr(g_directory, L'\\');
    if (!slash)
    {
        g_loadError = VRInitError_Init_Internal;
        OutputDebugStringW(L"CCD OpenVR hook: module path is not absolute.\n");
        return TRUE;
    }
    slash[1] = L'\0';

    wchar_t path[kPathCapacity];
    if (!SiblingPath(path, L"ccd_vr_projection.log"))
    {
        g_loadError = VRInitError_Init_Internal;
        OutputDebugStringW(L"CCD OpenVR hook: adjacent file path is too long.\n");
        return TRUE;
    }
    g_logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE)
        OutputDebugStringW(L"CCD OpenVR hook: cannot open local diagnostic log.\n");

    Log("process pid=%lu architecture=x86 integration=detours imports_restored=%u",
        GetCurrentProcessId(), g_importsRestored ? 1u : 0u);
    const DWORD processLength = GetModuleFileNameW(nullptr, path, kPathCapacity);
    if (processLength != 0 && processLength < kPathCapacity)
        LogPath("process executable=", path);
    const HMODULE nativeModule = GetModuleHandleW(L"openvr_api.dll");
    if (nativeModule)
    {
        const DWORD nativeLength = GetModuleFileNameW(nativeModule, path, kPathCapacity);
        if (nativeLength != 0 && nativeLength < kPathCapacity)
            LogPath("native=", path);
    }

    if (!SiblingPath(path, L"ccd_vr_projection.ini"))
    {
        g_loadError = VRInitError_Init_Internal;
        Log("load failed: config path is too long; error=%d", g_loadError);
        return TRUE;
    }
    // Read once for this DLL lifetime. A/B changes require a complete game restart.
    g_enabled = GetPrivateProfileIntW(L"Projection", L"Enabled", 1, path) != 0;
    Log("mode=%s Enabled=%u", g_enabled ? "CCD raw vertical correction" : "pass-through",
        g_enabled ? 1u : 0u);

    g_settingsReady = true;
    Log("native OpenVR hooks active");
    return TRUE;
}

bool EnsureSettings()
{
    if (!InitOnceExecuteOnce(&g_settingsOnce, LoadSettings, nullptr, nullptr))
    {
        OutputDebugStringW(L"CCD OpenVR hook: one-time initialization failed.\n");
        return false;
    }
    return g_settingsReady;
}

EVRInitError LoadError()
{
    return g_loadError == VRInitError_None ? VRInitError_Init_Internal : g_loadError;
}

void ResetBinding()
{
    // The caller holds the exclusive runtime lock, after all wrapped calls end.
    g_nativeSystem = nullptr;
    g_interfaceLogged = false;
    g_rawLogged.store(0, std::memory_order_relaxed);
}

class NativeCall
{
public:
    NativeCall()
    {
        AcquireSRWLockShared(&g_runtimeLock);
        native_ = g_nativeSystem;
        if (!native_)
        {
            // OpenVR invalidates interfaces on shutdown. Do not manufacture
            // successful values if a caller uses one outside its lifetime.
            OutputDebugStringW(L"CCD OpenVR hook: IVRSystem used outside initialized lifetime.\n");
            std::abort();
        }
    }
    ~NativeCall() { ReleaseSRWLockShared(&g_runtimeLock); }
    IVRSystem *operator->() const noexcept { return native_; }
    NativeCall(const NativeCall &) = delete;
    NativeCall &operator=(const NativeCall &) = delete;
private:
    IVRSystem *native_;
};

// No virtual destructor or guessed vtable slots: this is exactly Valve v1.0.7's
// IVRSystem_015. MSVC emits the x86 member and aggregate-return ABI itself.
class SystemProxy final : public IVRSystem
{
public:
    void GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) override
    {
        NativeCall native;
        native->GetRecommendedRenderTargetSize(width, height);
    }
    HmdMatrix44_t GetProjectionMatrix(EVREye eye, float nearZ, float farZ) override
    {
        NativeCall native;
        return native->GetProjectionMatrix(eye, nearZ, farZ);
    }
    void GetProjectionRaw(EVREye eye, float *left, float *right, float *top, float *bottom) override
    {
        NativeCall native;
        native->GetProjectionRaw(eye, left, right, top, bottom);
        const float nativeTop = *top;
        const float nativeBottom = *bottom;
        if (g_enabled)
        {
            *top = -nativeBottom;
            *bottom = -nativeTop;
        }
        const unsigned int bit = eye == Eye_Left ? 1u : eye == Eye_Right ? 2u : 0u;
        if (bit != 0 && (g_rawLogged.load(std::memory_order_relaxed) & bit) == 0 &&
            (g_rawLogged.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
        {
            Log("session=%lu eye=%s native_raw=[%.9g %.9g %.9g %.9g] returned_raw=[%.9g %.9g %.9g %.9g]",
                g_session, eye == Eye_Left ? "left" : "right",
                *left, *right, nativeTop, nativeBottom, *left, *right, *top, *bottom);
        }
    }
    bool ComputeDistortion(EVREye eye, float u, float v, DistortionCoordinates_t *coordinates) override
    {
        NativeCall native;
        return native->ComputeDistortion(eye, u, v, coordinates);
    }
    HmdMatrix34_t GetEyeToHeadTransform(EVREye eye) override
    {
        NativeCall native;
        return native->GetEyeToHeadTransform(eye);
    }
    bool GetTimeSinceLastVsync(float *seconds, uint64_t *frameCounter) override
    {
        NativeCall native;
        return native->GetTimeSinceLastVsync(seconds, frameCounter);
    }
    int32_t GetD3D9AdapterIndex() override
    {
        NativeCall native;
        return native->GetD3D9AdapterIndex();
    }
    void GetDXGIOutputInfo(int32_t *adapterIndex) override
    {
        NativeCall native;
        native->GetDXGIOutputInfo(adapterIndex);
    }
    bool IsDisplayOnDesktop() override
    {
        NativeCall native;
        return native->IsDisplayOnDesktop();
    }
    bool SetDisplayVisibility(bool visible) override
    {
        NativeCall native;
        return native->SetDisplayVisibility(visible);
    }
    void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin origin, float predictedSeconds,
        TrackedDevicePose_t *poses, uint32_t count) override
    {
        NativeCall native;
        native->GetDeviceToAbsoluteTrackingPose(origin, predictedSeconds, poses, count);
    }
    void ResetSeatedZeroPose() override
    {
        NativeCall native;
        native->ResetSeatedZeroPose();
    }
    HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override
    {
        NativeCall native;
        return native->GetSeatedZeroPoseToStandingAbsoluteTrackingPose();
    }
    HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override
    {
        NativeCall native;
        return native->GetRawZeroPoseToStandingAbsoluteTrackingPose();
    }
    uint32_t GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass deviceClass,
        TrackedDeviceIndex_t *indices, uint32_t count, TrackedDeviceIndex_t relativeTo) override
    {
        NativeCall native;
        return native->GetSortedTrackedDeviceIndicesOfClass(deviceClass, indices, count, relativeTo);
    }
    EDeviceActivityLevel GetTrackedDeviceActivityLevel(TrackedDeviceIndex_t device) override
    {
        NativeCall native;
        return native->GetTrackedDeviceActivityLevel(device);
    }
    void ApplyTransform(TrackedDevicePose_t *output, const TrackedDevicePose_t *pose,
        const HmdMatrix34_t *transform) override
    {
        NativeCall native;
        native->ApplyTransform(output, pose, transform);
    }
    TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole role) override
    {
        NativeCall native;
        return native->GetTrackedDeviceIndexForControllerRole(role);
    }
    ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(TrackedDeviceIndex_t device) override
    {
        NativeCall native;
        return native->GetControllerRoleForTrackedDeviceIndex(device);
    }
    ETrackedDeviceClass GetTrackedDeviceClass(TrackedDeviceIndex_t device) override
    {
        NativeCall native;
        return native->GetTrackedDeviceClass(device);
    }
    bool IsTrackedDeviceConnected(TrackedDeviceIndex_t device) override
    {
        NativeCall native;
        return native->IsTrackedDeviceConnected(device);
    }
    bool GetBoolTrackedDeviceProperty(TrackedDeviceIndex_t device, ETrackedDeviceProperty property,
        ETrackedPropertyError *error) override
    {
        NativeCall native;
        return native->GetBoolTrackedDeviceProperty(device, property, error);
    }
    float GetFloatTrackedDeviceProperty(TrackedDeviceIndex_t device, ETrackedDeviceProperty property,
        ETrackedPropertyError *error) override
    {
        NativeCall native;
        return native->GetFloatTrackedDeviceProperty(device, property, error);
    }
    int32_t GetInt32TrackedDeviceProperty(TrackedDeviceIndex_t device, ETrackedDeviceProperty property,
        ETrackedPropertyError *error) override
    {
        NativeCall native;
        return native->GetInt32TrackedDeviceProperty(device, property, error);
    }
    uint64_t GetUint64TrackedDeviceProperty(TrackedDeviceIndex_t device, ETrackedDeviceProperty property,
        ETrackedPropertyError *error) override
    {
        NativeCall native;
        return native->GetUint64TrackedDeviceProperty(device, property, error);
    }
    HmdMatrix34_t GetMatrix34TrackedDeviceProperty(TrackedDeviceIndex_t device,
        ETrackedDeviceProperty property, ETrackedPropertyError *error) override
    {
        NativeCall native;
        return native->GetMatrix34TrackedDeviceProperty(device, property, error);
    }
    uint32_t GetStringTrackedDeviceProperty(TrackedDeviceIndex_t device, ETrackedDeviceProperty property,
        char *value, uint32_t bufferSize, ETrackedPropertyError *error) override
    {
        NativeCall native;
        return native->GetStringTrackedDeviceProperty(device, property, value, bufferSize, error);
    }
    const char *GetPropErrorNameFromEnum(ETrackedPropertyError error) override
    {
        NativeCall native;
        return native->GetPropErrorNameFromEnum(error);
    }
    bool PollNextEvent(VREvent_t *event, uint32_t size) override
    {
        NativeCall native;
        return native->PollNextEvent(event, size);
    }
    bool PollNextEventWithPose(ETrackingUniverseOrigin origin, VREvent_t *event,
        uint32_t size, TrackedDevicePose_t *pose) override
    {
        NativeCall native;
        return native->PollNextEventWithPose(origin, event, size, pose);
    }
    const char *GetEventTypeNameFromEnum(EVREventType type) override
    {
        NativeCall native;
        return native->GetEventTypeNameFromEnum(type);
    }
    HiddenAreaMesh_t GetHiddenAreaMesh(EVREye eye, EHiddenAreaMeshType type) override
    {
        NativeCall native;
        return native->GetHiddenAreaMesh(eye, type);
    }
    bool GetControllerState(TrackedDeviceIndex_t device, VRControllerState_t *state,
        uint32_t size) override
    {
        NativeCall native;
        return native->GetControllerState(device, state, size);
    }
    bool GetControllerStateWithPose(ETrackingUniverseOrigin origin, TrackedDeviceIndex_t device,
        VRControllerState_t *state, uint32_t size, TrackedDevicePose_t *pose) override
    {
        NativeCall native;
        return native->GetControllerStateWithPose(origin, device, state, size, pose);
    }
    void TriggerHapticPulse(TrackedDeviceIndex_t device, uint32_t axis,
        unsigned short durationMicroseconds) override
    {
        NativeCall native;
        native->TriggerHapticPulse(device, axis, durationMicroseconds);
    }
    const char *GetButtonIdNameFromEnum(EVRButtonId button) override
    {
        NativeCall native;
        return native->GetButtonIdNameFromEnum(button);
    }
    const char *GetControllerAxisTypeNameFromEnum(EVRControllerAxisType axis) override
    {
        NativeCall native;
        return native->GetControllerAxisTypeNameFromEnum(axis);
    }
    bool CaptureInputFocus() override
    {
        NativeCall native;
        return native->CaptureInputFocus();
    }
    void ReleaseInputFocus() override
    {
        NativeCall native;
        native->ReleaseInputFocus();
    }
    bool IsInputFocusCapturedByAnotherProcess() override
    {
        NativeCall native;
        return native->IsInputFocusCapturedByAnotherProcess();
    }
    uint32_t DriverDebugRequest(TrackedDeviceIndex_t device, const char *request,
        char *response, uint32_t responseSize) override
    {
        NativeCall native;
        return native->DriverDebugRequest(device, request, response, responseSize);
    }
    EVRFirmwareError PerformFirmwareUpdate(TrackedDeviceIndex_t device) override
    {
        NativeCall native;
        return native->PerformFirmwareUpdate(device);
    }
    void AcknowledgeQuit_Exiting() override
    {
        NativeCall native;
        native->AcknowledgeQuit_Exiting();
    }
    void AcknowledgeQuit_UserPrompt() override
    {
        NativeCall native;
        native->AcknowledgeQuit_UserPrompt();
    }
};

static_assert(!std::is_abstract<SystemProxy>::value, "Every IVRSystem_015 method must be forwarded.");
SystemProxy g_systemProxy;
} // namespace

extern "C" uint32_t __cdecl CCD_VR_InitInternal(vr::EVRInitError *error,
    vr::EVRApplicationType applicationType)
{
    if (!EnsureSettings())
    {
        if (error)
            *error = LoadError();
        return 0;
    }
    ExclusiveLock lock(g_runtimeLock);
    ResetBinding();
    ++g_session;
    vr::EVRInitError nativeError = vr::VRInitError_None;
    const uint32_t token = g_init(&nativeError, applicationType);
    if (error)
        *error = nativeError;
    Log("session=%lu init application_type=%d error=%d token=%u", g_session,
        applicationType, nativeError, token);
    return token;
}

extern "C" void *__cdecl CCD_VR_GetGenericInterface(const char *version, vr::EVRInitError *error)
{
    if (!EnsureSettings())
    {
        if (error)
            *error = LoadError();
        return nullptr;
    }
    ExclusiveLock lock(g_runtimeLock);
    vr::EVRInitError nativeError = vr::VRInitError_None;
    void *native = g_getInterface(version, &nativeError);
    if (error)
        *error = nativeError;
    // In particular, FnTable:IVRSystem_015 and all other versions pass through.
    if (!native || nativeError != vr::VRInitError_None || !version ||
        std::strcmp(version, vr::IVRSystem_Version) != 0)
        return native;

    g_nativeSystem = static_cast<vr::IVRSystem *>(native);
    if (!g_interfaceLogged)
    {
        g_interfaceLogged = true;
        Log("session=%lu intercepted=%s native=%p proxy=%p", g_session, version,
            native, static_cast<void *>(&g_systemProxy));
    }
    return static_cast<vr::IVRSystem *>(&g_systemProxy);
}

extern "C" void __cdecl CCD_VR_ShutdownInternal()
{
    // A diagnostic/configuration failure must not prevent native shutdown.
    EnsureSettings();
    ExclusiveLock lock(g_runtimeLock);
    ResetBinding();
    Log("session=%lu shutdown begin", g_session);
    g_shutdown();
    Log("session=%lu shutdown complete", g_session);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH || DetourIsHelperProcess())
        return TRUE;

    g_shimModule = module;
    // Detours adds this DLL to the child's imports in memory before startup.
    // Restore those headers before CCD runs. A direct diagnostic LoadLibrary
    // has no such payload, so FALSE is valid for that loading path.
    g_importsRestored = DetourRestoreAfterWith() != FALSE;

    // Returned interface objects and native entrypoint detours must remain
    // valid until process exit; unloading this DLL mid-session is unsupported.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned))
        return FALSE;

    LONG status = DetourTransactionBegin();
    if (status == NO_ERROR)
    {
        status = DetourUpdateThread(GetCurrentThread());
        if (status == NO_ERROR)
            status = DetourAttach(reinterpret_cast<PVOID *>(&g_init),
                reinterpret_cast<PVOID>(&CCD_VR_InitInternal));
        if (status == NO_ERROR)
            status = DetourAttach(reinterpret_cast<PVOID *>(&g_getInterface),
                reinterpret_cast<PVOID>(&CCD_VR_GetGenericInterface));
        if (status == NO_ERROR)
            status = DetourAttach(reinterpret_cast<PVOID *>(&g_shutdown),
                reinterpret_cast<PVOID>(&CCD_VR_ShutdownInternal));
        if (status == NO_ERROR)
            status = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }
    if (status != NO_ERROR)
    {
        char message[128];
        std::snprintf(message, sizeof(message),
            "CCD OpenVR hook: startup detour transaction failed, win32=%ld.\n", status);
        OutputDebugStringA(message);
        SetLastError(static_cast<DWORD>(status));
        return FALSE;
    }
    // No file I/O, SteamVR initialization, or /MT thread-notification suppression
    // here. Configuration/logging are deferred until the first intercepted call.
    return TRUE;
}
