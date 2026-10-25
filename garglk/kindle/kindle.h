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

#ifndef GARGLK_KINDLE_H
#define GARGLK_KINDLE_H

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtk/gtk.h>

// GTK+ 2 interface for jailbroken Kindles, drawn with cairo in the flat black
// and white style of the Kindle user interface. Sizes derive from the screen
// width so they scale across devices.
namespace kindle {

// Window title understood by the Kindle window manager: every window of
// the application keeps the size and position it asks for.
constexpr const char *WINDOW_TITLE = "L:D_N:application_ID:net.fabiszewski.gargoyle_O:URL";

// Translation of the interface, from l10n/<language>.po next to the executable.
const char *tr(const char *msgid);

// On-screen keyboard, driven through LIPC.
void show_keyboard();
void hide_keyboard();
int keyboard_height();
std::optional<std::string> keyboard_language();

enum class Icon { None, Up, Previous, Next, Keyboard, Menu, Folder, File, Game };

int unit();
PangoFontDescription *font(double size, bool bold);
void rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius);
void draw_icon(cairo_t *cr, Icon icon, double x, double y, double size);

GtkWidget *button(const std::string &label, Icon icon, bool framed, std::function<void()> clicked);
void set_emphasis(GtkWidget *button, bool emphasis);
GtkWidget *label(const std::string &text, double size, bool bold);

// A white bar with a rule on its bottom or top edge, as used for headers and footers.
GtkWidget *bar(GtkWidget *content, bool rule_on_top);

// Centered modal dialog; true when the accept button is pressed. Without
// a cancel label it is a message with a single button.
bool dialog(GtkWindow *parent, const std::string &message, const std::string &accept, const std::string &cancel = "",
        GdkPixbuf *picture = nullptr);

// Menu dropping from the top right corner, like the Kindle menu, as wide as
// its longest item. A tap outside closes it; when it lands on another button
// of parent that button acts too, except for opener, the button that opened
// the menu.
std::optional<std::size_t> menu(GtkWindow *parent, GtkWidget *opener, int top, const std::vector<std::string> &items);

enum class Browse { Game, Restore, Save };

struct Action {
    std::string label;
    std::function<void()> run;
};

// Full screen file browser: one paged list with folders first, the path next
// to an up button, and the selection framed. In Game mode, classify names the
// platform of a playable file, which is drawn on its icon; other files cannot
// be selected. The folder is listed again after extra runs.
std::optional<std::string> browse(Browse mode, const std::string &directory, const std::string &suggestion = "",
        const std::optional<Action> &extra = std::nullopt,
        const std::function<std::optional<std::string>(const std::string &)> &classify = nullptr);

}

#endif
