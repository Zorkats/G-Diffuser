// port/gdx_alt_assets.cpp -- see gdx_alt_assets.h.

#include "gdx_alt_assets.h"

#include "ship/resource/Resource.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"

size_t GdxAltAssetsDirtyShadowed(Ship::ResourceManager& rm) {
    auto am = rm.GetArchiveManager();
    auto files = (am != nullptr) ? am->ListFiles(Ship::IResource::gAltAssetPrefix + "*") : nullptr;
    if (files == nullptr) {
        return 0;
    }
    const size_t prefix = Ship::IResource::gAltAssetPrefix.length();
    size_t marked = 0;
    for (const auto& path : *files) {
        if (path.size() <= prefix) {
            continue;
        }
        // loadExact: the base path itself, not its alt/ entry.
        auto cached = rm.GetCachedResource(path.substr(prefix), true);
        if (cached != nullptr) {
            cached->Dirty();
            marked++;
        }
    }
    return marked;
}

std::string GdxAltAssetsPackOverridePath(Ship::ArchiveManager& am, const std::string& key, bool altOn) {
    const std::string packPath = "textures/pack/" + key;
    if (altOn) {
        std::string altPath = Ship::IResource::gAltAssetPrefix + packPath;
        if (am.HasFile(altPath)) {
            return altPath;
        }
    }
    return am.HasFile(packPath) ? packPath : std::string();
}
