// Copyright (c) Zorkats

#include "gdx_companion_backdrop.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#include <SDL.h>
#include <SDL_opengl.h>

#include "stb_image.h"

#include "gdx_companion_paths.h"
#include "gdx_companion_themes.h"

namespace gdx_companion {

Backdrop::~Backdrop() { Clear(); }

void Backdrop::Clear() {
    if (texture_ != 0) {
        const GLuint id = texture_;
        glDeleteTextures(1, &id);
    }
    texture_ = 0;
    width_ = 0;
    height_ = 0;
}

void Backdrop::Swap(Backdrop& other) noexcept {
    std::swap(texture_, other.texture_);
    std::swap(width_, other.width_);
    std::swap(height_, other.height_);
}

bool Backdrop::Load(const std::string& utf8_path, bool pixel_art, Rgb8 fallback, Rgb8& dominant,
                    std::string& error) {
    Clear();
    dominant = fallback;
    // Read through std::filesystem so non-ASCII paths open on Windows, then decode from memory.
    const std::filesystem::path path = PathFromUtf8(utf8_path);
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxBackdropBytes) {
        error = ec ? "cannot read the image: " + ec.message() : "the image is empty or larger than 32 MiB";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() != size) {
        error = "cannot read the image";
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    const int length = static_cast<int>(bytes.size());
    if (!stbi_info_from_memory(bytes.data(), length, &width, &height, &channels)) {
        error = std::string("not a readable PNG or JPEG: ") + stbi_failure_reason();
        return false;
    }
    if (width <= 0 || height <= 0 || width > kMaxBackdropSide || height > kMaxBackdropSide) {
        error = "the image is " + std::to_string(width) + "x" + std::to_string(height) + "; each side must be " +
                std::to_string(kMaxBackdropSide) + " or less";
        return false;
    }
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), length, &width, &height, &channels, 4);
    if (pixels == nullptr) {
        error = std::string("the image does not decode: ") + stbi_failure_reason();
        return false;
    }
    dominant = DominantColor(pixels, width, height, fallback);

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    const GLint filter = pixel_art ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    texture_ = id;
    width_ = width;
    height_ = height;
    return true;
}

void Backdrop::Draw(ImDrawList* list, ImVec2 min, ImVec2 max) const {
    if (texture_ == 0 || list == nullptr) return;
    const float area_w = max.x - min.x;
    const float area_h = max.y - min.y;
    if (area_w <= 0.0f || area_h <= 0.0f) return;
    // Cover: scale until both sides fill the area, then crop the overflow evenly.
    const float image_aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const float area_aspect = area_w / area_h;
    ImVec2 uv0(0.0f, 0.0f);
    ImVec2 uv1(1.0f, 1.0f);
    if (image_aspect > area_aspect) {
        const float keep = area_aspect / image_aspect;
        uv0.x = (1.0f - keep) * 0.5f;
        uv1.x = uv0.x + keep;
    } else {
        const float keep = image_aspect / area_aspect;
        uv0.y = (1.0f - keep) * 0.5f;
        uv1.y = uv0.y + keep;
    }
    const ImU32 tint = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, kBackdropOpacity));
    list->AddImage(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(texture_)), min, max, uv0, uv1, tint);
}

}  // namespace gdx_companion
