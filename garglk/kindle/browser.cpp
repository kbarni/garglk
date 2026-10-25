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
#include <cmath>
#include <filesystem>

#include "kindle.h"

namespace {

struct Entry {
    std::string name;
    std::string key;
    bool directory;
    // The platform of a game, looked up when the entry is first shown.
    std::optional<std::optional<std::string>> platform;
};

struct Browser {
    kindle::Browse mode;
    std::function<std::optional<std::string>(const std::string &)> classify;
    std::string directory;
    std::vector<Entry> entries;
    std::optional<std::size_t> selected;
    int page = 0;
    int rows = 1;
    double press_x = 0, press_y = 0;
    std::optional<std::string> result;
    GMainLoop *loop = g_main_loop_new(nullptr, false);

    GtkWidget *window = nullptr;
    GtkWidget *up = nullptr;
    GtkWidget *path = nullptr;
    GtkWidget *list = nullptr;
    GtkWidget *name = nullptr;
    GtkWidget *pager = nullptr;
    GtkWidget *previous = nullptr;
    GtkWidget *next = nullptr;
    GtkWidget *page_label = nullptr;
    GtkWidget *accept = nullptr;

    [[nodiscard]] int pages() const {
        return std::max<int>(1, (entries.size() + rows - 1) / rows);
    }

    [[nodiscard]] std::string path_of(const Entry &entry) const {
        return (std::filesystem::path(directory) / entry.name).string();
    }

    [[nodiscard]] std::string typed_name() const {
        std::string text = gtk_entry_get_text(GTK_ENTRY(name));
        auto first = text.find_first_not_of(' ');
        return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(' ') - first + 1);
    }

    const std::optional<std::string> &platform(Entry &entry) const {
        if (!entry.platform.has_value()) {
            entry.platform.emplace(classify && !entry.directory ? classify(path_of(entry)) : std::nullopt);
        }
        return *entry.platform;
    }

    bool selectable(Entry &entry) const {
        return entry.directory || mode != kindle::Browse::Game || !classify || platform(entry).has_value();
    }
};

}

static double row_height()
{
    return kindle::unit() * 2.6;
}

static void update_controls(Browser &browser)
{
    bool can_accept = browser.mode == kindle::Browse::Save ? !browser.typed_name().empty() : browser.selected.has_value();

    gtk_widget_set_sensitive(browser.accept, can_accept);
    gtk_widget_set_sensitive(browser.up, browser.directory != "/");
    gtk_label_set_text(GTK_LABEL(browser.page_label), (std::to_string(browser.page + 1) + " / " + std::to_string(browser.pages())).c_str());
    gtk_widget_set_sensitive(browser.previous, browser.page > 0);
    gtk_widget_set_sensitive(browser.next, browser.page < browser.pages() - 1);
    if (browser.pages() > 1) {
        gtk_widget_show(browser.pager);
    } else {
        gtk_widget_hide(browser.pager);
    }
    gtk_widget_queue_draw(browser.list);
}

static void list_directory(Browser &browser, const std::string &directory)
{
    std::error_code ec;
    auto canonical = std::filesystem::canonical(directory, ec);

    browser.directory = ec ? directory : canonical.string();
    browser.entries.clear();
    for (std::filesystem::directory_iterator item(browser.directory, ec), end; !ec && item != end; item.increment(ec)) {
        auto name = item->path().filename().string();
        if (name[0] == '.') {
            continue;
        }

        gchar *key = g_utf8_collate_key_for_filename(name.c_str(), -1);
        browser.entries.push_back({name, key, item->is_directory(ec), std::nullopt});
        g_free(key);
    }

    // Folders first, then natural ordering, as in the Kindle library.
    std::sort(browser.entries.begin(), browser.entries.end(), [](const Entry &a, const Entry &b) {
        return a.directory != b.directory ? a.directory : a.key < b.key;
    });

    browser.selected.reset();
    browser.page = 0;
    gtk_label_set_text(GTK_LABEL(browser.path), browser.directory.c_str());
    update_controls(browser);
}

static void finish(Browser &browser, std::optional<std::string> result)
{
    browser.result = std::move(result);
    g_main_loop_quit(browser.loop);
}

static void accept(Browser &browser)
{
    if (browser.mode != kindle::Browse::Save) {
        finish(browser, browser.path_of(browser.entries[*browser.selected]));
        return;
    }

    auto name = browser.typed_name();
    auto path = (std::filesystem::path(browser.directory) / name).string();
    gchar *question = g_strdup_printf(kindle::tr("\"%s\" already exists. Do you want to replace it?"), name.c_str());
    bool replace = !std::filesystem::exists(path)
        || kindle::dialog(GTK_WINDOW(browser.window), question, kindle::tr("Replace"), kindle::tr("Cancel"));

    g_free(question);
    if (replace) {
        finish(browser, path);
    }
}

static void turn_page(Browser &browser, int delta)
{
    int page = std::clamp(browser.page + delta, 0, browser.pages() - 1);

    if (page != browser.page) {
        browser.page = page;
        update_controls(browser);
    }
}

static void tap_row(Browser &browser, std::size_t index)
{
    if (index >= browser.entries.size() || !browser.selectable(browser.entries[index])) {
        return;
    }

    Entry &entry = browser.entries[index];
    if (entry.directory) {
        list_directory(browser, browser.path_of(entry));
    } else if (browser.selected == index && browser.mode != kindle::Browse::Save) {
        // A second tap on the selected file opens it.
        accept(browser);
    } else {
        browser.selected = index;
        if (browser.mode == kindle::Browse::Save) {
            gtk_entry_set_text(GTK_ENTRY(browser.name), entry.name.c_str());
        }
        update_controls(browser);
    }
}

static gboolean list_press(GtkWidget *, GdkEventButton *event, Browser *browser)
{
    browser->press_x = event->x;
    browser->press_y = event->y;
    return true;
}

// Swipes turn pages, as e-ink readers do instead of scrolling.
static gboolean list_release(GtkWidget *, GdkEventButton *event, Browser *browser)
{
    double dx = event->x - browser->press_x;
    double dy = event->y - browser->press_y;
    double threshold = kindle::unit() * 2;

    if (std::abs(dx) > threshold && std::abs(dx) > std::abs(dy)) {
        turn_page(*browser, dx < 0 ? 1 : -1);
    } else if (std::abs(dy) > threshold) {
        turn_page(*browser, dy < 0 ? 1 : -1);
    } else {
        tap_row(*browser, browser->page * browser->rows + static_cast<int>(event->y / row_height()));
    }
    return true;
}

static void list_allocate(GtkWidget *, GtkAllocation *allocation, Browser *browser)
{
    int rows = std::max(1, static_cast<int>(allocation->height / row_height()));

    if (rows != browser->rows) {
        browser->page = browser->page * browser->rows / rows;
        browser->rows = rows;
        update_controls(*browser);
    }
}

// The platform of a game, in white on a black band across its icon.
static void draw_badge(cairo_t *cr, PangoLayout *layout, const std::string &platform, double center, double y, double size)
{
    int width, height;

    pango_layout_set_text(layout, platform.c_str(), -1);
    pango_layout_get_pixel_size(layout, &width, &height);

    double band_width = std::max(width + size * 0.16, size * 0.7);
    double band_height = height + size * 0.06;
    double band_y = y + size * 0.5;

    kindle::rounded_rectangle(cr, center - band_width / 2, band_y, band_width, band_height, band_height / 4);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, center - width / 2.0, band_y + (band_height - height) / 2);
    pango_cairo_show_layout(cr, layout);
}

static gboolean list_expose(GtkWidget *widget, GdkEventExpose *, Browser *browser)
{
    cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(widget));
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, nullptr);
    PangoLayout *badge = gtk_widget_create_pango_layout(widget, nullptr);
    PangoFontDescription *font = kindle::font(0.95, false);
    PangoFontDescription *badge_font = kindle::font(0.45, true);
    GtkAllocation allocation;
    double unit = kindle::unit();
    double height = row_height();
    double icon_size = unit * 1.5;
    double text_x = unit * 1.5 + icon_size;
    std::size_t first = browser->page * browser->rows;

    gtk_widget_get_allocation(widget, &allocation);
    double width = allocation.width;
    pango_layout_set_font_description(layout, font);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, (width - text_x - unit * 2.2) * PANGO_SCALE);
    pango_layout_set_font_description(badge, badge_font);

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    if (browser->entries.empty()) {
        int text_width;
        pango_layout_set_width(layout, -1);
        pango_layout_set_text(layout, kindle::tr("This folder is empty"), -1);
        pango_layout_get_pixel_size(layout, &text_width, nullptr);
        cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
        cairo_move_to(cr, (width - text_width) / 2, unit * 3);
        pango_cairo_show_layout(cr, layout);
    }

    for (std::size_t i = first; i < std::min(first + browser->rows, browser->entries.size()); i++) {
        Entry &entry = browser->entries[i];
        double y = (i - first) * height;
        double icon_y = y + (height - icon_size) / 2;
        double ink = browser->selectable(entry) ? 0 : 0.6;
        int text_height;

        if (browser->selected == i) {
            cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
            kindle::rounded_rectangle(cr, unit * 0.3, y + 3, width - unit * 0.6, height - 6, unit * 0.35);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
        } else {
            cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, unit * 0.6, y + height - 0.5);
            cairo_line_to(cr, width - unit * 0.6, y + height - 0.5);
            cairo_stroke(cr);
        }

        cairo_set_source_rgb(cr, ink, ink, ink);
        if (entry.directory) {
            kindle::draw_icon(cr, kindle::Icon::Folder, unit * 0.7, icon_y, icon_size);
            kindle::draw_icon(cr, kindle::Icon::Next, width - unit * 1.8, y + (height - unit) / 2, unit);
        } else if (const auto &platform = browser->platform(entry); platform.has_value()) {
            kindle::draw_icon(cr, kindle::Icon::Game, unit * 0.7, icon_y, icon_size);
            draw_badge(cr, badge, *platform, unit * 0.7 + icon_size / 2, icon_y, icon_size);
            cairo_set_source_rgb(cr, ink, ink, ink);
        } else {
            kindle::draw_icon(cr, kindle::Icon::File, unit * 0.7, icon_y, icon_size);
        }

        pango_layout_set_text(layout, entry.name.c_str(), -1);
        pango_layout_get_pixel_size(layout, nullptr, &text_height);
        cairo_move_to(cr, text_x, y + (height - text_height) / 2);
        pango_cairo_show_layout(cr, layout);
    }

    pango_font_description_free(badge_font);
    pango_font_description_free(font);
    g_object_unref(badge);
    g_object_unref(layout);
    cairo_destroy(cr);
    return true;
}

static GtkWidget *header_new(Browser &browser)
{
    GtkWidget *hbox = gtk_hbox_new(false, kindle::unit() / 2);

    browser.up = kindle::button("", kindle::Icon::Up, true, [&browser] {
        list_directory(browser, std::filesystem::path(browser.directory).parent_path().string());
    });
    browser.path = kindle::label("", 0.85, true);
    gtk_label_set_ellipsize(GTK_LABEL(browser.path), PANGO_ELLIPSIZE_START);
    gtk_box_pack_start(GTK_BOX(hbox), browser.up, false, false, 0);
    gtk_box_pack_start(GTK_BOX(hbox), browser.path, true, true, 0);
    return kindle::bar(hbox, false);
}

static GtkWidget *footer_new(Browser &browser, const std::optional<kindle::Action> &extra)
{
    GtkWidget *hbox = gtk_hbox_new(false, kindle::unit() * 3 / 4);
    GtkWidget *pager_box = gtk_hbox_new(false, 0);
    bool game = browser.mode == kindle::Browse::Game;
    const char *accept_label = game ? kindle::tr("Open")
                             : browser.mode == kindle::Browse::Restore ? kindle::tr("Restore")
                             : kindle::tr("Save");

    if (extra.has_value()) {
        gtk_box_pack_start(GTK_BOX(hbox), kindle::button(extra->label, kindle::Icon::None, true, [&browser, run = extra->run] {
            auto directory = browser.directory;
            run();
            list_directory(browser, directory);
        }), false, false, 0);
    }

    browser.previous = kindle::button("", kindle::Icon::Previous, false, [&browser] { turn_page(browser, -1); });
    browser.next = kindle::button("", kindle::Icon::Next, false, [&browser] { turn_page(browser, 1); });
    browser.page_label = kindle::label("", 0.8, false);
    gtk_box_pack_start(GTK_BOX(pager_box), browser.previous, false, false, 0);
    gtk_box_pack_start(GTK_BOX(pager_box), browser.page_label, false, false, 0);
    gtk_box_pack_start(GTK_BOX(pager_box), browser.next, false, false, 0);
    browser.pager = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(browser.pager), pager_box);
    gtk_box_pack_start(GTK_BOX(hbox), browser.pager, true, true, 0);

    // The action comes first and leaving on the right, as in desktop dialogs.
    browser.accept = kindle::button(accept_label, kindle::Icon::None, true, [&browser] { accept(browser); });
    kindle::set_emphasis(browser.accept, true);
    gtk_box_pack_end(GTK_BOX(hbox), kindle::button(game ? kindle::tr("Quit") : kindle::tr("Cancel"), kindle::Icon::None, true,
                [&browser] { finish(browser, std::nullopt); }), false, false, 0);
    gtk_box_pack_end(GTK_BOX(hbox), browser.accept, false, false, 0);

    return kindle::bar(hbox, true);
}

std::optional<std::string> kindle::browse(Browse mode, const std::string &directory, const std::string &suggestion,
        const std::optional<Action> &extra, const std::function<std::optional<std::string>(const std::string &)> &classify)
{
    Browser browser{mode, classify};
    GdkScreen *screen = gdk_screen_get_default();
    int height = gdk_screen_get_height(screen);
    GtkWidget *vbox = gtk_vbox_new(false, 0);
    GtkWidget *list_box = gtk_event_box_new();
    GdkColor white = {0, 0xffff, 0xffff, 0xffff};

    // Only file names have to be typed.
    if (mode == Browse::Save) {
        height -= keyboard_height();
    }

    browser.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(browser.window), WINDOW_TITLE);
    gtk_window_set_decorated(GTK_WINDOW(browser.window), false);
    gtk_widget_set_size_request(browser.window, gdk_screen_get_width(screen), height);
    gtk_window_set_resizable(GTK_WINDOW(browser.window), false);
    gtk_widget_modify_bg(browser.window, GTK_STATE_NORMAL, &white);

    browser.list = gtk_drawing_area_new();
    gtk_widget_add_events(browser.list, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(browser.list, "expose-event", G_CALLBACK(list_expose), &browser);
    g_signal_connect(browser.list, "button-press-event", G_CALLBACK(list_press), &browser);
    g_signal_connect(browser.list, "button-release-event", G_CALLBACK(list_release), &browser);
    g_signal_connect(browser.list, "size-allocate", G_CALLBACK(list_allocate), &browser);
    gtk_widget_modify_bg(list_box, GTK_STATE_NORMAL, &white);
    gtk_container_add(GTK_CONTAINER(list_box), browser.list);

    gtk_box_pack_start(GTK_BOX(vbox), header_new(browser), false, false, 0);
    gtk_box_pack_start(GTK_BOX(vbox), list_box, true, true, 0);

    if (mode == Browse::Save) {
        GtkWidget *alignment = gtk_alignment_new(0, 0, 1, 1);
        PangoFontDescription *description = font(1.0, false);
        int padding = unit() / 3;

        browser.name = gtk_entry_new();
        gtk_widget_modify_font(browser.name, description);
        pango_font_description_free(description);
        gtk_entry_set_text(GTK_ENTRY(browser.name), suggestion.c_str());
        g_signal_connect(browser.name, "changed", G_CALLBACK(+[](GtkEditable *, Browser *browser) {
            update_controls(*browser);
        }), &browser);
        gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), padding, padding, padding * 3 / 2, padding * 3 / 2);
        gtk_container_add(GTK_CONTAINER(alignment), browser.name);
        gtk_box_pack_start(GTK_BOX(vbox), alignment, false, false, 0);
    }

    gtk_box_pack_start(GTK_BOX(vbox), footer_new(browser, extra), false, false, 0);
    gtk_container_add(GTK_CONTAINER(browser.window), vbox);

    gtk_widget_show_all(browser.window);
    list_directory(browser, directory.empty() ? "/mnt/us" : directory);
    gtk_window_move(GTK_WINDOW(browser.window), 0, 0);
    gtk_grab_add(browser.window);

    if (mode == Browse::Save) {
        gtk_widget_grab_focus(browser.name);
        show_keyboard();
    } else {
        hide_keyboard();
    }

    g_main_loop_run(browser.loop);

    gtk_grab_remove(browser.window);
    gtk_widget_destroy(browser.window);
    g_main_loop_unref(browser.loop);

    return browser.result;
}
