// Copyright (c) Zorkats
//
// The theme's backdrop art: decoded once with stb_image, kept as one OpenGL texture, and drawn
// behind the whole window at kBackdropOpacity over the theme's base colour, scaled to cover the
// window and cropped at the centre. Call every method on the thread that owns the GL context.
#pragma once

#include <cstdint>
#include <string>

#include "imgui.h"
#include "gdx_companion_theme.h"

namespace gdx_companion {

constexpr int kMaxBackdropSide = 8192;

class Backdrop {
public:
    Backdrop() = default;
    Backdrop(const Backdrop&) = delete;
    Backdrop& operator=(const Backdrop&) = delete;
    ~Backdrop();

    // Replaces the current image with the file at utf8_path. dominant receives the image's dominant
    // colour (DominantColor), or fallback when no pixel is opaque. Returns false with error set, and
    // no image, when the file cannot be read or decoded or a side exceeds kMaxBackdropSide.
    bool Load(const std::string& utf8_path, bool pixel_art, Rgb8 fallback, Rgb8& dominant, std::string& error);
    void Clear();
    void Swap(Backdrop& other) noexcept;
    bool HasImage() const { return texture_ != 0; }

    // Draws the image over [min, max] on list. Does nothing without an image.
    void Draw(ImDrawList* list, ImVec2 min, ImVec2 max) const;

private:
    unsigned int texture_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace gdx_companion
