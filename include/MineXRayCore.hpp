#pragma once

// MINE MOD - XRay / Ore Scanner core
// Target: Minecraft for Windows 1.26.4501.0 (Bedrock 1.26.45.1)
// Intended for an in-process MSVC x64 DLL.
//
// This first milestone does NOT patch the chunk renderer.
// It scans blocks around the camera through the game's own BlockSource::getBlock()
// and exposes ore positions for your existing ESP / 3D renderer.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mine_mod::xray {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct BlockPos {
    int x{};
    int y{};
    int z{};
};

enum class OreKind : std::uint8_t {
    None = 0,
    Coal,
    Copper,
    Iron,
    Gold,
    Redstone,
    Lapis,
    Diamond,
    Emerald,
    NetherGold,
    NetherQuartz,
    AncientDebris,
};

struct OreHit {
    BlockPos pos{};
    OreKind kind{OreKind::None};
};

struct Settings {
    // 32 x 24 gives ~207k positions per complete scan.
    int horizontalRadius = 32;
    int verticalRadius = 24;

    // Increase if tick() runs once per frame; reduce if it runs only on game tick.
    std::size_t blocksPerTick = 3500;

    // Bedrock Overworld limits. Kept configurable for other dimensions / future work.
    int minY = -64;
    int maxY = 320;

    // Re-center the scan only after meaningful player/camera movement.
    int recenterHorizontal = 4;
    int recenterVertical = 3;
};

namespace offsets_1_26_4501 {

// IClientInstance vtable (validated against getLevelRenderer slot 187).
inline constexpr std::size_t ClientInstance_getRegion        = 29;
inline constexpr std::size_t ClientInstance_getLocalPlayer   = 30;
inline constexpr std::size_t ClientInstance_getLevelRenderer = 187;

// BlockSource vtable.
inline constexpr std::size_t BlockSource_getBlock = 2;
inline constexpr std::size_t BlockSource_getChunk = 41;

// Exact 1.26.4501.0 fields.
inline constexpr std::ptrdiff_t Actor_level                     = 0x10D8;
inline constexpr std::ptrdiff_t Block_blockLegacy               = 0x60;
inline constexpr std::ptrdiff_t BlockLegacy_fullNamespacedName  = 0xD0;

// BlockSource layout validated from the live process.
inline constexpr std::ptrdiff_t BlockSource_level       = 0x20;
inline constexpr std::ptrdiff_t BlockSource_chunkSource = 0x28;
inline constexpr std::ptrdiff_t BlockSource_dimension   = 0x30;

// 1.26.X render chain used for a stable scan center.
inline constexpr std::ptrdiff_t LevelRender_levelRendererPlayer = 0x430;
inline constexpr std::ptrdiff_t LevelRendererPlayer_cameraPos   = 0x704;

// Stable build RVAs useful for debug validation only.
// Runtime absolute address = Minecraft.Windows.exe module base + RVA.
inline constexpr std::uintptr_t ClientInstance_vtable_RVA = 0x0E774DB0;
inline constexpr std::uintptr_t Level_vtable_RVA          = 0x0E79E800;
inline constexpr std::uintptr_t BlockSource_vtable_RVA    = 0x0E7EF3A0;

} // namespace offsets_1_26_4501

template <typename Ret, typename... Args>
inline Ret vcall(void* object, std::size_t index, Args... args) {
    auto** vtable = *reinterpret_cast<void***>(object);
    using Fn = Ret(__fastcall*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[index])(object, args...);
}

inline void* getBlockSource(void* clientInstance) {
    if (!clientInstance) return nullptr;
    return vcall<void*>(
        clientInstance,
        offsets_1_26_4501::ClientInstance_getRegion
    );
}

inline void* getLocalPlayer(void* clientInstance) {
    if (!clientInstance) return nullptr;
    return vcall<void*>(
        clientInstance,
        offsets_1_26_4501::ClientInstance_getLocalPlayer
    );
}

inline void* getLevelRenderer(void* clientInstance) {
    if (!clientInstance) return nullptr;
    return vcall<void*>(
        clientInstance,
        offsets_1_26_4501::ClientInstance_getLevelRenderer
    );
}

inline Vec3 getCameraPosition(void* clientInstance) {
    Vec3 result{};
    void* levelRender = getLevelRenderer(clientInstance);
    if (!levelRender) return result;

    auto* player = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(levelRender) +
        offsets_1_26_4501::LevelRender_levelRendererPlayer
    );
    if (!player) return result;

    return *reinterpret_cast<Vec3*>(
        reinterpret_cast<std::uintptr_t>(player) +
        offsets_1_26_4501::LevelRendererPlayer_cameraPos
    );
}

inline void* getBlock(void* blockSource, const BlockPos& pos) {
    if (!blockSource) return nullptr;

    auto** vtable = *reinterpret_cast<void***>(blockSource);
    using Fn = void*(__fastcall*)(void*, const BlockPos&);
    auto fn = reinterpret_cast<Fn>(
        vtable[offsets_1_26_4501::BlockSource_getBlock]
    );
    return fn(blockSource, pos);
}

inline std::string_view getFullBlockName(void* block) {
    if (!block) return {};

    void* legacy = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(block) +
        offsets_1_26_4501::Block_blockLegacy
    );
    if (!legacy) return {};

    // 1.26 uses the std::string at BlockLegacy + 0xD0 as the full ID,
    // e.g. "minecraft:deepslate_diamond_ore".
    const auto& full = *reinterpret_cast<const std::string*>(
        reinterpret_cast<std::uintptr_t>(legacy) +
        offsets_1_26_4501::BlockLegacy_fullNamespacedName
    );

    // Small sanity guard. Valid vanilla IDs are nowhere near this long.
    if (full.empty() || full.size() > 160) return {};
    return std::string_view(full.data(), full.size());
}

inline std::string_view stripNamespace(std::string_view id) {
    const auto colon = id.find(':');
    return colon == std::string_view::npos ? id : id.substr(colon + 1);
}

inline OreKind classifyOre(std::string_view fullId) {
    const auto id = stripNamespace(fullId);

    if (id == "diamond_ore" || id == "deepslate_diamond_ore")
        return OreKind::Diamond;

    if (id == "emerald_ore" || id == "deepslate_emerald_ore")
        return OreKind::Emerald;

    if (id == "gold_ore" || id == "deepslate_gold_ore")
        return OreKind::Gold;

    if (id == "iron_ore" || id == "deepslate_iron_ore")
        return OreKind::Iron;

    if (id == "copper_ore" || id == "deepslate_copper_ore")
        return OreKind::Copper;

    if (id == "coal_ore" || id == "deepslate_coal_ore")
        return OreKind::Coal;

    if (id == "lapis_ore" || id == "deepslate_lapis_ore")
        return OreKind::Lapis;

    if (id == "redstone_ore" ||
        id == "deepslate_redstone_ore" ||
        id == "lit_redstone_ore" ||
        id == "lit_deepslate_redstone_ore")
        return OreKind::Redstone;

    if (id == "nether_gold_ore")
        return OreKind::NetherGold;

    if (id == "quartz_ore" || id == "nether_quartz_ore")
        return OreKind::NetherQuartz;

    if (id == "ancient_debris")
        return OreKind::AncientDebris;

    return OreKind::None;
}

inline std::uint64_t packPos(const BlockPos& p) {
    // Minecraft-style compact key. Negative coordinates are masked intentionally.
    const auto x = static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) & 0x3FFFFFFull;
    const auto z = static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z)) & 0x3FFFFFFull;
    const auto y = static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) & 0xFFFull;
    return (x << 38) | (z << 12) | y;
}

class Scanner {
public:
    explicit Scanner(Settings settings = {}) : settings_(settings) {}

    void setSettings(const Settings& settings) {
        std::scoped_lock lock(mutex_);
        settings_ = settings;
        cursor_ = 0;
        initialized_ = false;
        ++generation_;
    }

    const Settings& settings() const {
        return settings_;
    }

    // Call from a safe in-game thread / hook while a world is loaded.
    void tick(void* clientInstance) {
        if (!clientInstance) return;

        void* region = getBlockSource(clientInstance);
        if (!region) return;

        const Vec3 camera = getCameraPosition(clientInstance);
        if (!std::isfinite(camera.x) ||
            !std::isfinite(camera.y) ||
            !std::isfinite(camera.z)) {
            return;
        }

        BlockPos cameraBlock{
            static_cast<int>(std::floor(camera.x)),
            static_cast<int>(std::floor(camera.y)),
            static_cast<int>(std::floor(camera.z))
        };

        std::scoped_lock lock(mutex_);

        if (!initialized_ || needsRecenter(cameraBlock)) {
            origin_ = cameraBlock;
            cursor_ = 0;
            initialized_ = true;
            ++generation_;
            pruneOutsideRadius();
        }

        const int hr = std::max(1, settings_.horizontalRadius);
        const int vr = std::max(1, settings_.verticalRadius);
        const int width = hr * 2 + 1;
        const int height = vr * 2 + 1;

        const std::uint64_t total =
            static_cast<std::uint64_t>(width) *
            static_cast<std::uint64_t>(height) *
            static_cast<std::uint64_t>(width);

        if (total == 0) return;

        std::size_t processed = 0;

        while (processed < settings_.blocksPerTick) {
            if (cursor_ >= total) {
                cursor_ = 0;
                finishGeneration();
                ++generation_;
            }

            std::uint64_t n = cursor_++;

            const int localX = static_cast<int>(n % width);
            n /= width;
            const int localZ = static_cast<int>(n % width);
            n /= width;
            const int localY = static_cast<int>(n % height);

            BlockPos pos{
                origin_.x + localX - hr,
                origin_.y + localY - vr,
                origin_.z + localZ - hr
            };

            ++processed;

            if (pos.y < settings_.minY || pos.y > settings_.maxY)
                continue;

            // Optional spherical horizontal cut; saves calls at cube corners.
            const int dx = pos.x - origin_.x;
            const int dz = pos.z - origin_.z;
            if (dx * dx + dz * dz > hr * hr)
                continue;

            void* block = getBlock(region, pos);
            if (!block) continue;

            const OreKind kind = classifyOre(getFullBlockName(block));
            const auto key = packPos(pos);

            if (kind == OreKind::None) {
                hits_.erase(key);
                continue;
            }

            hits_[key] = TrackedHit{OreHit{pos, kind}, generation_};
        }
    }

    [[nodiscard]] std::vector<OreHit> snapshot() const {
        std::scoped_lock lock(mutex_);
        std::vector<OreHit> out;
        out.reserve(hits_.size());
        for (const auto& [_, tracked] : hits_)
            out.push_back(tracked.hit);
        return out;
    }

    [[nodiscard]] std::size_t hitCount() const {
        std::scoped_lock lock(mutex_);
        return hits_.size();
    }

    void clear() {
        std::scoped_lock lock(mutex_);
        hits_.clear();
        cursor_ = 0;
        initialized_ = false;
        ++generation_;
    }

    // Extra runtime sanity check using the exact Actor::level offset.
    // Useful when debugging getLocalPlayer().
    [[nodiscard]] bool localPlayerPointsTo(void* clientInstance, void* expectedLevel) const {
        void* player = getLocalPlayer(clientInstance);
        if (!player || !expectedLevel) return false;

        void* level = *reinterpret_cast<void**>(
            reinterpret_cast<std::uintptr_t>(player) +
            offsets_1_26_4501::Actor_level
        );
        return level == expectedLevel;
    }

private:
    struct TrackedHit {
        OreHit hit{};
        std::uint64_t generation{};
    };

    bool needsRecenter(const BlockPos& p) const {
        return std::abs(p.x - origin_.x) >= settings_.recenterHorizontal ||
               std::abs(p.z - origin_.z) >= settings_.recenterHorizontal ||
               std::abs(p.y - origin_.y) >= settings_.recenterVertical;
    }

    void pruneOutsideRadius() {
        const int hr = std::max(1, settings_.horizontalRadius);
        const int vr = std::max(1, settings_.verticalRadius);

        for (auto it = hits_.begin(); it != hits_.end();) {
            const auto& p = it->second.hit.pos;
            if (std::abs(p.x - origin_.x) > hr ||
                std::abs(p.z - origin_.z) > hr ||
                std::abs(p.y - origin_.y) > vr) {
                it = hits_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void finishGeneration() {
        // Anything not rediscovered during the completed pass is stale.
        for (auto it = hits_.begin(); it != hits_.end();) {
            if (it->second.generation != generation_)
                it = hits_.erase(it);
            else
                ++it;
        }
    }

    Settings settings_{};

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, TrackedHit> hits_;

    BlockPos origin_{};
    std::uint64_t cursor_{};
    std::uint64_t generation_{1};
    bool initialized_{false};
};

inline const char* oreName(OreKind kind) {
    switch (kind) {
        case OreKind::Coal:          return "Coal";
        case OreKind::Copper:        return "Copper";
        case OreKind::Iron:          return "Iron";
        case OreKind::Gold:          return "Gold";
        case OreKind::Redstone:      return "Redstone";
        case OreKind::Lapis:         return "Lapis";
        case OreKind::Diamond:       return "Diamond";
        case OreKind::Emerald:       return "Emerald";
        case OreKind::NetherGold:    return "Nether Gold";
        case OreKind::NetherQuartz:  return "Nether Quartz";
        case OreKind::AncientDebris: return "Ancient Debris";
        default:                     return "Unknown";
    }
}

} // namespace mine_mod::xray
