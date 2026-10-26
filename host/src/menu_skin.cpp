// Menu skin: optional pictures for the launcher, so it can look like the game's own menu. Put PNG files (with transparency)
// in assets/menu_skin/ (next to the program, in an "assets" folder; see the README there). Any that are missing are skipped, and the
// launcher keeps its flat wooden look (frontend.cpp's style_plank).
//
//   plank.png           behind each launcher option
//   plank_selected.png  behind the option that is selected (hover or focus)
//   arrow.png           at the left of the selected option
//   title.png           over the top of the screen, centred (replaces the default title)
//   logo.png            (0.10.36) with the video: the logo the background picture has, cut out of it, shown where the picture has it
//                       (in place of the launcher's title).
//   background.mp4      (0.10.33) a video for the whole screen, behind everything, played without sound and looped (background_video.cpp).
//                       With it the background picture isn't used, and the options keep their own planks (plank.png) in the
//                       launcher's column.
//   background.png      the whole screen, behind everything. It is a 4:3 picture (1448 x 1086) shown 1440 x 1080 dp in the middle of the
//                       window; with it the options are laid out to sit on its five planks (a sixth is added under them, and Version
//                       and Add ROM become small buttons in the top-left corner). See the constants below.
//
// The pictures are the user's own, kept on their computer; the repository doesn't contain any.

#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "recompui/recompui.h"
#include "base/ui_launcher.h"
#include "base/ui_game_option.h"
#include "elements/ui_image.h"
#include "util/file.h"
#include "elements/ui_label.h"

#include "conker.hpp"

namespace {
    // The UI's own styling calls don't have z-index; this reaches Style's protected set_property for it.
    struct StyleAccess : recompui::Style {
        static void z_index(recompui::Style* style, int z) {
            void (recompui::Style::*set)(Rml::PropertyId, const Rml::Property&) = &StyleAccess::set_property;
            (style->*set)(Rml::PropertyId::ZIndex, Rml::Property((float)z, Rml::Unit::NUMBER));
        }
    };

    struct SkinnedOption {
        recompui::GameOption* option = nullptr;
        recompui::Image* selected = nullptr;
        recompui::Image* arrow = nullptr;
        bool showing = false;
    };
    std::vector<SkinnedOption> skinned;
    // (0.10.33) the background video's picture in the UI's image store, given each new frame
    const std::string video_key = "?/conker/skin/video";
    bool video_on = false;
    std::vector<char> video_frame;

    // The image's key in the UI's image store, or empty if there's no such file.
    std::string load(const char* name) {
        const std::filesystem::path path = recompui::file::get_asset_path((std::string("menu_skin/") + name + ".png").c_str());
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return {};
        }
        std::ifstream file(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            return {};
        }
        const std::string key = std::string("?/conker/skin/") + name;
        recompui::queue_image_from_bytes_file(key, bytes);
        std::printf("[skin] %s.png loaded (%zu bytes)\n", name, bytes.size());
        return key;
    }

    recompui::Image* make_layer(recompui::Element* parent, const std::string& key) {
        recompui::ContextId context = recompui::get_current_context();
        recompui::Image* image = context.create_element<recompui::Image>(parent, key);
        image->set_position(recompui::Position::Absolute);
        image->set_pointer_events(recompui::PointerEvents::None);
        return image;
    }
}

// The geometry of the 4:3 picture, in dp (the UI is 1080 dp high at any window size, so the picture is always 1440 x 1080
// in the middle of the window): where its first plank is, the distance from one plank to the next, a plank's size and the
// centre of the planks, measured from the middle of the screen.
namespace {
    constexpr float picture_width = 1440.0f, picture_height = 1080.0f;
    constexpr float first_plank_top = 453.0f, plank_pitch = 102.5f, plank_height = 88.0f, plank_width = 566.0f;
    constexpr float plank_centre_offset = 51.0f;
    constexpr int picture_planks = 5;   // planks the picture has; more get one of plank.png added
    // plank.png and plank_selected.png include the plank's dark outline and shadow, a little outside the plank's own box.
    constexpr float outline_left = 8.0f, outline_top = 6.0f, outline_width = 582.0f, outline_height = 104.0f;

    // puts a plank picture over an option's box
    void fit_plank(recompui::Image* image, bool stage) {
        if (stage) {
            image->set_left(-outline_left);
            image->set_top(-outline_top);
            image->set_width(outline_width);
            image->set_height(outline_height);
        }
        else {
            image->set_left(0);
            image->set_top(0);
            image->set_width(100.0f, recompui::Unit::Percent);
            image->set_height(100.0f, recompui::Unit::Percent);
        }
    }
}

void conker::menu_skin::apply(recompui::LauncherMenu* menu, recompui::GameOptionsMenu* options, const std::vector<recompui::GameOption*>& corner) {
    skinned.clear();
    const std::string plank = load("plank");
    const std::string plank_selected = load("plank_selected");
    const std::string arrow = load("arrow");
    const std::string title = load("title");
    // (0.10.33) a background video, if there's one and it plays, in place of the background picture
    video_on = conker::background_video::start(recompui::file::get_asset_path("menu_skin/background.mp4"));
    const std::string background = video_on ? std::string() : load("background");
    const bool stage = !background.empty();
    if (video_on) {
        recompui::Image* image = make_layer(menu->get_background_wrapper(), video_key);
        image->set_top(0);
        image->set_left(0);
        image->set_width(100.0f, recompui::Unit::Percent);
        image->set_height(100.0f, recompui::Unit::Percent);
        // (0.10.34) the options laid out as on the picture's planks (each on its own plank.png), under the launcher's title, and
        // Version and Add ROM small buttons in the top-left corner
        options->unset_bottom();
        options->set_top(first_plank_top - 24.0f);
        options->set_width(plank_width);
        options->set_margin_left(plank_centre_offset);   // (where the picture has its planks)
        options->set_gap(plank_pitch - plank_height);
        // (0.10.36; 0.10.37 the whole sign) the picture's logo over the video, where the picture has it: its pixels 457 to 1046
        // across and 26 to 415 down (of 1448 x 1086, shown 1440 x 1080 dp), so 586 x 387 dp from 26 dp down, its middle 27 dp
        // right of the screen's
        const std::string logo = load("logo");
        if (!logo.empty()) {
            menu->remove_default_title();
            recompui::Image* image = make_layer(menu->get_background_wrapper(), logo);
            image->set_top(26.0f);
            image->set_left(50.0f, recompui::Unit::Percent);
            image->set_margin_left(27.0f - 586.0f / 2.0f);
            image->set_width(586.0f);
            image->set_height(387.0f);
        }
    }
    const bool layout = stage || video_on;   // the picture's layout (with the video, every plank is plank.png)

    if (stage) {
        menu->remove_default_title();   // the picture has the title
        recompui::Image* image = make_layer(menu->get_background_wrapper(), background);
        image->set_top(0);
        image->set_left(50.0f, recompui::Unit::Percent);
        image->set_translate_2D(-50.0f, 0.0f, recompui::Unit::Percent);
        image->set_width(picture_width);
        image->set_height(picture_height);
        // the options sit on the picture's planks
        options->unset_bottom();
        options->set_top(first_plank_top - 24.0f);   // (the menu's container is 24 dp inside the screen)
        options->set_width(plank_width);
        options->set_margin_left(plank_centre_offset);
        options->set_gap(plank_pitch - plank_height);
    }
    if (!title.empty()) {
        menu->remove_default_title();
        recompui::Image* image = make_layer(menu, title);
        image->set_top(3.0f, recompui::Unit::Percent);
        image->set_left(50.0f, recompui::Unit::Percent);
        image->set_translate_2D(-50.0f, 0.0f, recompui::Unit::Percent);
        image->set_width(46.0f, recompui::Unit::Percent);
        image->set_height_auto();
    }
    if (plank.empty() && plank_selected.empty() && arrow.empty() && !layout) {
        return;
    }
    int index = 0;
    int corner_index = 0;
    for (recompui::GameOption* option : options->get_options()) {
        const bool in_corner = layout && std::find(corner.begin(), corner.end(), option) != corner.end();
        if (in_corner) {
            // small buttons in the top-left corner (they're out of the column: the menu is where its first plank is)
            option->set_position(recompui::Position::Absolute);
            option->set_left(-470.0f);
            option->set_top(-420.0f + 56.0f * (float)corner_index++);
            option->set_width(330.0f);
            option->set_padding(6.0f);
            option->set_rotation(0.0f);
            option->set_background_color(recompui::Color{ 20, 12, 6, 190 });
            option->set_border_width(0.0f);
            if (recompui::Label* label = option->get_label()) {
                label->set_font_size(26.0f);
            }
            continue;
        }
        SkinnedOption entry;
        entry.option = option;
        const bool picture_has_plank = stage && index < picture_planks;
        if (!plank.empty() || layout) {
            option->set_background_color(recompui::Color{ 0, 0, 0, 0 });
            option->set_border_width(0.0f);
            option->hover_style.set_background_color(recompui::Color{ 0, 0, 0, 0 });
            option->focus_style.set_background_color(recompui::Color{ 0, 0, 0, 0 });
            option->set_rotation(0.0f);
            option->set_padding(0.0f);
            option->set_margin_bottom(0.0f);
            option->set_height(layout ? plank_height : 84.0f);
            option->set_min_height(layout ? plank_height : 84.0f);
            if (recompui::Label* label = option->get_label()) {
                label->set_font_size(40.0f);
            }
        }
        if (!plank.empty() && !picture_has_plank) {
            fit_plank(make_layer(option, plank), layout);
        }
        if (!plank_selected.empty()) {
            entry.selected = make_layer(option, plank_selected);
            fit_plank(entry.selected, layout);
            entry.selected->display_hide();
        }
        if (!arrow.empty()) {
            entry.arrow = make_layer(option, arrow);
            entry.arrow->set_top(50.0f, recompui::Unit::Percent);
            entry.arrow->set_left(layout ? -34.0f : 20.0f);
            entry.arrow->set_translate_2D(0.0f, -50.0f, recompui::Unit::Percent);
            entry.arrow->set_height(60.0f);
            entry.arrow->set_width_auto();
            entry.arrow->display_hide();
        }
        // the lettering above the pictures
        if (recompui::Label* label = option->get_label()) {
            label->set_position(recompui::Position::Relative);
            StyleAccess::z_index(label, 5);
            // (0.10.38) a black outline round the letters on the planks, so they read over the wood and the video
            label->set_attribute("style", "font-effect: outline(3dp #000000);");
        }
        skinned.push_back(entry);
        index++;
    }
}

// Every frame the launcher is up (from update_launcher, with the launcher's context open).
void conker::menu_skin::update() {
    if (video_on) {
        uint32_t width = 0, height = 0;
        if (conker::background_video::take_frame(video_frame, width, height)) {
            // the new frame under the same name, and the old one let go: the picture loads it as it's next drawn
            recompui::queue_image_from_bytes_rgba32(video_key, video_frame, width, height);
            recompui::release_image(video_key);
        }
    }
    for (SkinnedOption& entry : skinned) {
        const bool selected = entry.option->is_style_enabled(recompui::focus_state) || entry.option->is_style_enabled(recompui::hover_state);
        if (selected == entry.showing) {
            continue;
        }
        entry.showing = selected;
        for (recompui::Image* image : { entry.selected, entry.arrow }) {
            if (image != nullptr) {
                if (selected) {
                    image->display_show();
                }
                else {
                    image->display_hide();
                }
            }
        }
    }
}
