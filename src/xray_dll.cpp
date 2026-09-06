#include <windows.h>

#include "MineXRayCore.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace ox = mine_mod::xray::offsets_1_26_4501;

HMODULE g_self = nullptr;
HMODULE g_minecraft = nullptr;
FILE* g_log = nullptr;

std::atomic_bool g_xrayEnabled{false};
std::atomic_bool g_unloading{false};
std::vector<std::string> g_visibleTokens;

struct CodePatch {
    std::uint8_t* address = nullptr;
    std::array<std::uint8_t, 12> original{};
    bool installed = false;
};

CodePatch g_renderLayerPatch;
std::int32_t g_renderLayerFieldOffset = -1;
void** g_blockLegacyVtable = nullptr;
std::size_t g_renderLayerVtableIndex = static_cast<std::size_t>(-1);
void* g_clientInstance = nullptr;

using RebuildChunkFn = void(__fastcall*)(void*);
RebuildChunkFn g_rebuildChunk = nullptr;

void logLine(const char* format, ...) {
    char buffer[1600]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);

    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
    if (g_log) {
        std::fprintf(g_log, "%s\n", buffer);
        std::fflush(g_log);
    }
}

void openLog() {
    wchar_t tempPath[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempPath)) return;
    std::wstring path = tempPath;
    path += L"MineXRay.log";
    _wfopen_s(&g_log, path.c_str(), L"w");
}

std::filesystem::path moduleDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    return std::filesystem::path(path).parent_path();
}

std::string trim(std::string text) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::string lowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

void loadVisibleTokens() {
    g_visibleTokens.clear();
    const auto path = moduleDirectory() / L"xray-blocks.txt";
    std::ifstream input(path);

    if (input) {
        std::string line;
        while (std::getline(input, line)) {
            const auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            line = lowerCopy(trim(line));
            if (!line.empty()) g_visibleTokens.push_back(line);
        }
    }

    if (g_visibleTokens.empty()) {
        g_visibleTokens = {
            "ore",
            "ancient_debris",
            "lava",
            "water",
            "chest",
            "spawner",
            "amethyst",
            "vault"
        };
    }

    logLine("[+] Visible tokens: %zu", g_visibleTokens.size());
    for (const auto& token : g_visibleTokens) logLine("    keep: %s", token.c_str());
}

bool isReadableAddress(const void* address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(address, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;
    return true;
}

bool isReadableRange(const void* address, std::size_t size) {
    if (!address || size == 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + size;
    if (end < begin) return false;

    auto cursor = begin;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;
        const auto next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= cursor) return false;
        cursor = std::min(next, end);
    }
    return true;
}

bool isWritableProtection(DWORD protect) {
    if ((protect & PAGE_GUARD) || (protect & PAGE_NOACCESS)) return false;
    const DWORD base = protect & 0xFF;
    return base == PAGE_READWRITE
        || base == PAGE_WRITECOPY
        || base == PAGE_EXECUTE_READWRITE
        || base == PAGE_EXECUTE_WRITECOPY;
}

void* safeReadPointer(const void* address) {
#if defined(_MSC_VER)
    __try {
        return *reinterpret_cast<void* const*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    if (!isReadableRange(address, sizeof(void*))) return nullptr;
    return *reinterpret_cast<void* const*>(address);
#endif
}

bool safeReadInt32(const void* address, std::int32_t& value) {
#if defined(_MSC_VER)
    __try {
        value = *reinterpret_cast<const std::int32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    if (!isReadableRange(address, sizeof(value))) return false;
    value = *reinterpret_cast<const std::int32_t*>(address);
    return true;
#endif
}

void* safeCallPtr(void* object, std::size_t index) {
    if (!object) return nullptr;
#if defined(_MSC_VER)
    __try {
        auto** table = *reinterpret_cast<void***>(object);
        using Fn = void*(__fastcall*)(void*);
        return reinterpret_cast<Fn>(table[index])(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    return nullptr;
#endif
}

bool copyMsvcString(const void* stringObject, char* output, std::size_t outputCapacity) {
    if (!output || outputCapacity < 2) return false;
    output[0] = '\0';

#if defined(_MSC_VER)
    __try {
        const auto* base = reinterpret_cast<const std::uint8_t*>(stringObject);
        const std::size_t length = *reinterpret_cast<const std::size_t*>(base + 0x10);
        const std::size_t capacity = *reinterpret_cast<const std::size_t*>(base + 0x18);
        if (length == 0 || length >= outputCapacity || length > 255) return false;
        if (capacity < length) return false;

        const char* data = capacity < 16
            ? reinterpret_cast<const char*>(base)
            : *reinterpret_cast<const char* const*>(base);
        if (!data || !isReadableRange(data, length)) return false;

        std::memcpy(output, data, length);
        output[length] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return false;
#endif
}

bool plausibleBlockName(const char* text) {
    if (!text || !*text) return false;
    const std::size_t length = std::strlen(text);
    if (length < 2 || length > 200) return false;
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (!(std::isalnum(ch) || ch == '_' || ch == ':' || ch == '.' || ch == '-' || ch == '/')) {
            return false;
        }
    }
    return true;
}

bool readBlockName(void* blockLegacy, std::string& result) {
    if (!blockLegacy || !isReadableAddress(blockLegacy)) return false;

    char buffer[256]{};

    // PRIMARY SOURCE: exact 1.26.4501.0 field from the uploaded MineXRayCore.hpp.
    if (copyMsvcString(
            reinterpret_cast<std::uint8_t*>(blockLegacy) + ox::BlockLegacy_fullNamespacedName,
            buffer,
            sizeof(buffer))
        && plausibleBlockName(buffer)) {
        result.assign(buffer);
        return true;
    }

    // Diagnostic fallbacks only. The HPP field above is always preferred.
    constexpr std::array<std::size_t, 4> fallbackOffsets = {0x08, 0xE8, 0x98, 0x30};
    for (const auto offset : fallbackOffsets) {
        if (copyMsvcString(reinterpret_cast<std::uint8_t*>(blockLegacy) + offset, buffer, sizeof(buffer))
            && plausibleBlockName(buffer)) {
            result.assign(buffer);
            return true;
        }
    }
    return false;
}

bool shouldRemainVisible(const std::string& identifier) {
    const std::string name = lowerCopy(identifier);
    if (name.find("air") != std::string::npos) return true;
    for (const auto& token : g_visibleTokens) {
        if (name.find(token) != std::string::npos) return true;
    }
    return false;
}

std::vector<int> parsePattern(const char* text) {
    std::vector<int> result;
    const char* p = text;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (*p == '?') {
            result.push_back(-1);
            ++p;
            if (*p == '?') ++p;
        } else {
            char token[3]{};
            token[0] = *p++;
            if (!*p) return {};
            token[1] = *p++;
            char* end = nullptr;
            const long value = std::strtol(token, &end, 16);
            if (!end || *end != '\0' || value < 0 || value > 0xFF) return {};
            result.push_back(static_cast<int>(value));
        }
        while (*p == ' ') ++p;
    }
    return result;
}

std::uint8_t* getSection(HMODULE module, const char* name, std::size_t& size) {
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char sectionName[9]{};
        std::memcpy(sectionName, section[i].Name, 8);
        if (std::strcmp(sectionName, name) == 0) {
            size = section[i].Misc.VirtualSize;
            return base + section[i].VirtualAddress;
        }
    }
    return nullptr;
}

std::uint8_t* findPattern(std::uint8_t* start, std::size_t size, const std::vector<int>& pattern) {
    if (!start || pattern.empty() || size < pattern.size()) return nullptr;
    for (std::size_t i = 0; i + pattern.size() <= size; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] >= 0 && start[i + j] != static_cast<std::uint8_t>(pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) return start + i;
    }
    return nullptr;
}

std::vector<std::uint8_t*> findPatterns(
    std::uint8_t* start,
    std::size_t size,
    const std::vector<int>& pattern,
    std::size_t limit
) {
    std::vector<std::uint8_t*> matches;
    if (!start || pattern.empty() || size < pattern.size()) return matches;
    for (std::size_t i = 0; i + pattern.size() <= size && matches.size() < limit; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] >= 0 && start[i + j] != static_cast<std::uint8_t>(pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) matches.push_back(start + i);
    }
    return matches;
}

std::uintptr_t resolveRip(std::uint8_t* instruction, std::size_t dispOffset, std::size_t instructionSize) {
    std::int32_t displacement{};
    std::memcpy(&displacement, instruction + dispOffset, sizeof(displacement));
    return reinterpret_cast<std::uintptr_t>(instruction + instructionSize) + static_cast<std::intptr_t>(displacement);
}

bool addressInSection(const void* address, std::uint8_t* section, std::size_t size) {
    if (!address || !section || size == 0) return false;
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto begin = reinterpret_cast<std::uintptr_t>(section);
    return value >= begin && value < begin + size;
}

std::vector<std::uintptr_t> findWritablePointers(std::uintptr_t target, std::size_t limit) {
    std::vector<std::uintptr_t> results;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto cursor = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxAddress = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    constexpr std::size_t chunkSize = 1024 * 1024;
    HANDLE self = GetCurrentProcess();

    while (cursor < maxAddress && results.size() < limit) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) break;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionSize = static_cast<std::size_t>(mbi.RegionSize);

        if (mbi.State == MEM_COMMIT && isWritableProtection(mbi.Protect) && regionSize >= sizeof(std::uintptr_t)) {
            std::size_t offset = 0;
            while (offset < regionSize && results.size() < limit) {
                const auto toRead = std::min(chunkSize, regionSize - offset);
                std::vector<std::uint8_t> bytes(toRead);
                SIZE_T read = 0;
                if (ReadProcessMemory(self, reinterpret_cast<const void*>(regionBase + offset),
                                      bytes.data(), toRead, &read)
                    && read >= sizeof(std::uintptr_t)) {
                    const auto absoluteBase = regionBase + offset;
                    std::size_t i = static_cast<std::size_t>((8 - (absoluteBase & 7)) & 7);
                    for (; i + sizeof(std::uintptr_t) <= read && results.size() < limit; i += 8) {
                        std::uintptr_t value{};
                        std::memcpy(&value, bytes.data() + i, sizeof(value));
                        if (value == target) results.push_back(absoluteBase + i);
                    }
                }
                offset += toRead;
            }
        }

        const auto next = regionBase + regionSize;
        if (next <= cursor) break;
        cursor = next;
    }
    return results;
}

void** findBlockLegacyVtable() {
    if (!g_minecraft) return nullptr;

    std::size_t textSize = 0;
    auto* text = getSection(g_minecraft, ".text", textSize);
    std::size_t rdataSize = 0;
    auto* rdata = getSection(g_minecraft, ".rdata", rdataSize);
    if (!text || !rdata) return nullptr;

    const auto constructorPattern = parsePattern(
        "48 8D 05 ?? ?? ?? ?? 48 89 01 4C 8B 72 ?? 48 B9");
    if (auto* match = findPattern(text, textSize, constructorPattern)) {
        const auto target = resolveRip(match, 3, 7);
        if (addressInSection(reinterpret_cast<void*>(target), rdata, rdataSize)) {
            logLine("[+] BlockLegacy vtable via constructor: 0x%llX",
                    static_cast<unsigned long long>(target));
            return reinterpret_cast<void**>(target);
        }
    }

    constexpr char rttiName[] = ".?AVBlockLegacy@@";
    std::uint8_t* nameAddress = nullptr;
    for (std::size_t i = 0; i + sizeof(rttiName) <= rdataSize; ++i) {
        if (std::memcmp(rdata + i, rttiName, sizeof(rttiName) - 1) == 0) {
            nameAddress = rdata + i;
            break;
        }
    }
    if (!nameAddress || nameAddress < rdata + 16) {
        logLine("[!] BlockLegacy RTTI not found");
        return nullptr;
    }

    const auto moduleBase = reinterpret_cast<std::uintptr_t>(g_minecraft);
    const auto typeDescriptor = reinterpret_cast<std::uintptr_t>(nameAddress - 16);
    const auto typeDescriptorRva = static_cast<std::uint32_t>(typeDescriptor - moduleBase);

    for (std::size_t i = 0; i + 24 <= rdataSize; i += 4) {
        const auto* candidate = rdata + i;
        std::uint32_t signature{};
        std::uint32_t pTypeDescriptor{};
        std::uint32_t selfRva{};
        std::memcpy(&signature, candidate + 0, sizeof(signature));
        std::memcpy(&pTypeDescriptor, candidate + 12, sizeof(pTypeDescriptor));
        std::memcpy(&selfRva, candidate + 20, sizeof(selfRva));
        if (signature != 1 || pTypeDescriptor != typeDescriptorRva) continue;
        if (moduleBase + selfRva != reinterpret_cast<std::uintptr_t>(candidate)) continue;

        const auto colAddress = reinterpret_cast<std::uintptr_t>(candidate);
        for (std::size_t p = 0; p + sizeof(std::uintptr_t) <= rdataSize; p += sizeof(std::uintptr_t)) {
            std::uintptr_t value{};
            std::memcpy(&value, rdata + p, sizeof(value));
            if (value != colAddress) continue;

            auto** vtable = reinterpret_cast<void**>(rdata + p + sizeof(std::uintptr_t));
            void* firstFunction = safeReadPointer(vtable);
            if (addressInSection(firstFunction, text, textSize)) {
                logLine("[+] BlockLegacy vtable via RTTI: 0x%llX",
                        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(vtable)));
                return vtable;
            }
        }
    }

    logLine("[!] BlockLegacy vtable unresolved");
    return nullptr;
}

bool decodeSimpleIntGetter(std::uint8_t* function, std::int32_t& fieldOffset, std::size_t& length) {
    if (!function || !isReadableRange(function, 16)) return false;

    if (function[0] == 0x8B && function[1] == 0x81 && function[6] == 0xC3) {
        std::memcpy(&fieldOffset, function + 2, sizeof(fieldOffset));
        length = 7;
    } else if (function[0] == 0x8B && function[1] == 0x41 && function[3] == 0xC3) {
        fieldOffset = static_cast<std::int8_t>(function[2]);
        length = 4;
    } else {
        return false;
    }

    if (fieldOffset < 0 || fieldOffset > 0x3000) return false;
    for (std::size_t i = length; i < 12; ++i) {
        if (function[i] != 0xCC && function[i] != 0x90) return false;
    }
    return true;
}

bool followedByFloatGetter(std::uint8_t* function, std::size_t getterLength) {
    if (!function || !isReadableRange(function, 80)) return false;
    std::size_t i = getterLength;
    while (i < 64 && (function[i] == 0xCC || function[i] == 0x90)) ++i;
    if (i + 9 >= 80) return false;

    return function[i] == 0xF3
        && function[i + 1] == 0x0F
        && function[i + 2] == 0x10
        && ((function[i + 3] == 0x81 && function[i + 8] == 0xC3)
            || (function[i + 3] == 0x41 && function[i + 5] == 0xC3));
}

std::uint8_t* findRenderLayerGetter() {
    std::size_t textSize = 0;
    auto* text = getSection(g_minecraft, ".text", textSize);
    if (!text) return nullptr;

    g_blockLegacyVtable = findBlockLegacyVtable();

    const auto exactPattern = parsePattern(
        "8B 81 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC CC F3 0F 10 81");
    const auto exactMatches = findPatterns(text, textSize, exactPattern, 32);
    logLine("[+] Historical getRenderLayer exact matches: %zu", exactMatches.size());

    if (g_blockLegacyVtable) {
        std::vector<std::pair<std::size_t, std::uint8_t*>> pairedCandidates;

        for (std::size_t index = 0; index < 384; ++index) {
            auto* function = reinterpret_cast<std::uint8_t*>(safeReadPointer(g_blockLegacyVtable + index));
            if (!addressInSection(function, text, textSize)) break;

            std::int32_t field = -1;
            std::size_t getterLength = 0;
            if (!decodeSimpleIntGetter(function, field, getterLength)) continue;

            if (std::find(exactMatches.begin(), exactMatches.end(), function) != exactMatches.end()) {
                g_renderLayerVtableIndex = index;
                g_renderLayerFieldOffset = field;
                logLine("[+] getRenderLayer exact vtable match: slot=%zu field=+0x%X fn=%p",
                        index, static_cast<unsigned>(field), function);
                return function;
            }

            if (followedByFloatGetter(function, getterLength)) {
                pairedCandidates.emplace_back(index, function);
                logLine("    render-like getter: slot=%zu field=+0x%X fn=%p",
                        index, static_cast<unsigned>(field), function);
            }
        }

        constexpr std::size_t historicalIndex = 180;
        auto* historical = reinterpret_cast<std::uint8_t*>(safeReadPointer(g_blockLegacyVtable + historicalIndex));
        std::int32_t field = -1;
        std::size_t getterLength = 0;
        if (addressInSection(historical, text, textSize)
            && decodeSimpleIntGetter(historical, field, getterLength)
            && followedByFloatGetter(historical, getterLength)) {
            g_renderLayerVtableIndex = historicalIndex;
            g_renderLayerFieldOffset = field;
            logLine("[+] getRenderLayer validated at historical slot 180: field=+0x%X fn=%p",
                    static_cast<unsigned>(field), historical);
            return historical;
        }

        if (pairedCandidates.size() == 1) {
            const auto [index, function] = pairedCandidates.front();
            std::int32_t uniqueField = -1;
            std::size_t uniqueLength = 0;
            if (decodeSimpleIntGetter(function, uniqueField, uniqueLength)) {
                g_renderLayerVtableIndex = index;
                g_renderLayerFieldOffset = uniqueField;
                logLine("[+] getRenderLayer selected from unique structural pair: slot=%zu field=+0x%X",
                        index, static_cast<unsigned>(uniqueField));
                return function;
            }
        }
    }

    if (exactMatches.size() == 1) {
        std::int32_t field = -1;
        std::size_t getterLength = 0;
        if (decodeSimpleIntGetter(exactMatches.front(), field, getterLength)) {
            g_renderLayerFieldOffset = field;
            logLine("[+] getRenderLayer accepted from unique executable signature: field=+0x%X",
                    static_cast<unsigned>(field));
            return exactMatches.front();
        }
    }

    logLine("[-] Could not safely resolve BlockLegacy::getRenderLayer");
    return nullptr;
}

int __fastcall hookedRenderLayer(void* blockLegacy) {
    std::int32_t originalLayer = 0;
    if (g_renderLayerFieldOffset < 0
        || !safeReadInt32(reinterpret_cast<std::uint8_t*>(blockLegacy) + g_renderLayerFieldOffset, originalLayer)) {
        return 0;
    }

    if (!g_xrayEnabled.load(std::memory_order_relaxed)
        || g_unloading.load(std::memory_order_relaxed)) {
        return originalLayer;
    }

    std::string identifier;
    if (!readBlockName(blockLegacy, identifier)) return originalLayer;
    if (shouldRemainVisible(identifier)) return originalLayer;

    return 10;
}

bool installRenderLayerPatch() {
    if (g_renderLayerPatch.installed) return true;
    auto* target = findRenderLayerGetter();
    if (!target) return false;

    std::int32_t verifiedField = -1;
    std::size_t getterLength = 0;
    if (!decodeSimpleIntGetter(target, verifiedField, getterLength)
        || verifiedField != g_renderLayerFieldOffset) {
        logLine("[-] Refusing hook: getter validation changed");
        return false;
    }

    std::array<std::uint8_t, 12> patch{};
    patch[0] = 0x48;
    patch[1] = 0xB8;
    const auto hookAddress = reinterpret_cast<std::uintptr_t>(&hookedRenderLayer);
    std::memcpy(patch.data() + 2, &hookAddress, sizeof(hookAddress));
    patch[10] = 0xFF;
    patch[11] = 0xE0;

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, patch.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logLine("[-] VirtualProtect(getRenderLayer) failed: %lu", GetLastError());
        return false;
    }

    std::memcpy(g_renderLayerPatch.original.data(), target, g_renderLayerPatch.original.size());
    std::memcpy(target, patch.data(), patch.size());
    FlushInstructionCache(GetCurrentProcess(), target, patch.size());

    DWORD ignored = 0;
    VirtualProtect(target, patch.size(), oldProtect, &ignored);

    g_renderLayerPatch.address = target;
    g_renderLayerPatch.installed = true;
    logLine("[+] BlockLegacy::getRenderLayer hooked at %p", target);
    return true;
}

void uninstallRenderLayerPatch() {
    if (!g_renderLayerPatch.installed || !g_renderLayerPatch.address) return;

    DWORD oldProtect = 0;
    if (VirtualProtect(g_renderLayerPatch.address, g_renderLayerPatch.original.size(),
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(g_renderLayerPatch.address, g_renderLayerPatch.original.data(),
                    g_renderLayerPatch.original.size());
        FlushInstructionCache(GetCurrentProcess(), g_renderLayerPatch.address,
                              g_renderLayerPatch.original.size());
        DWORD ignored = 0;
        VirtualProtect(g_renderLayerPatch.address, g_renderLayerPatch.original.size(), oldProtect, &ignored);
    }

    g_renderLayerPatch = {};
    logLine("[+] getRenderLayer hook removed");
}

std::uintptr_t exactClientVtableAddress() {
    return reinterpret_cast<std::uintptr_t>(g_minecraft) + ox::ClientInstance_vtable_RVA;
}

bool validateClientVtable(std::uintptr_t vtableAddress) {
    if (!vtableAddress || !isReadableRange(reinterpret_cast<void*>(vtableAddress),
        (ox::ClientInstance_getLevelRenderer + 1) * sizeof(void*))) {
        return false;
    }

    std::size_t textSize = 0;
    auto* text = getSection(g_minecraft, ".text", textSize);
    if (!text) return false;

    auto** table = reinterpret_cast<void**>(vtableAddress);
    void* getRegion = safeReadPointer(table + ox::ClientInstance_getRegion);
    void* getLocalPlayer = safeReadPointer(table + ox::ClientInstance_getLocalPlayer);
    void* getLevelRenderer = safeReadPointer(table + ox::ClientInstance_getLevelRenderer);
    return addressInSection(getRegion, text, textSize)
        && addressInSection(getLocalPlayer, text, textSize)
        && addressInSection(getLevelRenderer, text, textSize);
}

void* findClientInstance() {
    const auto exactVtable = exactClientVtableAddress();
    if (g_clientInstance && safeReadPointer(g_clientInstance) == reinterpret_cast<void*>(exactVtable)) {
        return g_clientInstance;
    }

    if (!validateClientVtable(exactVtable)) {
        logLine("[!] Exact HPP ClientInstance vtable RVA 0x%llX did not validate",
                static_cast<unsigned long long>(ox::ClientInstance_vtable_RVA));
        return nullptr;
    }

    const auto hits = findWritablePointers(exactVtable, 16);
    logLine("[+] Exact ClientInstance vtable=0x%llX pointer hits=%zu",
            static_cast<unsigned long long>(exactVtable), hits.size());

    int bestScore = -1;
    void* best = nullptr;
    for (const auto address : hits) {
        auto* candidate = reinterpret_cast<void*>(address);
        if (safeReadPointer(candidate) != reinterpret_cast<void*>(exactVtable)) continue;

        int score = 1;
        void* levelRenderer = safeCallPtr(candidate, ox::ClientInstance_getLevelRenderer);
        if (levelRenderer && isReadableAddress(levelRenderer)) score += 10;

        void* region = safeCallPtr(candidate, ox::ClientInstance_getRegion);
        if (region && isReadableAddress(region)) {
            score += 10;
            const auto expectedBlockSourceVtable = reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(g_minecraft) + ox::BlockSource_vtable_RVA);
            if (safeReadPointer(region) == expectedBlockSourceVtable) score += 30;
        }

        void* player = safeCallPtr(candidate, ox::ClientInstance_getLocalPlayer);
        if (player && isReadableRange(player, static_cast<std::size_t>(ox::Actor_level) + sizeof(void*))) {
            score += 10;
            void* level = safeReadPointer(reinterpret_cast<std::uint8_t*>(player) + ox::Actor_level);
            if (level && isReadableAddress(level)) {
                score += 10;
                const auto expectedLevelVtable = reinterpret_cast<void*>(
                    reinterpret_cast<std::uintptr_t>(g_minecraft) + ox::Level_vtable_RVA);
                if (safeReadPointer(level) == expectedLevelVtable) score += 30;
            }
        }

        logLine("    CI candidate=%p score=%d", candidate, score);
        if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }

    if (best && bestScore >= 11) {
        g_clientInstance = best;
        logLine("[+] ClientInstance selected from exact HPP validation: %p score=%d", best, bestScore);
        return best;
    }

    logLine("[!] No ClientInstance candidate passed exact HPP validation");
    return nullptr;
}

void resolveRebuildFunction() {
    if (g_rebuildChunk) return;
    std::size_t textSize = 0;
    auto* text = getSection(g_minecraft, ".text", textSize);
    if (!text) return;

    const auto pattern = parsePattern(
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC ?? "
        "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 ?? 48 8B F9 48 8D A9");
    auto* match = findPattern(text, textSize, pattern);
    if (match) {
        g_rebuildChunk = reinterpret_cast<RebuildChunkFn>(match);
        logLine("[+] rebuildAllRenderChunkGeometry candidate: %p", match);
    } else {
        logLine("[!] Rebuild signature missing. Re-enter the world after enabling XRay.");
    }
}

bool safeCallRebuild(void* coordinator) {
    if (!g_rebuildChunk || !coordinator) return false;
#if defined(_MSC_VER)
    __try {
        g_rebuildChunk(coordinator);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return false;
#endif
}

bool forceChunkRebuild() {
    resolveRebuildFunction();
    if (!g_rebuildChunk) return false;

    void* client = findClientInstance();
    if (!client) return false;

    // Exact HPP path: getLevelRenderer is vtable slot 187.
    void* levelRenderer = safeCallPtr(client, ox::ClientInstance_getLevelRenderer);
    if (!levelRenderer || !isReadableAddress(levelRenderer)) {
        logLine("[!] Exact getLevelRenderer(187) returned no readable object");
        return false;
    }

    // The list detail is Borion-derived, not part of MineXRayCore.hpp, so this is
    // best-effort only. All reads/calls are guarded and failure falls back to re-entering.
    void* sentinel = safeReadPointer(reinterpret_cast<std::uint8_t*>(levelRenderer) + 0x20);
    if (!sentinel || !isReadableRange(sentinel, 0x20)) {
        logLine("[!] LevelRenderer rebuild list not validated; re-enter world instead");
        return false;
    }

    void* node = safeReadPointer(sentinel);
    if (!node) return false;

    std::size_t rebuilt = 0;
    std::size_t iterations = 0;
    while (node && node != sentinel && iterations++ < 512) {
        if (!isReadableRange(node, 0x20)) break;
        void* coordinator = safeReadPointer(reinterpret_cast<std::uint8_t*>(node) + 0x18);
        if (coordinator && isReadableAddress(coordinator) && safeCallRebuild(coordinator)) ++rebuilt;
        node = safeReadPointer(node);
    }

    logLine("[+] Chunk rebuild request: %zu coordinator(s)", rebuilt);
    return rebuilt > 0;
}

void runDiagnostics() {
    logLine("---------------- diagnostics ----------------");
    logLine("HPP exact: CI slots region=%zu localPlayer=%zu levelRenderer=%zu",
            ox::ClientInstance_getRegion,
            ox::ClientInstance_getLocalPlayer,
            ox::ClientInstance_getLevelRenderer);
    logLine("HPP exact: BlockLegacy fullName=+0x%llX Block->legacy=+0x%llX",
            static_cast<unsigned long long>(ox::BlockLegacy_fullNamespacedName),
            static_cast<unsigned long long>(ox::Block_blockLegacy));
    logLine("hook=%s xray=%s field=0x%X vslot=%s",
            g_renderLayerPatch.installed ? "installed" : "missing",
            g_xrayEnabled.load() ? "ON" : "OFF",
            static_cast<unsigned>(g_renderLayerFieldOffset),
            g_renderLayerVtableIndex == static_cast<std::size_t>(-1) ? "unknown" : "resolved");

    if (!g_renderLayerPatch.installed) installRenderLayerPatch();
    findClientInstance();
    resolveRebuildFunction();
    logLine("---------------------------------------------");
}

void setXray(bool enabled) {
    if (enabled && !g_renderLayerPatch.installed) {
        if (!installRenderLayerPatch()) {
            logLine("[-] XRAY cannot enable: getRenderLayer unresolved");
            Beep(250, 180);
            return;
        }
    }

    g_xrayEnabled.store(enabled, std::memory_order_relaxed);
    logLine("[+] XRAY %s", enabled ? "ON" : "OFF");
    logLine("[i] If already inside a world, press F8 or leave/re-enter the world so chunks rebuild.");
    Beep(enabled ? 900 : 500, 90);
}

DWORD WINAPI workerThread(void*) {
    openLog();
    logLine("============================================================");
    logLine("MineDLL / MineXRay - HPP-backed Bedrock XRay");
    logLine("Target: Minecraft Bedrock 1.26.4501.0 / 26.45.1 x64");
    logLine("Using uploaded MineXRayCore.hpp exact 1.26.4501.0 offsets");
    logLine("Core XRay method: BlockLegacy::getRenderLayer hook (Horion/Borion style)");
    logLine("F6 toggle | F7 diagnostics | F8 best-effort chunk rebuild | F12 unload");

    g_minecraft = GetModuleHandleW(L"Minecraft.Windows.exe");
    if (!g_minecraft) {
        logLine("[-] Minecraft.Windows.exe module not found");
        if (g_log) {
            std::fclose(g_log);
            g_log = nullptr;
        }
        FreeLibraryAndExitThread(g_self, 1);
        return 1;
    }

    loadVisibleTokens();
    Sleep(800);

    if (installRenderLayerPatch()) {
        // Do not force an unvalidated rebuild on startup. Inject on the main menu,
        // then enter the world so chunks are first-built with XRay already active.
        setXray(true);
    } else {
        logLine("[-] Initial hook failed. F7 writes detailed resolver diagnostics.");
        Beep(250, 180);
    }

    while (true) {
        if (GetAsyncKeyState(VK_F6) & 1) setXray(!g_xrayEnabled.load(std::memory_order_relaxed));
        if (GetAsyncKeyState(VK_F7) & 1) runDiagnostics();
        if (GetAsyncKeyState(VK_F8) & 1) {
            if (!forceChunkRebuild()) {
                logLine("[i] F8 could not validate rebuild chain. Leave and re-enter the world.");
                Beep(350, 80);
            }
        }
        if (GetAsyncKeyState(VK_F12) & 1) break;
        Sleep(30);
    }

    logLine("[+] Unloading MineXRay");
    g_unloading.store(true, std::memory_order_relaxed);
    g_xrayEnabled.store(false, std::memory_order_relaxed);
    uninstallRenderLayerPatch();

    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }

    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
