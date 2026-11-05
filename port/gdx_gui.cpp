#include "gdx_gui.h"

#include <imgui.h>

#include "ship/Context.h"
#include "ship/window/gui/Fonts.h"
#include "ship/window/gui/IconsFontAwesome4.h"

#include "gdx_imgui_nav.h"

namespace {

ImFont* sFontStandard = nullptr;
ImFont* sFontLarge = nullptr;
ImFont* sFontMono = nullptr;

ImFont* LoadFontWithIcons(const std::string& path, float size) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig textConfig;
    textConfig.OversampleH = 2;
    textConfig.OversampleV = 2;

    // ImGui loads Latin-1 alone by default, so the dashes, bullets, ellipses and arrows in UI text would draw
    // as '?'. Montserrat and Inconsolata both carry these blocks.
    static const ImWchar textRanges[] = {
        0x0020, 0x00FF, // Basic Latin, Latin-1 Supplement
        0x2000, 0x206F, // General Punctuation
        0x2100, 0x214F, // Letterlike Symbols
        0x2190, 0x21FF, // Arrows
        0,
    };
    ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &textConfig, textRanges);
    if (font == nullptr) {
        return nullptr;
    }

    static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = size * 2.0f / 3.0f;
    io.Fonts->AddFontFromMemoryCompressedBase85TTF(fontawesome_compressed_data_base85, size * 2.0f / 3.0f,
                                                   &iconConfig, iconRanges);
    return font;
}

} // namespace

void GdxFast3dGui::ImGuiWMInit() {
    // The renderer backend is initialized after this virtual returns, so fonts added here land in
    // its first font-atlas upload.
    Fast::Fast3dGui::ImGuiWMInit();

    const std::string fontRoot = Ship::Context::GetPathRelativeToAppDirectory("fonts/");
    sFontStandard = LoadFontWithIcons(fontRoot + "Montserrat-Regular.ttf", 18.0f);
    sFontLarge = LoadFontWithIcons(fontRoot + "Montserrat-Regular.ttf", 22.0f);
    sFontMono = LoadFontWithIcons(fontRoot + "Inconsolata-Regular.ttf", 17.0f);

    // Missing loose assets must never block boot. ImGui's default font already includes Font
    // Awesome through libultraship, so it is a complete fallback.
    ImFont* fallback = ImGui::GetIO().Fonts->Fonts.empty() ? nullptr : ImGui::GetIO().Fonts->Fonts[0];
    if (sFontStandard == nullptr) {
        sFontStandard = fallback;
    }
    if (sFontLarge == nullptr) {
        sFontLarge = sFontStandard;
    }
    if (sFontMono == nullptr) {
        sFontMono = sFontStandard;
    }
    if (sFontStandard != nullptr) {
        ImGui::GetIO().FontDefault = sFontStandard;
    }
}

void GdxFast3dGui::ImGuiWMNewFrame() {
    Fast::Fast3dGui::ImGuiWMNewFrame();
    // Must land between the platform backend's gamepad poll (just above) and ImGui::NewFrame, which
    // the caller runs next: the feed both reads the backend's HasGamepad claim and queues key events
    // for this frame. See port/gdx_imgui_nav.h.
    gdx_imgui_nav_tick();
}

ImFont* GdxGuiFontStandard() {
    return sFontStandard;
}

ImFont* GdxGuiFontLarge() {
    return sFontLarge;
}

ImFont* GdxGuiFontMono() {
    return sFontMono;
}
