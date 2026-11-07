// port/gdx_alt_assets.h -- the resource side of the Workshop "Use alternate assets" switch.
//
// libultraship's ResourceManager serves "alt/<path>" in place of <path> while alt assets are on,
// but it looks in its cache first. A base resource cached while the switch was off would keep
// being served, so turning the switch on marks those resources dirty. The switch itself and the
// texture caches live in gdx_workshop.cpp; this file links against libultraship only, so a test
// can drive it with a real ResourceManager.

#ifndef GDX_ALT_ASSETS_H
#define GDX_ALT_ASSETS_H

#include <cstddef>
#include <string>

namespace Ship {
class ArchiveManager;
class ResourceManager;
}

// Marks dirty every cached base resource that a mounted archive has an alt/ entry for, so its next
// load goes through the alt/ lookup. Resources without an alt/ entry are left alone. Returns how
// many were marked.
size_t GdxAltAssetsDirtyShadowed(Ship::ResourceManager& rm);

// The archive path the texture-pack channel serves for key, or "" when no mounted pack has one.
// With alt assets on, "alt/textures/pack/<key>" comes before "textures/pack/<key>", so a pack can
// keep its art under alt/ and the switch turns it on and off without a reload.
std::string GdxAltAssetsPackOverridePath(Ship::ArchiveManager& am, const std::string& key, bool altOn);

#endif // GDX_ALT_ASSETS_H
