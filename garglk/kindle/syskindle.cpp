// Copyright (C) 2006-2009 by Tor Andersson.
// Copyright (C) 2010 by Ben Cressey, Chris Spiegel.
//
// This file is part of Gargoyle.
//
// Gargoyle is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// Gargoyle is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Gargoyle; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>

#include <gdk/gdkkeysyms.h>
#include <unistd.h>

#include "glk.h"
#include "garglk.h"

#include "kindle.h"

static GtkWidget *frame;
static GtkWidget *canvas;
static GtkWidget *header_title;
static GtkWidget *menu_button;
static GtkIMContext *imcontext;

static guint timer_id;
static bool timed_out;

// The last text selected in the game window, which the core stores when a
// double tap selects a word.
static std::vector<glui32> cliptext;

// A swipe also ends in a button release, which must not count as a tap.
static bool swiped;

void glk_request_timer_events(glui32 ms)
{
    if (timer_id != 0) {
        g_source_remove(timer_id);
        timer_id = 0;
    }

    if (ms != 0) {
        timer_id = g_timeout_add(ms, [](gpointer) -> gboolean {
            timed_out = true;
            return true;
        }, nullptr);
    }
}

void gli_notification_waiting()
{
    g_main_context_wakeup(nullptr);
}

static GtkWindow *parent_window()
{
    return frame != nullptr ? GTK_WINDOW(frame) : nullptr;
}

void garglk::winabort(const std::string &msg)
{
    std::cerr << "fatal: " << msg << std::endl;
    kindle::dialog(parent_window(), msg, kindle::tr("Close"));
    gli_exit(EXIT_FAILURE);
}

void garglk::winwarning(const std::string &, const std::string &msg)
{
    std::cerr << "warning: " << msg << std::endl;
    kindle::dialog(parent_window(), msg, kindle::tr("Close"));
}

void winexit()
{
    gli_exit(0);
}

static std::string gamedata_directory()
{
    switch (gli_conf_gamedata_location) {
    case GamedataLocation::Fixed:
        return gli_conf_gamedata_dir;
    case GamedataLocation::Gamedir:
        return gli_workdir;
    case GamedataLocation::Dedicated: {
        auto name = std::filesystem::path(gli_workfile.value_or("")).filename();
        auto dir = std::filesystem::path(g_get_user_data_dir()) / "gargoyle" / "gamedata" / name;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.string();
    }
    default:
        // The Kindle launcher points this to the saved_games folder.
        const char *saves = std::getenv("SAVED_GAMES");
        return saves != nullptr ? saves : gli_workdir;
    }
}

std::string garglk::winopenfile(const char *, FileFilter)
{
    return kindle::browse(kindle::Browse::Restore, gamedata_directory()).value_or("");
}

// The suggested name is the story name with the date, so saves do not
// replace each other by default.
std::string garglk::winsavefile(const char *, FileFilter filter)
{
    static const std::unordered_map<FileFilter, const char *> extensions = {
        {FileFilter::Save, "glksave"},
        {FileFilter::Text, "txt"},
        {FileFilter::Data, "glkdata"},
    };
    auto story = std::filesystem::path(gli_story_name.empty() ? "game" : gli_story_name).stem().string();
    std::time_t now = std::time(nullptr);
    char stamp[32];

    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M", std::localtime(&now));
    auto suggestion = story + "-" + stamp + "." + extensions.at(filter);
    return kindle::browse(kindle::Browse::Save, gamedata_directory(), suggestion).value_or("");
}

void winclipstore(const std::vector<glui32> &text)
{
    cliptext = text;
}

static int window_height()
{
    int height = gdk_screen_get_height(gdk_screen_get_default());
    return gli_conf_fullscreen ? height : height - kindle::keyboard_height();
}

static void fit_window()
{
    GdkGeometry geometry;

    geometry.min_width = geometry.max_width = gdk_screen_get_width(gdk_screen_get_default());
    geometry.min_height = geometry.max_height = window_height();
    gtk_window_set_geometry_hints(GTK_WINDOW(frame), frame, &geometry, GdkWindowHints(GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE));
    gtk_window_resize(GTK_WINDOW(frame), geometry.min_width, geometry.min_height);
}

// A hidden keyboard leaves its area to the game, as in full screen mode,
// which suits external keyboards.
static void toggle_keyboard()
{
    gli_conf_fullscreen = !gli_conf_fullscreen;

    // Games opened later from the game list keep the choice.
    setenv("GARGOYLE_FULLSCREEN", gli_conf_fullscreen ? "1" : "0", true);

    if (gli_conf_fullscreen) {
        kindle::hide_keyboard();
    } else {
        kindle::show_keyboard();
    }
    fit_window();
}

// Glk offers no way to ask a game to save or restore, so the standard
// commands are typed for it on a cleared input line, after skipping the
// pages still to read, which would swallow the keys.
static void type_command(const std::string &command)
{
    gli_input_handle_key(keycode_End);
    gli_input_handle_key(keycode_Escape);
    for (unsigned char c : command) {
        gli_input_handle_key(c);
    }
    gli_input_handle_key(keycode_Return);
}

static void quit()
{
    if (kindle::dialog(GTK_WINDOW(frame), kindle::tr("Are you sure you want to quit? You will lose all unsaved progress!"),
                kindle::tr("Quit"), kindle::tr("Cancel"))) {
        winexit();
    }
}

// The launcher sits next to the interpreters and shows the game list when
// started without a game.
static void return_to_list()
{
    if (!kindle::dialog(GTK_WINDOW(frame), kindle::tr("Go back to the game list? You will lose all unsaved progress!"),
                kindle::tr("Game list"), kindle::tr("Cancel"))) {
        return;
    }

    auto dir = garglk::winappdir();
    if (dir.has_value()) {
        auto launcher = *dir + "/gargoyle";
        execl(launcher.c_str(), launcher.c_str(), static_cast<char *>(nullptr));
    }
}

static void open_menu(GtkWidget *bar)
{
    GtkAllocation allocation;
    gtk_widget_get_allocation(bar, &allocation);

    switch (kindle::menu(GTK_WINDOW(frame), menu_button, allocation.height, {
                kindle::tr("Save game"),
                kindle::tr("Restore game"),
                kindle::tr("Game list"),
                kindle::tr("Quit"),
            }).value_or(-1)) {
    case 0: type_command("save"); break;
    case 1: type_command("restore"); break;
    case 2: return_to_list(); break;
    case 3: quit(); break;
    }
}

static GtkWidget *header_bar()
{
    GtkWidget *hbox = gtk_hbox_new(false, kindle::unit() / 2);
    GtkWidget *bar = kindle::bar(hbox, false);

    header_title = kindle::label("", 0.9, true);
    gtk_label_set_ellipsize(GTK_LABEL(header_title), PANGO_ELLIPSIZE_END);
    menu_button = kindle::button("", kindle::Icon::Menu, false, [bar] { open_menu(bar); });

    gtk_box_pack_start(GTK_BOX(hbox), header_title, true, true, 0);
    gtk_box_pack_start(GTK_BOX(hbox), kindle::button("", kindle::Icon::Keyboard, false, toggle_keyboard), false, false, 0);
    gtk_box_pack_start(GTK_BOX(hbox), menu_button, false, false, 0);
    return bar;
}

static void on_resize(GtkWidget *, GtkAllocation *allocation, gpointer)
{
    // The first allocation happens before the game starts, so it must not
    // create an arrange event.
    static bool first = true;

    if (allocation->width != gli_image_rgb.width() || allocation->height != gli_image_rgb.height()) {
        gli_windows_size_change(allocation->width, allocation->height, !first);
        first = false;
    }
}

static gboolean on_expose(GtkWidget *widget, GdkEventExpose *event, gpointer)
{
    GdkRectangle area;
    GdkRectangle image = {0, 0, gli_image_rgb.width(), gli_image_rgb.height()};

    if (!gli_drawselect || gli_force_redraw) {
        gli_windows_redraw();
    } else {
        gli_drawselect = false;
    }

    // GTK+ 2.20, as on the Kindle, blits RGB data without converting the
    // whole image as cairo would.
    if (gdk_rectangle_intersect(&event->area, &image, &area)) {
        gdk_draw_rgb_image(gtk_widget_get_window(widget), gtk_widget_get_style(widget)->black_gc,
                area.x, area.y, area.width, area.height, GDK_RGB_DITHER_NONE,
                gli_image_rgb.data() + area.y * gli_image_rgb.stride() + area.x * 3, gli_image_rgb.stride());
    }
    return true;
}

// A double tap on a word of the story types it on the input line, as
// there is no clipboard to paste from.
static void type_selected_word()
{
    auto first = std::find_if(cliptext.begin(), cliptext.end(), g_unichar_isalnum);
    auto last = std::find_if_not(first, cliptext.end(), g_unichar_isalnum);
    window_t *win = gli_focuswin;

    if (first == last || win == nullptr || win->type != wintype_TextBuffer || (!win->line_request && !win->line_request_uni)) {
        return;
    }

    window_textbuffer_t *buffer = win->winbuffer();
    if (buffer->scrollpos != 0) {
        gli_input_handle_key(keycode_End);
    }
    if (buffer->incurs > buffer->infence && buffer->chars[buffer->incurs - 1] != ' ') {
        gli_input_handle_key(' ');
    }
    std::for_each(first, last, [](glui32 c) { gli_input_handle_key(g_unichar_tolower(c)); });
    gli_input_handle_key(' ');
}

// Two-finger taps edit the input line by thirds of the screen: clear it,
// delete the previous word and skip a word left on the left; history in
// the middle; delete a character and the next word, and show or hide the
// keyboard on the right.
static void two_finger_tap(int x, int y)
{
    static const glui32 keys[2][3] = {
        {keycode_Escape, keycode_DeleteWordLeft, keycode_SkipWordLeft},
        {keycode_Erase, keycode_DeleteWordRight, 0},
    };
    int width = gli_image_rgb.width();
    int height = gli_image_rgb.height();
    int column = std::clamp(x * 3 / std::max(width, 1), 0, 2);
    int row = std::clamp(y * 3 / std::max(height, 1), 0, 2);

    if (column == 1) {
        gli_input_handle_key(y < height / 2 ? keycode_Up : keycode_Down);
    } else if (column == 2 && row == 2) {
        toggle_keyboard();
    } else {
        gli_input_handle_key(keys[column / 2][row]);
    }
}

static gboolean on_button_press(GtkWidget *, GdkEventButton *event, gpointer)
{
    if (event->button != 1) {
        if (event->type == GDK_BUTTON_PRESS) {
            two_finger_tap(event->x, event->y);
        }
    } else if (event->type == GDK_2BUTTON_PRESS) {
        if (!swiped) {
            cliptext.clear();
            gli_input_handle_click(event->x, event->y, 2);
            type_selected_word();
            gli_clear_selection();
        }
    } else if (event->type == GDK_BUTTON_PRESS) {
        gli_input_handle_click(event->x, event->y, 1);
    }
    return true;
}

static gboolean on_button_release(GtkWidget *, GdkEventButton *event, gpointer)
{
    if (event->button == 1) {
        gli_copyselect = false;
        swiped = false;
    }
    return true;
}

// Swipes page through the story vertically and move the cursor
// horizontally.
static gboolean on_scroll(GtkWidget *, GdkEventScroll *event, gpointer)
{
    static const std::unordered_map<int, glui32> keys = {
        {GDK_SCROLL_UP, keycode_PageUp},
        {GDK_SCROLL_DOWN, keycode_PageDown},
        {GDK_SCROLL_LEFT, keycode_Right},
        {GDK_SCROLL_RIGHT, keycode_Left},
    };

    swiped = true;
    gli_input_handle_key(keys.at(event->direction));
    return true;
}

static void on_input(GtkIMContext *, gchar *input, gpointer)
{
    for (const gchar *p = input; *p != '\0'; p = g_utf8_next_char(p)) {
        gli_input_handle_key(g_utf8_get_char(p));
    }
}

static gboolean on_key_press(GtkWidget *, GdkEventKey *event, gpointer)
{
    static const std::unordered_map<guint, glui32> control_keys = {
        {GDK_a, keycode_Home},
        {GDK_b, keycode_Left},
        {GDK_d, keycode_Erase},
        {GDK_e, keycode_End},
        {GDK_f, keycode_Right},
        {GDK_h, keycode_Delete},
        {GDK_k, keycode_KillLine},
        {GDK_n, keycode_Down},
        {GDK_p, keycode_Up},
        {GDK_u, keycode_Escape},
        {GDK_w, keycode_DeleteWordLeft},
        {GDK_y, keycode_Yank},
        {GDK_Left, keycode_SkipWordLeft},
        {GDK_Right, keycode_SkipWordRight},
    };
    static const std::unordered_map<guint, glui32> keys = {
        {GDK_Return, keycode_Return},
        {GDK_KP_Enter, keycode_Return},
        {GDK_BackSpace, keycode_Delete},
        {GDK_Delete, keycode_Erase},
        {GDK_Tab, keycode_Tab},
        {GDK_Prior, keycode_PageUp},
        {GDK_Next, keycode_PageDown},
        {GDK_Home, keycode_Home},
        {GDK_End, keycode_End},
        {GDK_Left, keycode_Left},
        {GDK_Right, keycode_Right},
        {GDK_Up, keycode_Up},
        {GDK_Down, keycode_Down},
        {GDK_Escape, keycode_Escape},
        {GDK_F1, keycode_Func1},
        {GDK_F2, keycode_Func2},
        {GDK_F3, keycode_Func3},
        {GDK_F4, keycode_Func4},
        {GDK_F5, keycode_Func5},
        {GDK_F6, keycode_Func6},
        {GDK_F7, keycode_Func7},
        {GDK_F8, keycode_Func8},
        {GDK_F9, keycode_Func9},
        {GDK_F10, keycode_Func10},
        {GDK_F11, keycode_Func11},
        {GDK_F12, keycode_Func12},
    };

    if ((event->state & GDK_CONTROL_MASK) != 0) {
        auto key = control_keys.find(gdk_keyval_to_lower(event->keyval));
        if (key != control_keys.end()) {
            gli_input_handle_key(key->second);
        }
    } else if (!gtk_im_context_filter_keypress(imcontext, event)) {
        auto key = keys.find(event->keyval);
        // Keys the input method does not commit include those of
        // layouts other than Latin-1, such as Cyrillic.
        gunichar ch = gdk_keyval_to_unicode(event->keyval);
        if (key != keys.end()) {
            gli_input_handle_key(key->second);
        } else if (ch >= 32) {
            gli_input_handle_key(ch);
        }
    }
    return true;
}

static gboolean on_key_release(GtkWidget *, GdkEventKey *event, gpointer)
{
    if ((event->state & GDK_CONTROL_MASK) == 0) {
        gtk_im_context_filter_keypress(imcontext, event);
    }
    return true;
}

void wininit()
{
    gtk_init(nullptr, nullptr);

    // Text is sized from the screen width like the rest of the interface:
    // 2.6 times its desktop size on the 1072 pixels of a 300 ppi Paperwhite.
    gli_backingscalefactor = gdk_screen_get_width(gdk_screen_get_default()) / 412.0;

    // E-ink screens have no subpixels.
    gli_conf_lcd = false;
}

void winopen()
{
    // The keyboard choice of the previous game outlives garglk.ini.
    if (const char *env = std::getenv("GARGOYLE_FULLSCREEN"); env != nullptr) {
        gli_conf_fullscreen = std::atoi(env) != 0;
    }

    frame = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_can_focus(frame, true);
    gtk_widget_add_events(frame, GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(frame, "key-press-event", G_CALLBACK(on_key_press), nullptr);
    g_signal_connect(frame, "key-release-event", G_CALLBACK(on_key_release), nullptr);
    g_signal_connect(frame, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer) { winexit(); }), nullptr);
    g_signal_connect_after(frame, "focus-in-event", G_CALLBACK(+[](GtkWidget *, GdkEventFocus *, gpointer) -> gboolean {
        if (!gli_conf_fullscreen) {
            kindle::show_keyboard();
        }
        return false;
    }), nullptr);

    canvas = gtk_drawing_area_new();
    gtk_widget_add_events(canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK);
    g_signal_connect(canvas, "size-allocate", G_CALLBACK(on_resize), nullptr);
    g_signal_connect(canvas, "expose-event", G_CALLBACK(on_expose), nullptr);
    g_signal_connect(canvas, "button-press-event", G_CALLBACK(on_button_press), nullptr);
    g_signal_connect(canvas, "button-release-event", G_CALLBACK(on_button_release), nullptr);
    g_signal_connect(canvas, "scroll-event", G_CALLBACK(on_scroll), nullptr);

    GtkWidget *vbox = gtk_vbox_new(false, 0);
    gtk_box_pack_start(GTK_BOX(vbox), header_bar(), false, false, 0);
    gtk_box_pack_start(GTK_BOX(vbox), canvas, true, true, 0);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    imcontext = gtk_im_multicontext_new();
    g_signal_connect(imcontext, "commit", G_CALLBACK(on_input), nullptr);

    gtk_window_set_title(GTK_WINDOW(frame), kindle::WINDOW_TITLE);
    wintitle();
    fit_window();
    gtk_widget_show_all(frame);

    // GTK+ expects the client window before filtering key events.
    gtk_im_context_set_client_window(imcontext, gtk_widget_get_window(frame));
    gtk_widget_grab_focus(frame);
}

// The window title is reserved for the window manager, so the story title
// goes to the header bar.
void wintitle()
{
    if (header_title != nullptr) {
        auto title = !gli_story_title.empty() ? gli_story_title : std::filesystem::path(gli_story_name).stem().string();
        gtk_label_set_text(GTK_LABEL(header_title), title.c_str());
    }
}

void winrepaint(int x0, int y0, int x1, int y1)
{
    gtk_widget_queue_draw_area(canvas, x0, y0, x1 - x0, y1 - y0);
}

// The screen is e-ink, so dark themes only cost contrast.
bool windark()
{
    return false;
}

// The header bar always shows the title, including the prompt to exit
// once the game is over.
bool garglk::winisfullscreen()
{
    return false;
}

std::optional<std::string> garglk::winappdir()
{
    char path[4096];
    ssize_t length = readlink("/proc/self/exe", path, sizeof path);

    if (length <= 0 || length == sizeof path) {
        return std::nullopt;
    }
    return std::filesystem::path(std::string(path, length)).parent_path().string();
}

// Fonts and themes are installed next to the executables.
std::optional<std::string> garglk::winfontpath(const std::string &filename)
{
    return winappdir().value_or(".") + "/" + filename;
}

std::string garglk::windatadir()
{
    return winappdir().value_or(".");
}

std::vector<std::string> garglk::winthemedirs()
{
    return {windatadir() + "/themes"};
}

std::optional<std::string> garglk::winlegacythemedir()
{
    return std::nullopt;
}

void gli_select(event_t *event, bool polled)
{
    gli_event_clearevent(event);

    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
    gli_dispatch_event(event, polled);

    if (!polled) {
        while (event->type == evtype_None && !timed_out) {
            gtk_main_iteration();
            gli_dispatch_event(event, polled);
        }
    }

    if (event->type == evtype_None && timed_out) {
        gli_event_store(evtype_Timer, nullptr, 0, 0);
        gli_dispatch_event(event, polled);
        timed_out = false;
    }
}

// Games whose information was already shown, by IFID.
static std::filesystem::path shown_games_file()
{
    return std::filesystem::path(g_get_user_data_dir()) / "gargoyle" / "info_shown";
}

static bool first_showing(const std::string &ifid)
{
    auto path = shown_games_file();
    std::ifstream in(path);
    std::string line;

    while (std::getline(in, line)) {
        if (line == ifid) {
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::app) << ifid << '\n';
    return true;
}

// The cover and blurb of a game, like the details page of a book.
void garglk::show_game_info(const garglk::GameInfo &info, bool show_once)
{
    if (show_once && info.ifid.has_value() && !first_showing(*info.ifid)) {
        return;
    }

    GdkPixbuf *cover = nullptr;
    if (info.cover.has_value()) {
        GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
        // A third of the screen width, keeping the proportions.
        g_signal_connect(loader, "size-prepared", G_CALLBACK(+[](GdkPixbufLoader *loader, int width, int height, gpointer) {
            double scale = gdk_screen_get_width(gdk_screen_get_default()) / 3.0 / std::max(width, height);
            gdk_pixbuf_loader_set_size(loader, width * scale, height * scale);
        }), nullptr);
        bool written = gdk_pixbuf_loader_write(loader, info.cover->data(), info.cover->size(), nullptr);
        if (gdk_pixbuf_loader_close(loader, nullptr) && written) {
            cover = GDK_PIXBUF(g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader)));
        }
        g_object_unref(loader);
    }

    std::string text = info.title + "\n" + info.author;
    if (info.headline.has_value()) {
        text += "\n" + *info.headline;
    }
    for (const auto &paragraph : info.description) {
        text += "\n\n" + paragraph;
    }

    // Long descriptions are cut so the dialog fits on the screen.
    constexpr glong MAX_LENGTH = 700;
    if (g_utf8_strlen(text.c_str(), -1) > MAX_LENGTH) {
        text.resize(g_utf8_offset_to_pointer(text.c_str(), MAX_LENGTH) - text.c_str());
        text += "\u2026";
    }

    kindle::dialog(nullptr, text, kindle::tr("Play"), "", cover);
    if (cover != nullptr) {
        g_object_unref(cover);
    }
}
