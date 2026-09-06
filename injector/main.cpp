#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

DWORD findProcessId(const wchar_t* processName) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
}

std::filesystem::path ownDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    return std::filesystem::path(path).parent_path();
}

void printWinError(const char* label) {
    const DWORD error = GetLastError();
    std::cerr << "[-] " << label << " falhou. GetLastError=" << error << "\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wcout << L"MineXRay Injector - Minecraft Bedrock 1.26.4501.0 / 26.45\n";
    std::wcout << L"Alvo: Minecraft.Windows.exe\n\n";

    std::filesystem::path dllPath;
    if (argc >= 2) {
        dllPath = std::filesystem::absolute(argv[1]);
    } else {
        dllPath = ownDirectory() / L"MineXRay.dll";
    }

    if (!std::filesystem::exists(dllPath)) {
        std::wcerr << L"[-] MineXRay.dll nao encontrada: " << dllPath.wstring() << L"\n";
        std::wcerr << L"    Deixe MineXRayInjector.exe e MineXRay.dll na mesma pasta.\n";
        return 1;
    }

    const DWORD pid = findProcessId(L"Minecraft.Windows.exe");
    if (!pid) {
        std::wcerr << L"[-] Minecraft.Windows.exe nao encontrado. Abra o Minecraft primeiro.\n";
        return 2;
    }

    std::wcout << L"[+] PID: " << pid << L"\n";
    std::wcout << L"[+] DLL: " << dllPath.wstring() << L"\n";

    const DWORD access = PROCESS_CREATE_THREAD
        | PROCESS_QUERY_INFORMATION
        | PROCESS_VM_OPERATION
        | PROCESS_VM_WRITE
        | PROCESS_VM_READ;

    HANDLE process = OpenProcess(access, FALSE, pid);
    if (!process) {
        printWinError("OpenProcess");
        std::cerr << "    Tente executar o injector como administrador.\n";
        return 3;
    }

    const std::wstring dll = dllPath.wstring();
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);

    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        printWinError("VirtualAllocEx");
        CloseHandle(process);
        return 4;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dll.c_str(), bytes, &written) || written != bytes) {
        printWinError("WriteProcessMemory");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 5;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto* loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (!loadLibrary) {
        printWinError("GetProcAddress(LoadLibraryW)");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 6;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!thread) {
        printWinError("CreateRemoteThread");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 7;
    }

    const DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait != WAIT_OBJECT_0) {
        std::cerr << "[-] Timeout esperando LoadLibraryW.\n";
        CloseHandle(thread);
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 8;
    }

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);

    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    if (exitCode == 0) {
        std::cerr << "[-] LoadLibraryW retornou 0. A DLL nao foi carregada.\n";
        return 9;
    }

    std::cout << "\n[+] MineXRay.dll injetada.\n";
    std::cout << "[+] F6 = liga/desliga XRay\n";
    std::cout << "[+] F7 = diagnostico\n";
    std::cout << "[+] F8 = tenta reconstruir chunks\n";
    std::cout << "[+] F12 = descarrega a DLL\n";
    std::cout << "[+] Log: %TEMP%\\MineXRay.log\n";
    return 0;
}
