#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <detours.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cwchar>
#include <new>
#include <string>

#if !defined(_M_IX86)
#error The CCD launcher must be built for x86.
#endif

namespace {
constexpr DWORD kPathCapacity = 32768;
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_;
};

bool Fail(const wchar_t* operation, DWORD code = GetLastError())
{
    wchar_t message[2048];
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, message, static_cast<DWORD>(_countof(message)), nullptr);
    fwprintf(stderr, L"%ls failed (Win32 %lu / 0x%08lX): %ls\n", operation,
             code, code, length ? message : L"No system error text available.");
    return false;
}

bool SameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b)
{
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

bool CanonicalFilePath(const wchar_t* input, std::wstring& result,
                       BY_HANDLE_FILE_INFORMATION* identity = nullptr)
{
    wchar_t path[kPathCapacity];
    const DWORD count = GetFullPathNameW(input, kPathCapacity, path, nullptr);
    if (!count || count >= kPathCapacity)
        return Fail(L"GetFullPathNameW", count ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
    Handle file(CreateFileW(path, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return Fail(L"Opening path for canonicalization");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) return Fail(L"GetFileInformationByHandle");
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return Fail(L"Requiring a file path", ERROR_BAD_PATHNAME);
    const DWORD finalCount = GetFinalPathNameByHandleW(file.get(), path, kPathCapacity,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!finalCount || finalCount >= kPathCapacity)
        return Fail(L"GetFinalPathNameByHandleW", finalCount ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
    result.assign(path, finalCount);
    if (result.compare(0, 8, L"\\\\?\\UNC\\") == 0) result.replace(0, 8, L"\\\\");
    else if (result.compare(0, 4, L"\\\\?\\") == 0) result.erase(0, 4);
    if (identity) *identity = info;
    return true;
}

bool ExactAnsiPath(const wchar_t* path, std::string& result)
{
    const bool utf8 = GetACP() == CP_UTF8;
    BOOL substituted = FALSE;
    const DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    // Detours stores the imported DLL name in a MAX_PATH-sized ANSI buffer.
    char encoded[MAX_PATH];
    const int size = WideCharToMultiByte(CP_ACP, flags, path, -1, encoded,
        static_cast<int>(sizeof(encoded)), nullptr, utf8 ? nullptr : &substituted);
    if (size <= 0 || substituted) return false;
    wchar_t decoded[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, encoded, -1, decoded,
            static_cast<int>(_countof(decoded))) || std::wcscmp(path, decoded) != 0)
        return false;
    result.assign(encoded, static_cast<size_t>(size - 1));
    return true;
}

bool DetoursPath(const std::wstring& hook, std::string& result)
{
    if (ExactAnsiPath(hook.c_str(), result)) return true;
    wchar_t shortPath[kPathCapacity];
    const DWORD count = GetShortPathNameW(hook.c_str(), shortPath, kPathCapacity);
    if (count && count < kPathCapacity && ExactAnsiPath(shortPath, result)) return true;
    fwprintf(stderr, L"The hook path cannot be represented losslessly in the Windows ANSI code page "
                     L"within MAX_PATH, and no usable short path is available. "
                     L"Place the launcher and hook together in a shorter, ANSI-compatible path.\n");
    return Fail(L"Preparing Detours DLL path", ERROR_NO_UNICODE_TRANSLATION);
}

bool CheckProcesses(const BY_HANDLE_FILE_INFORMATION& target)
{
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return Fail(L"Reading launcher session");
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return Fail(L"Enumerating processes");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) return Fail(L"Process32FirstW");
    bool steamFound = false;
    do {
        if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0) {
            DWORD candidateSession = 0;
            if (ProcessIdToSessionId(entry.th32ProcessID, &candidateSession) && candidateSession == session)
                steamFound = true;
        }
        if (_wcsicmp(entry.szExeFile, L"Starter.exe") != 0) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!process.get()) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) continue; // Process exited after the snapshot.
            return Fail(L"Inspecting an existing Starter.exe (refusing an ambiguous launch)", error);
        }
        wchar_t image[kPathCapacity];
        DWORD size = kPathCapacity;
        if (!QueryFullProcessImageNameW(process.get(), 0, image, &size))
            return Fail(L"Reading existing Starter.exe image path");
        Handle imageFile(CreateFileW(image, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (imageFile.get() == INVALID_HANDLE_VALUE)
            return Fail(L"Opening existing Starter.exe image");
        BY_HANDLE_FILE_INFORMATION identity{};
        if (!GetFileInformationByHandle(imageFile.get(), &identity))
            return Fail(L"Identifying existing Starter.exe image");
        if (SameFile(target, identity)) {
            fwprintf(stderr, L"This CCD installation is already running (PID %lu). "
                             L"Close it before using this launcher; no process was attached.\n", entry.th32ProcessID);
            return Fail(L"Checking for an existing game", ERROR_ALREADY_EXISTS);
        }
    } while (Process32NextW(snapshot.get(), &entry));
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) return Fail(L"Process32NextW", error);
    if (!steamFound) {
        fwprintf(stderr, L"Start Steam and sign in in this Windows session first. "
                         L"This launcher does not start Steam or bypass its checks.\n");
        return Fail(L"Checking for running Steam", ERROR_NOT_READY);
    }
    return true;
}

int Launch(int argc, wchar_t** argv)
{
    if (argc != 2) {
        fwprintf(stderr, L"Usage: ccd_vr_launcher.exe \"absolute-or-relative-path\\Starter.exe\"\n");
        Fail(L"Checking arguments", ERROR_BAD_ARGUMENTS);
        return 1;
    }
    const wchar_t* inputName = wcsrchr(argv[1], L'\\');
    const wchar_t* slashName = wcsrchr(argv[1], L'/');
    if (!inputName || (slashName && slashName > inputName)) inputName = slashName;
    inputName = inputName ? inputName + 1 : argv[1];
    if (_wcsicmp(inputName, L"Starter.exe") != 0) {
        Fail(L"Refusing a non-Starter.exe target", ERROR_BAD_ARGUMENTS);
        return 1;
    }
    std::wstring target;
    BY_HANDLE_FILE_INFORMATION targetIdentity{};
    if (!CanonicalFilePath(argv[1], target, &targetIdentity)) return 1;
    const size_t separator = target.find_last_of(L'\\');
    if (separator == std::wstring::npos || _wcsicmp(target.c_str() + separator + 1, L"Starter.exe") != 0) {
        Fail(L"Refusing a resolved non-Starter.exe target", ERROR_BAD_ARGUMENTS);
        return 1;
    }
    const std::wstring workingDirectory(target, 0, separator + 1);
    DWORD binaryType = 0;
    if (!GetBinaryTypeW(target.c_str(), &binaryType)) { Fail(L"GetBinaryTypeW"); return 1; }
    if (binaryType != SCS_32BIT_BINARY) { Fail(L"Requiring an x86 Starter.exe", ERROR_BAD_EXE_FORMAT); return 1; }

    wchar_t module[kPathCapacity];
    const DWORD moduleSize = GetModuleFileNameW(nullptr, module, kPathCapacity);
    if (!moduleSize || moduleSize >= kPathCapacity) {
        Fail(L"GetModuleFileNameW", moduleSize ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
        return 1;
    }
    wchar_t* launcherSeparator = std::wcsrchr(module, L'\\');
    if (!launcherSeparator) { Fail(L"Locating launcher directory", ERROR_BAD_PATHNAME); return 1; }
    const size_t directoryLength = static_cast<size_t>(launcherSeparator - module) + 1;
    constexpr wchar_t hookName[] = L"ccd_vr_hook.dll";
    if (directoryLength + _countof(hookName) > kPathCapacity) {
        Fail(L"Locating hook DLL", ERROR_INSUFFICIENT_BUFFER);
        return 1;
    }
    std::wmemcpy(module + directoryLength, hookName, _countof(hookName));
    std::wstring hook;
    if (!CanonicalFilePath(module, hook)) return 1;
    std::string ansiHook;
    if (!DetoursPath(hook, ansiHook) || !CheckProcesses(targetIdentity)) return 1;

    // These changes affect only this launcher and its newly created child.
    if (!SetEnvironmentVariableW(L"SteamAppId", L"493490") ||
        !SetEnvironmentVariableW(L"SteamGameId", L"493490")) {
        Fail(L"Setting child Steam environment");
        return 1;
    }
    std::wstring commandLine;
    commandLine.reserve(target.size() + 2);
    commandLine.push_back(L'"');
    commandLine.append(target);
    commandLine.push_back(L'"');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!DetourCreateProcessWithDllExW(target.c_str(), commandLine.data(), nullptr, nullptr,
                                      FALSE, CREATE_SUSPENDED, nullptr, workingDirectory.c_str(),
                                      &startup, &process, ansiHook.c_str(), nullptr)) {
        // Detours owns failure cleanup, including terminating a child if injection fails.
        // Its output handles may already be closed and must not be closed again here.
        Fail(L"DetourCreateProcessWithDllExW");
        return 1;
    }
    Handle child(process.hProcess);
    Handle thread(process.hThread);
    const DWORD priorSuspendCount = ResumeThread(thread.get());
    if (priorSuspendCount != 1) {
        const DWORD error = priorSuspendCount == static_cast<DWORD>(-1) ? GetLastError() : ERROR_INVALID_STATE;
        Fail(L"Resuming the newly created game", error);
        if (!TerminateProcess(child.get(), error)) Fail(L"Terminating the failed child");
        return 1;
    }
    wprintf(L"Created and resumed CCD PID %lu.\nHook: %ls\n"
            L"Early-load injection was prepared; game readiness and VR compatibility are not verified.\n",
            process.dwProcessId, hook.c_str());
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    // Wide console output remains UTF-8 when redirected to a file or pipe.
    if (_setmode(_fileno(stdout), _O_U8TEXT) == -1 || _setmode(_fileno(stderr), _O_U8TEXT) == -1) {
        _wperror(L"Configuring UTF-8 console output");
        return 1;
    }
    try {
        return Launch(argc, argv);
    } catch (const std::bad_alloc&) {
        Fail(L"Allocating launcher memory", ERROR_NOT_ENOUGH_MEMORY);
        return 1;
    }
}
