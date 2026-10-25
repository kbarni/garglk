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
#include <utility>

#include "kindle.h"

// Amazon Ember is the Kindle interface font; the others are fallbacks.
static constexpr const char *FONT_FAMILY = "Amazon Ember,Bookerly,Sans";

namespace {

struct Button {
    std::string label;
    kindle::Icon icon;
    bool framed;
    std::function<void()> clicked;
    bool emphasis = false;
    bool menu_item = false;
    bool rule_below = false;
    bool armed = false;
};

struct Popup {
    GMainLoop *loop = g_main_loop_new(nullptr, false);
    int response = -1;

    void respond(int value) {
        response = value;
        if (g_main_loop_is_running(loop)) {
            g_main_loop_quit(loop);
        }
    }
};

struct MenuTap {
    Popup popup;
    std::optional<GdkPoint> outside;
};

}

static Button *button_of(GtkWidget *widget)
{
    return static_cast<Button *>(g_object_get_data(G_OBJECT(widget), "kindle-button"));
}

int kindle::unit()
{
    return std::max(gdk_screen_get_width(gdk_screen_get_default()) / 36, 12);
}

PangoFontDescription *kindle::font(double size, bool bold)
{
    PangoFontDescription *font = pango_font_description_from_string(FONT_FAMILY);

    pango_font_description_set_absolute_size(font, size * unit() * PANGO_SCALE);
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    return font;
}

static PangoLayout *layout_new(GtkWidget *widget, const std::string &text, double size, bool bold)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text.c_str());
    PangoFontDescription *description = kindle::font(size, bold);

    pango_layout_set_font_description(layout, description);
    pango_font_description_free(description);
    return layout;
}

void kindle::rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -G_PI / 2, 0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, G_PI / 2);
    cairo_arc(cr, x + radius, y + height - radius, radius, G_PI / 2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

// Draws a polyline given as x, y pairs in units of the icon size.
static void polyline(cairo_t *cr, double x, double y, double s, std::initializer_list<double> points, bool closed = false)
{
    for (auto p = points.begin(); p != points.end(); p += 2) {
        if (p == points.begin()) {
            cairo_move_to(cr, x + p[0] * s, y + p[1] * s);
        } else {
            cairo_line_to(cr, x + p[0] * s, y + p[1] * s);
        }
    }
    if (closed) {
        cairo_close_path(cr);
    }
}

void kindle::draw_icon(cairo_t *cr, Icon icon, double x, double y, double s)
{
    cairo_save(cr);
    cairo_set_line_width(cr, std::max(s / 12, 2.0));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    switch (icon) {
    case Icon::Previous:
        polyline(cr, x, y, s, {0.62, 0.2, 0.32, 0.5, 0.62, 0.8});
        cairo_stroke(cr);
        break;
    case Icon::Next:
        polyline(cr, x, y, s, {0.38, 0.2, 0.68, 0.5, 0.38, 0.8});
        cairo_stroke(cr);
        break;
    case Icon::Up:
        polyline(cr, x, y, s, {0.5, 0.85, 0.5, 0.18});
        polyline(cr, x, y, s, {0.2, 0.46, 0.5, 0.16, 0.8, 0.46});
        cairo_stroke(cr);
        break;
    case Icon::Keyboard:
        rounded_rectangle(cr, x + 0.05 * s, y + 0.24 * s, 0.9 * s, 0.54 * s, 0.08 * s);
        cairo_stroke(cr);
        for (int i = 0; i < 5; i++) {
            cairo_rectangle(cr, x + (0.17 + i * 0.15) * s, y + 0.35 * s, 0.07 * s, 0.07 * s);
            cairo_rectangle(cr, x + (0.17 + i * 0.15) * s, y + 0.48 * s, 0.07 * s, 0.07 * s);
        }
        cairo_fill(cr);
        polyline(cr, x, y, s, {0.3, 0.66, 0.7, 0.66});
        cairo_stroke(cr);
        break;
    case Icon::Menu:
        for (int i = 0; i < 3; i++) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, x + 0.5 * s, y + (0.22 + i * 0.28) * s, s / 11, 0, 2 * G_PI);
        }
        cairo_fill(cr);
        break;
    case Icon::Folder: {
        cairo_pattern_t *ink = cairo_pattern_reference(cairo_get_source(cr));
        polyline(cr, x, y, s, {0.08, 0.22, 0.38, 0.22, 0.47, 0.32, 0.92, 0.32, 0.92, 0.8, 0.08, 0.8}, true);
        cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
        cairo_fill_preserve(cr);
        cairo_set_source(cr, ink);
        cairo_pattern_destroy(ink);
        cairo_stroke(cr);
        break;
    }
    case Icon::File:
    case Icon::Game:
        polyline(cr, x, y, s, {0.22, 0.1, 0.6, 0.1, 0.78, 0.28, 0.78, 0.9, 0.22, 0.9}, true);
        polyline(cr, x, y, s, {0.6, 0.1, 0.6, 0.28, 0.78, 0.28});
        // The page of a game leaves room for the name of its platform.
        for (int i = 0; icon == Icon::File && i < 3; i++) {
            polyline(cr, x, y, s, {0.34, 0.46 + i * 0.13, 0.66, 0.46 + i * 0.13});
        }
        cairo_stroke(cr);
        break;
    case Icon::None:
        break;
    }

    cairo_restore(cr);
}

static double icon_size(const Button &button)
{
    return kindle::unit() * (button.label.empty() ? 1.3 : 1.1);
}

static double content_width(const Button &button, int text_width)
{
    double width = text_width;

    if (button.icon != kindle::Icon::None) {
        width += icon_size(button) + (button.label.empty() ? 0 : kindle::unit() * 0.3);
    }
    return width;
}

static void button_size_request(GtkWidget *widget, GtkRequisition *requisition, Button *button)
{
    int text_width = 0;
    int unit = kindle::unit();

    if (!button->label.empty()) {
        PangoLayout *layout = layout_new(widget, button->label, 0.85, button->emphasis);
        pango_layout_get_pixel_size(layout, &text_width, nullptr);
        g_object_unref(layout);
    }

    requisition->width = content_width(*button, text_width) + unit * (button->label.empty() ? 0.9 : 1.6);
    requisition->height = unit * 2.1;
}

static gboolean button_expose(GtkWidget *widget, GdkEventExpose *, Button *button)
{
    cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(widget));
    GtkAllocation allocation;
    double unit = kindle::unit();
    double ink = gtk_widget_is_sensitive(widget) ? 0 : 0.62;
    int text_width = 0, text_height = 0;
    PangoLayout *layout = nullptr;

    gtk_widget_get_allocation(widget, &allocation);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    if (button->rule_below) {
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, unit * 0.6, allocation.height - 0.5);
        cairo_line_to(cr, allocation.width - unit * 0.6, allocation.height - 0.5);
        cairo_stroke(cr);
    }

    cairo_set_source_rgb(cr, ink, ink, ink);

    if (button->framed) {
        cairo_set_line_width(cr, button->emphasis ? 3 : 2);
        kindle::rounded_rectangle(cr, 2, 2, allocation.width - 4, allocation.height - 4, unit * 0.35);
        cairo_stroke(cr);
    }

    if (!button->label.empty()) {
        layout = layout_new(widget, button->label, 0.85, button->emphasis);
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
    }

    double x = button->menu_item ? unit : (allocation.width - content_width(*button, text_width)) / 2;

    if (button->icon != kindle::Icon::None) {
        double size = icon_size(*button);
        kindle::draw_icon(cr, button->icon, x, (allocation.height - size) / 2, size);
        x += size + unit * 0.3;
    }

    if (layout != nullptr) {
        cairo_move_to(cr, x, (allocation.height - text_height) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    cairo_destroy(cr);
    return true;
}

// No redraw on press: every refresh is slow on e-ink.
static gboolean button_press(GtkWidget *, GdkEventButton *, Button *button)
{
    button->armed = true;
    return true;
}

// Only a press that started on the button counts, not the end of a tap
// that closed a menu.
static gboolean button_release(GtkWidget *widget, GdkEventButton *event, Button *button)
{
    GtkAllocation allocation;
    bool armed = std::exchange(button->armed, false);

    gtk_widget_get_allocation(widget, &allocation);
    if (armed && gtk_widget_is_sensitive(widget) && button->clicked
            && event->x >= 0 && event->y >= 0 && event->x < allocation.width && event->y < allocation.height) {
        button->clicked();
    }
    return true;
}

GtkWidget *kindle::button(const std::string &label, Icon icon, bool framed, std::function<void()> clicked)
{
    GtkWidget *widget = gtk_drawing_area_new();
    auto *button = new Button{label, icon, framed, std::move(clicked)};

    g_object_set_data_full(G_OBJECT(widget), "kindle-button", button, [](gpointer data) {
        delete static_cast<Button *>(data);
    });
    gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(widget, "size-request", G_CALLBACK(button_size_request), button);
    g_signal_connect(widget, "expose-event", G_CALLBACK(button_expose), button);
    g_signal_connect(widget, "button-press-event", G_CALLBACK(button_press), button);
    g_signal_connect(widget, "button-release-event", G_CALLBACK(button_release), button);
    return widget;
}

void kindle::set_emphasis(GtkWidget *widget, bool emphasis)
{
    button_of(widget)->emphasis = emphasis;
    gtk_widget_queue_resize(widget);
}

GtkWidget *kindle::label(const std::string &text, double size, bool bold)
{
    GtkWidget *label = gtk_label_new(text.c_str());
    PangoFontDescription *description = font(size, bold);
    GdkColor black = {0, 0, 0, 0};

    gtk_widget_modify_font(label, description);
    gtk_widget_modify_fg(label, GTK_STATE_NORMAL, &black);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    pango_font_description_free(description);
    return label;
}

static void paint_white(GtkWidget *widget)
{
    GdkColor white = {0, 0xffff, 0xffff, 0xffff};
    gtk_widget_modify_bg(widget, GTK_STATE_NORMAL, &white);
}

static gboolean bar_expose(GtkWidget *widget, GdkEventExpose *, gpointer rule_on_top)
{
    cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(widget));
    GtkAllocation allocation;

    gtk_widget_get_allocation(widget, &allocation);
    double y = rule_on_top != nullptr ? 1 : allocation.height - 1;
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, 0, y);
    cairo_line_to(cr, allocation.width, y);
    cairo_stroke(cr);
    cairo_destroy(cr);
    return false;
}

GtkWidget *kindle::bar(GtkWidget *content, bool rule_on_top)
{
    GtkWidget *bar = gtk_event_box_new();
    GtkWidget *alignment = gtk_alignment_new(0, 0.5, 1, 0);
    int padding = unit() / 3;

    paint_white(bar);
    gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), padding, padding, padding, padding);
    gtk_container_add(GTK_CONTAINER(alignment), content);
    gtk_container_add(GTK_CONTAINER(bar), alignment);
    g_signal_connect_after(bar, "expose-event", G_CALLBACK(bar_expose), rule_on_top ? bar : nullptr);
    return bar;
}

// A window whose white panel is framed by three pixels of black. Popups
// never take the focus, so the keyboard stays as it is.
static GtkWidget *popup_new(GtkWindow *parent, GtkWidget *&panel)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *border = gtk_event_box_new();
    GtkWidget *alignment = gtk_alignment_new(0, 0, 1, 1);
    GdkColor black = {0, 0, 0, 0};

    gtk_window_set_title(GTK_WINDOW(window), kindle::WINDOW_TITLE);
    gtk_window_set_decorated(GTK_WINDOW(window), false);
    gtk_window_set_modal(GTK_WINDOW(window), true);
    gtk_window_set_resizable(GTK_WINDOW(window), false);
    gtk_window_set_accept_focus(GTK_WINDOW(window), false);
    gtk_window_set_focus_on_map(GTK_WINDOW(window), false);
    if (parent != nullptr) {
        gtk_window_set_transient_for(GTK_WINDOW(window), parent);
    }

    gtk_widget_modify_bg(border, GTK_STATE_NORMAL, &black);
    gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), 3, 3, 3, 3);
    panel = gtk_event_box_new();
    paint_white(panel);
    gtk_container_add(GTK_CONTAINER(alignment), panel);
    gtk_container_add(GTK_CONTAINER(border), alignment);
    gtk_container_add(GTK_CONTAINER(window), border);
    return window;
}

// Placed before being mapped, so it appears once at its final position.
// Negative coordinates center the window on that axis.
static void popup_show(GtkWidget *window, int x, int y)
{
    GdkScreen *screen = gdk_screen_get_default();
    GtkRequisition size;

    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(window)));
    gtk_widget_size_request(window, &size);
    gtk_window_move(GTK_WINDOW(window),
            x < 0 ? (gdk_screen_get_width(screen) - size.width) / 2 : x,
            y < 0 ? (gdk_screen_get_height(screen) - size.height) / 2 : y);
    gtk_widget_show(window);
    gtk_grab_add(window);
}

static int popup_run(GtkWidget *window, Popup &popup)
{
    g_signal_connect(window, "delete-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, Popup *popup) -> gboolean {
        popup->respond(-1);
        return true;
    }), &popup);
    g_main_loop_run(popup.loop);
    gtk_grab_remove(window);
    gtk_widget_destroy(window);
    g_main_loop_unref(popup.loop);
    return popup.response;
}

bool kindle::dialog(GtkWindow *parent, const std::string &message, const std::string &accept, const std::string &cancel,
        GdkPixbuf *picture)
{
    Popup popup;
    int unit = kindle::unit();
    int width = gdk_screen_get_width(gdk_screen_get_default()) * 4 / 5;
    GtkWidget *panel;
    GtkWidget *window = popup_new(parent, panel);
    GtkWidget *vbox = gtk_vbox_new(false, unit);
    GtkWidget *buttons = gtk_hbox_new(true, unit * 3 / 4);
    GtkWidget *text = label(message, 0.9, false);
    GtkWidget *accept_button = button(accept, Icon::None, true, [&popup] { popup.respond(1); });

    gtk_container_set_border_width(GTK_CONTAINER(vbox), unit);
    if (picture != nullptr) {
        gtk_box_pack_start(GTK_BOX(vbox), gtk_image_new_from_pixbuf(picture), false, false, 0);
    }
    gtk_label_set_line_wrap(GTK_LABEL(text), true);
    gtk_widget_set_size_request(text, width - 2 * unit - 6, -1);
    gtk_box_pack_start(GTK_BOX(vbox), text, false, false, unit / 2);

    // The action comes first and cancelling on the right, as in desktop dialogs.
    set_emphasis(accept_button, true);
    gtk_box_pack_start(GTK_BOX(buttons), accept_button, true, true, 0);
    if (!cancel.empty()) {
        gtk_box_pack_start(GTK_BOX(buttons), button(cancel, Icon::None, true, [&popup] { popup.respond(0); }), true, true, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), buttons, false, false, 0);
    gtk_container_add(GTK_CONTAINER(panel), vbox);

    popup_show(window, -1, -1);
    return popup_run(window, popup) == 1;
}

static bool contains_root(GtkWidget *widget, int x, int y)
{
    GtkAllocation allocation;
    int wx, wy;

    gtk_widget_get_allocation(widget, &allocation);
    gdk_window_get_origin(gtk_widget_get_window(widget), &wx, &wy);
    return x >= wx && y >= wy && x < wx + allocation.width && y < wy + allocation.height;
}

static GtkWidget *button_at(GtkWidget *widget, int x, int y)
{
    if (!gtk_widget_get_visible(widget)) {
        return nullptr;
    }

    if (button_of(widget) != nullptr) {
        return gtk_widget_get_window(widget) != nullptr && contains_root(widget, x, y) ? widget : nullptr;
    }

    std::pair<GtkWidget *, GdkPoint> search{nullptr, {x, y}};
    if (GTK_IS_CONTAINER(widget)) {
        gtk_container_forall(GTK_CONTAINER(widget), [](GtkWidget *child, gpointer data) {
            auto &[found, point] = *static_cast<std::pair<GtkWidget *, GdkPoint> *>(data);
            if (found == nullptr) {
                found = button_at(child, point.x, point.y);
            }
        }, &search);
    }
    return search.first;
}

// Taps outside the menu close it, as on the Kindle. Taps on other windows
// of the application arrive relative to them.
static gboolean menu_press(GtkWidget *widget, GdkEventButton *event, MenuTap *tap)
{
    if (!contains_root(widget, event->x_root, event->y_root)) {
        tap->outside = GdkPoint{static_cast<int>(event->x_root), static_cast<int>(event->y_root)};
        tap->popup.respond(-1);
    }
    return false;
}

std::optional<std::size_t> kindle::menu(GtkWindow *parent, GtkWidget *opener, int top, const std::vector<std::string> &items)
{
    MenuTap tap;
    int unit = kindle::unit();
    int width = unit * 6;
    GtkWidget *panel;
    GtkWidget *window = popup_new(parent, panel);
    GtkWidget *vbox = gtk_vbox_new(false, 0);

    for (const auto &item : items) {
        PangoLayout *layout = layout_new(window, item, 0.85, false);
        int text_width;

        pango_layout_get_pixel_size(layout, &text_width, nullptr);
        width = std::max(width, text_width + unit * 3);
        g_object_unref(layout);
    }

    for (std::size_t i = 0; i < items.size(); i++) {
        GtkWidget *item = button(items[i], Icon::None, false, [&tap, i] { tap.popup.respond(i); });
        Button *look = button_of(item);

        look->menu_item = true;
        look->rule_below = i + 1 < items.size();
        gtk_widget_set_size_request(item, width, unit * 2.6);
        gtk_box_pack_start(GTK_BOX(vbox), item, false, false, 0);
    }
    gtk_container_add(GTK_CONTAINER(panel), vbox);

    popup_show(window, gdk_screen_get_width(gdk_screen_get_default()) - width - 6 - unit / 3, top);

    g_signal_connect(window, "button-press-event", G_CALLBACK(menu_press), &tap);
    gdk_pointer_grab(gtk_widget_get_window(window), true, GDK_BUTTON_PRESS_MASK, nullptr, nullptr, GDK_CURRENT_TIME);

    int response = popup_run(window, tap.popup);
    gdk_pointer_ungrab(GDK_CURRENT_TIME);

    // A tap on another button of the window closes the menu and acts at
    // once, instead of costing a second tap and screen refresh; the opener
    // only closes it.
    if (tap.outside.has_value() && parent != nullptr) {
        GtkWidget *other = button_at(GTK_WIDGET(parent), tap.outside->x, tap.outside->y);
        if (other != nullptr && other != opener && gtk_widget_is_sensitive(other) && button_of(other)->clicked) {
            button_of(other)->clicked();
        }
    }

    if (response < 0) {
        return std::nullopt;
    }
    return response;
}
