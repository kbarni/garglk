// Copyright 2010-2025 Chris Spiegel.
//
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef ZTERP_GLK
extern "C" {
#include <glk.h>
}

#ifndef winmethod_NoBorder
#define winmethod_NoBorder 0
#endif

#if defined(GLK_MODULE_IMAGE) && defined(ZTERP_GLK_BLORB) && !defined(ZTERP_NO_V6)
#define ZTERP_GLK_GRAPHICS

extern "C" {
#include <gi_blorb.h>
}
#endif

// Floating text windows are only used by the V6 hacks, so an overlay
// build is a graphics build as well.
#if defined(ZTERP_GLK_GRAPHICS) && defined(GLK_MODULE_GARGLKOVERLAY)
#define ZTERP_GLK_OVERLAY
#endif

#ifdef ZTERP_GLK_WINGLK
// rpcndr.h, eventually included via WinGlk.h, defines a type “byte”
// which conflicts with the “byte” from memory.h. Temporarily redefine
// it to avoid a compile error.
#define byte rpc_byte
// Windows Glk puts non-standard declarations (specifically for this
// file, those guarded by GLK_MODULE_GARGLKTEXT) in WinGlk.h, so include
// it to get color/style extensions.
#include <WinGlk.h>
#undef byte
#endif
#endif

#include "screen.h"
#include "branch.h"
#include "dict.h"
#include "iff.h"
#include "io.h"
#include "memory.h"
#include "meta.h"
#include "objects.h"
#include "options.h"
#include "osdep.h"
#include "process.h"
#include "sound.h"
#include "stack.h"
#include "stash.h"
#include "types.h"
#include "unicode.h"
#include "util.h"
#include "zterp.h"

#ifdef __DJGPP__
// For some reason, DJGPP does not expose std::round. Manually defining
// std::round like this violates the C++ standard, but since it’s only
// for DJGPP and is known to work with it, this beats compile failure.
namespace std {
double round(double x)
{
    return ::round(x);
}
}
#endif

bool screen_has_overlays()
{
#ifdef ZTERP_GLK_OVERLAY
    return true;
#else
    return false;
#endif
}

// Somewhat ugly hack to get around the fact that some Glk functions may
// not exist. These function calls should all be guarded (e.g.
// if (have_unicode), with have_unicode being set iff GLK_MODULE_UNICODE
// is defined) so they will never be called if the Glk implementation
// being used does not support them, but they must at least exist to
// prevent link errors.
#ifdef ZTERP_GLK
#ifndef GLK_MODULE_UNICODE
#define glk_put_char_uni(...)		die("bug %s:%d: glk_put_char_uni() called with no unicode", __FILE__, __LINE__)
#define glk_put_char_stream_uni(...)	die("bug %s:%d: glk_put_char_stream_uni() called with no unicode", __FILE__, __LINE__)
#define glk_request_char_event_uni(...)	die("bug %s:%d: glk_request_char_event_uni() called with no unicode", __FILE__, __LINE__)
#define glk_request_line_event_uni(...)	die("bug %s:%d: glk_request_line_event_uni() called with no unicode", __FILE__, __LINE__)
#endif
#endif

namespace Attribute {
constexpr auto Wrap   = 1U << 0;
constexpr auto Scroll = 1U << 1;
constexpr auto Script = 1U << 2;
constexpr auto Buffer = 1U << 3;
}

// Flag describing whether the header bit meaning “fixed font” is set.
static bool header_fixed_font;

// This variable stores whether Unicode is supported by the current Glk
// implementation, which determines whether Unicode or Latin-1 Glk
// functions are used. In addition, for both Glk and non-Glk versions,
// this affects the behavior of @check_unicode. In non-Glk versions,
// this is always true.
static bool have_unicode;

struct Window {
    Style style;
    Color fg_color = Color(), bg_color = Color();

    enum class Font { Query, Normal, Picture, Character, Fixed } font = Font::Normal;

    uint8_t attributes = Attribute::Buffer;

#ifdef ZTERP_GLK
    winid_t id = nullptr;
    // In general these are only meaningful for window 1, except that
    // Arthur uses them in window 2 as well.
    uint16_t x = 0, y = 0;
    bool has_echo = false;
#endif
};

static std::array<Window, 8> windows;
static Window *mainwin = &windows[0], *curwin = &windows[0];
#ifdef ZTERP_GLK
// This represents a line of input from Glk; if the global variable
// “have_unicode” is true, then the “unicode” member is used; otherwise,
// “latin1”.
struct Line {
    union {
        std::array<char, 256> latin1;
        std::array<glui32, 256> unicode;
    };
    glui32 len;
};

static Window *upperwin = &windows[1];
static Window statuswin;
static uint16_t upper_window_height = 0;
static uint16_t upper_window_width = 0;
static winid_t errorwin;

#ifdef ZTERP_GLK_OVERLAY
// Reject unknown sizes before overlay placement divides by them.
static bool cell_size(double &width, double &height)
{
    garglk_get_cell_size_pixels(&width, &height);

    return width > 0 && height > 0;
}

// Autorestore can invalidate winid_t values, so only retain the state
// common to all overlays here.
class Overlay {
public:
    enum class Transparency { Opaque, Transparent };

    // Optional colors for text drawn over artwork instead of the
    // user’s background.
    struct Colors {
        glui32 fg;
        glui32 bg;
    };

    explicit Overlay(Transparency transparency) :
        m_transparent(transparency == Transparency::Transparent)
    {
    }

    [[nodiscard]] bool active() const { return m_active; }

    [[nodiscard]] const std::optional<Colors> &colors() const { return m_colors; }

    void show(winid_t win, double left, double top, double right, double bottom, const std::optional<Colors> &colors = std::nullopt) {
        garglk_window_set_overlay(win,
                                  std::round(left), std::round(top),
                                  std::round(right), std::round(bottom),
                                  m_transparent);

        m_active = true;
        m_colors = colors;
    }

    // Let Gargoyle convert the grid dimensions to pixels. Converting
    // them here and having Gargoyle convert them back can lose a row
    // due to truncation.
    void show_grid(winid_t win, double left, double top, glui32 columns, glui32 rows, const std::optional<Colors> &colors = std::nullopt) {
        garglk_window_set_grid_overlay(win, std::round(left), std::round(top), columns, rows, m_transparent);

        m_active = true;
        m_colors = colors;
    }

    // Return whether the window was floating so callers can undo local state.
    bool hide(winid_t win) {
        if (!m_active) {
            return false;
        }

        m_active = false;
        m_colors.reset();

        // Autorestore may already have destroyed the window.
        if (win != nullptr) {
            garglk_window_clear_overlay(win);
        }

        return true;
    }

private:
    bool m_transparent;
    bool m_active = false;
    std::optional<Colors> m_colors;
};

static Overlay upper_window_overlay(Overlay::Transparency::Transparent);
#endif

enum class WindowRock : glui32 {
    None = 0,
    MainWin = 1,
    UpperWin = 2,
    // windows[2-7] are going to be copies of mainwin if they are used at all
    StatusWin = 3,
    ErrorWin = 4,

    // Graphics windows. Glk library autosave (see glkautosave.cpp)
    // closes every window and rebuilds the layout from the saved state,
    // so anything holding a winid_t must be able to find its window
    // again afterward.
    GraphicsWin = 5,
    GraphicsLeftBorder = 6,
    GraphicsRightBorder = 7,
    MysteriousSeparatorWin = 8,
    JourneyWin = 9,
    ArthurWin = 10,
    ShogunMenuWin = 11,
    HintWin = 12,
    ZorkZeroEncWin = 13,
};

#ifdef ZTERP_GLK_GRAPHICS
// Arthur uses window 2 for the larger upper window (room graphics,
// inventory, etc). Graphics are intercepted and drawn separately with
// GraphicsWindow (using Glk’s graphics window). For text mode, use a
// separate Glk text buffer, since Glk doesn’t support the kind of mixed
// graphics and text V6 needs; but if you give Arthur a text buffer
// window as window 2, it just generally works, when in text mode
// (inventory, score, room description).
static Window *arthurwin = &windows[2];

#ifdef ZTERP_GLK_OVERLAY
static Overlay arthur_map_text_overlay(Overlay::Transparency::Opaque);

static Window *shogunmenuwin = &windows[2];

// WINDEF menu geometry, in cells.
static glui32 shogun_menu_rows = 0, shogun_menu_cols = 0;
static glui32 shogun_menu_x = 0;
static Overlay shogun_menu_overlay(Overlay::Transparency::Opaque);

// The shared InvisiClues library addresses window 0 as a grid. Redirect
// it while DO-HINTS has word wrap disabled.
static winid_t hintwin;
static Overlay hint_overlay(Overlay::Transparency::Opaque);

// PICTURED-ENTRY supplies window 3 geometry in artwork pixels.
static Window *zorkzero_encwindow = &windows[3];
static winid_t zorkzero_encwin;
static Overlay zorkzero_enc_overlay(Overlay::Transparency::Transparent);
static int zorkzero_enc_x = 0, zorkzero_enc_y = 0;
static int zorkzero_enc_w = 0, zorkzero_enc_h = 0;

// The four minigames reserve part of a 320x200 sheet for window 0;
// Fanucci also positions window 1 over the board.
static Overlay zorkzero_sheet_text_overlay(Overlay::Transparency::Transparent);

// How far below the top of the sheet the floating grid starts, in
// pixels; see zorkzero_fanucci_lattice_offset().
static double zorkzero_fanucci_grid_offset = 0.0;

// These are positions in the artwork’s 320-wide coordinate space.
// SPLIT-BY-PICTURE uses split as the boundary between the two windows
// and as window 0’s horizontal inset. ADJUST-TEXT-WINDOW uses bottom as
// the distance from the bottom edge of the sheet.
struct ZorkZeroSheet {
    glui32 split;
    glui32 bottom;

    // Only Fanucci writes into window 1.
    bool upper_window;
};

static std::optional<ZorkZeroSheet> zorkzero_sheet;

static constexpr glui32 FANUCCI_MENU_LOC = 384;      // F-MENU-LOC
static constexpr glui32 FANUCCI_SCORE_LOC = 385;     // J-SCORE-LOC
static constexpr glui32 FANUCCI_DISCARD_LOC = 419;   // F-DISCARD-LOC
static constexpr glui32 FANUCCI_CARD_1_LOC = 420;    // F-CARD-1-LOC
static constexpr glui32 FANUCCI_CARD_SPACE = 421;    // F-CARD-SPACE

static constexpr glui32 FANUCCI_MENU_COLUMNS = 5;
static constexpr glui32 FANUCCI_MENU_ROWS = 3;

// A zero-sized split keeps the window out of the layout until it is floated.
static winid_t open_floating_window(glui32 wintype, WindowRock rock)
{
    return glk_window_open(mainwin->id, winmethod_Fixed | winmethod_Above | winmethod_NoBorder, 0, wintype, static_cast<glui32>(rock));
}
#endif

static double aspect_scale()
{
    return options.aspect_correction ? 1.2 : 1.0;
}

static constexpr uint32_t be32(const unsigned char *base)
{
    return
        (static_cast<uint32_t>(base[0]) << 24) |
        (static_cast<uint32_t>(base[1]) << 16) |
        (static_cast<uint32_t>(base[2]) <<  8) |
        (static_cast<uint32_t>(base[3]) <<  0);
}

static constexpr uint32_t blorbid(const char (&type)[5])
{
    return giblorb_make_id(type[0], type[1], type[2], type[3]);
}

#ifdef GLK_MODULE_GARGLKTEXT
static std::optional<glui32> default_bg;
#endif

// For scaling V6 graphics, the size of the whole display is needed. The
// Infocom Blorbs’ Reso chunks give the standard window size as 320x200,
// i.e. the full screen in DOS, so that’s what the scale factor is
// measured against: the full screen.
//
// This width is also used by the graphics window when it’s calculating
// scaling and centering values. It works because the graphics window is
// the same width as the display. That should never change, since all
// existing supported V6 games use this layout, but if it does change,
// this can no longer be used for that purpose.
static std::optional<glui32> full_window_width, full_window_height;

static void find_window_size()
{
    auto gwin = glk_window_open(glk_window_get_root(), winmethod_Above | winmethod_Proportional | winmethod_NoBorder, 100, wintype_Graphics, 0);
    if (gwin != nullptr) {
        glui32 w, h;
        glk_window_get_size(gwin, &w, &h);
        full_window_width = w;
        full_window_height = h;
        glk_window_close(gwin, nullptr);
    }
}

// Glk’s window/graphics model is not suitable for V6’s graphics.
// However, since Infocom produced only 4 V6 games, it’s possible to
// special-case their graphics calls to at least approximate a proper
// apperance, which Bocfel does. In addition to Infocom games, Colin
// Davies’ ports of Brian Howarth’s Mysterious Adventure games are
// supported as well.
//
// For most games, graphics tend to be drawn in a window on top, and
// text below it, which is implemented here by opening a graphics window
// above other windows, and resizing it as necessary. For Journey, the
// dedicated graphics window is on the left side.
//
// To deal with different target machines, Infocom’s graphics
// formats included what they called invisible pictures, which were
// coordinates showing where to draw pictures. Different target machines
// were given different invisible pictures, presumably based on the
// target resolution. Kevin Bracey created Blorb files based on the
// MS-DOS releases, which had a 320x200 target resolution.
//
// But different Infocom games dealt with graphics slightly differently.
//
// Zork Zero: Generally uses invisible pictures to determine where to
// draw things. The invisible pictures effectively instruct Zork Zero to
// draw into a 320x200 region, and it does so correctly, regardless of
// the actual size of the window: Zork Zero doesn’t generally care about
// the window size since it “knows” the size is 320x200 based on the
// invisible pictures. Drawing calls are intercepted and scaled up (or
// down) to the actual window size, and no further work is necessary.
// Zork Zero writes text on top of its graphics, which Glk does not
// support at all (graphics windows don’t support text, and text windows
// don’t support drawing at arbitrary locations). In these cases, the
// text will generally be in the window below the graphics. It looks a
// little odd, but works well enough. Zork Zero also has marginal
// images, which do use the window size to determine where to place
// them, but Bocfel ignores all the intricate setup for marginal
// pictures, and instead just “knows” which images are marginal, and
// instructs Glk to draw them in the margins, which it directly
// supports.
//
// Shogun: Most graphics in Shogun are marginal graphics, and as with
// Zork Zero, are hard coded to be drawn in the margins via Glk calls.
// The Shogun maze does use the upper graphics window, and again as with
// Zork Zero, it generally works using invisible pictures, resulting in
// proper calculations. There is one small hack needed to get an offset
// right, but otherwise the game’s coordinates are used.
//
// Journey: This makes a lot more use of the actual screen size to draw.
// The problem there is that the Glk screen size will likely be some
// value which is nowhere near the 320x200 that Journey is expecting. As
// a result, the handling of graphics is more invasive. Graphics in
// Journey are pictures drawn in a left-hand window that illustrate the
// current scene, but in some cases, these include overlaid images (what
// Infocom called “stamps”): in the cave, near the beginning, for
// example, is a covered pool. Journey draws the image of a covered
// pool. When you pick up the cover, Journey draws an image of a pool of
// water in exactly the same place the cover is. The pool image is a
// stamp, drawn in precisely the right location to hide the cover. To
// handle all this, Bocfel effectively takes over all image drawing. A
// graphics window on the left side, taking up 3/8 of the screen, is
// created. When Journey draws a background image, Bocfel ignores its
// coordinates, and draws it centered in this left-hand window. When
// Journey draws a stamp, Bocfel also ignores the coordinates. Instead,
// it has a table of where all the stamps are supposed to be drawn,
// since they are always in a fixed location. Journey should be able to
// calculate stamp locations, but since Bocfel doesn’t properly track
// windows (yet), it gets the values wrong. As with Shogun, if window
// tracking is eventually properly supported, this table of stamp sizes
// may become obsolete, but for now it works fine.
//
// Arthur: Arthur has two main graphics modes: either showing a picture
// of the current location, or a map. Arthur uses invisible images for
// the map, and draws it correctly with no aid needed (apart from a
// fixed offset as with Shogun’s maze). However, the location pictures
// make a lot of use of the actual window size, which doesn’t currently
// work. Since these images are always drawn in the same place, it’s
// trivial to plot them directly. Arthur also uses stamps like Journey,
// and like in Journey, these stamps’ locations are looked up in a
// table. Ultimately, as with all other games, it’d be nice to remove
// the hard coding and allow the game to calculate everything, but for
// now, this works.
//
// Mysterious Adventures games are generally straightforward in that
// each room has a static image which is displayed in the top window.
// However, the Z-machine versions of the games also have an image which
// serves as a separator, or border, between the upper part of the
// window (containing the image and description/exits/items) and the
// lower part, which contains the input and parser messages. Bocfel
// creates a second graphics window for this border, so that the
// appearance is generally faithful to the original intent.
static enum class Hack {
    None,
    Arthur,
    ZorkZero,
    Shogun,
    Journey,
    MysteriousAdventures,
} hack = Hack::None;

struct ImageSize {
    double width;
    double height;
};

// How wide the strips of artwork down each side of the text are, in the
// artwork’s pixels. Zork Zero’s banner images vary, so its numbers are the
// widest of each side and draw_border() right-aligns the narrower ones.
struct BorderWidths {
    int left;
    int right;
};

// The size of the artwork a graphics window shows, and the borders it
// hangs beside the text below, if any. No borders in the table means
// the screen has none.
struct WindowLayout {
    ImageSize size;
    std::optional<BorderWidths> borders;
};

struct ImageGeometry {
    ImageGeometry(double x_, double y_) : x(x_), y(y_) {}
    double x;
    double y;
};

class GraphicsWindow {
public:
    enum class Type {
        None,
        ArthurIntro,
        ArthurBanner,
        ArthurMap,
        ArthurDemon,
        ZorkZeroBorder,
        ZorkZero320,
        ZorkZeroGame,
        ZorkZeroSheet,
        ZorkZeroSnarfem,
        ShogunTitle,
        ShogunNormal,
        ShogunMaze,

        // The InvisiClues screen, which Zork Zero and Shogun draw with
        // the same artwork in two palettes: a 320x29 strip across the top
        // and a 30x171 panel of question marks down each side below it.
        HintBorder,
        Mysterious,
        MysteriousSeparator,
    };

    enum class Border {
        Left,
        Right,
    };

    // Each instance needs its own set of rocks so that its windows can
    // be differentiated after a Glk library autorestore.
    struct Rocks {
        WindowRock main;
        // Only the types which draw borders (see resize()) need these;
        // an instance which never draws them can leave them empty.
        std::optional<WindowRock> left_border;
        std::optional<WindowRock> right_border;
    };

    explicit GraphicsWindow(Rocks rocks) : m_rocks(rocks) {
    }

    bool create();
    bool resize(Type type);
    void destroy();
    void clear();
    void draw(glui32 pic, const ImageGeometry &geom, glui32 w, glui32 h) const;
    void draw_centered(glui32 pic, glui32 w, glui32 h) const;
    void draw_arthur_banner() const;
    bool draw_zorkzero_border(glui32 pic) const;
    void draw_shogun_borders() const;
    [[nodiscard]] bool is_zorkzero_fullscreen() const;

    // Completely forget state, dropping everything on the floor: in
    // addition to forgetting the windows, we must forget all geometry
    // till the new windows are back and resize() calculates it. This
    // must only be called during a Glk autorestore. Otherwise, it can
    // leak windows. Glk autorestore closes windows behind our back, and
    // then this method gets called so we’re aware of it.
    void forget();

    // As with forget(), this must only be called during Glk
    // autorestore. It, too, can leak windows otherwise. It’s
    // responsible for taking ownership of newly-created windows.
    void recover(winid_t win, WindowRock rock);

#ifdef GLK_MODULE_GARGLKTEXT
    void set_bg_color(const Color &bg);
#endif

    [[nodiscard]] winid_t id() const { return m_id; }
    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] double ratio() const { return m_ratio; }

    // Images are plotted in the window’s own coordinate space: the base
    // size for its type (320x96 for Arthur’s map, and so on), which is
    // also the space the games do their arithmetic in. Glk wants actual
    // window pixels. These convert between the two.
    //
    // x picks up m_x_offset, which centers the image when scale
    // limiting is in effect (it’s normally 0), and y picks up the
    // aspect correction. Every image is drawn at the top of its
    // window, so there is no y offset to match.
    [[nodiscard]] double to_pixel_x(double x) const { return std::round((m_ratio * x) + m_x_offset); }
    [[nodiscard]] double to_pixel_y(double y) const { return std::round(m_ratio * y * aspect_scale()); }
    [[nodiscard]] double from_pixel_x(double x) const { return std::round((x - m_x_offset) / m_ratio); }
    [[nodiscard]] double from_pixel_y(double y) const { return std::round(y / (m_ratio * aspect_scale())); }

private:
    void draw_border(Type type, Border border, glui32 pic) const;

    Rocks m_rocks;
    winid_t m_id = nullptr;
    winid_t m_left_border = nullptr;
    winid_t m_right_border = nullptr;
    Type m_type = Type::None;
    double m_ratio = 0.0;
    double m_x_offset = 0.0;
    ImageSize m_base_size = {0, 0};
};

// The “main” graphics window: this is use for the banner/map in Arthur,
// the maze in Shogun, the banner/map in Zork Zero, and the room images
// in Mysterious Adventures.
static GraphicsWindow graphics_window(GraphicsWindow::Rocks {
    WindowRock::GraphicsWin,
    WindowRock::GraphicsLeftBorder,
    WindowRock::GraphicsRightBorder,
});

// Arthur, Shogun, and Zork Zero all have roughly the same layout, with
// graphics on top and text on the bottom. Journey has a different
// enough layout that it’s not worth trying to shoehorn it into the
// GraphicsWindow class.
static winid_t journey_window;

// Mysterious Adventures have an image separating the “upper window”
// (graphics plus location info) from the main text window. For the main
// graphics window, use the common graphics_window, but dedicate a
// special window for the separator.
static GraphicsWindow mysterious_separator(GraphicsWindow::Rocks {
    WindowRock::MysteriousSeparatorWin,
});

// Mysterious Adventures games always start up with two images: the
// Mysterious Adventures logo, and the game title. These are displayed
// one right after the other, so if they were placed in the graphics
// window, the game title would always immediately overwrite the
// Mysterious Adventures logo. To avoid this, the graphics window isn’t
// opened until after these images are displayed; but these images are
// always the last two in the Blorb, and there are different numbers of
// images for different games, so the image IDs aren‘t the same across
// games. We could map game IDs to image IDs, but it’s simpler to just
// count the total number of images and use that.
static glui32 mysterious_max_image;

static constexpr glui32 SHOGUN_MAZE_BLOCK_WIDTH = 7;
static constexpr glui32 SHOGUN_MAZE_BLOCK_HEIGHT = 7;

// Shogun’s screens are drawn for the same 320x200 display the other two
// are. P-HINT-LOC is the invisible rectangle SETUP-TEXT-AND-STATUS
// reads to inset the InvisiClues windows from the edges of the screen.
static constexpr uint16_t SHOGUN_SCREEN_WIDTH = 320;
static constexpr glui32 SHOGUN_HINT_LOC = 49;

// Zork Zero’s screens are drawn for the same 320x200 display Arthur’s are.
static constexpr uint16_t ZORKZERO_SCREEN_WIDTH = 320;
static constexpr uint16_t ZORKZERO_SCREEN_HEIGHT = 200;

// Arthur draws into window 2, and works out where things go from that
// window’s geometry. Bocfel reports these values rather than the usual
// lies, which is what lets the game place its own compass rose, map and
// room pictures instead of Bocfel having to know where they all go.
//
// The values are what INIT-STATUS-LINE computes for a real V6 screen: M
// is the width of picture 100 (the banner margin, 14 pixels), so window
// 2 starts at M + 1 and is HWRD - 2M wide, and it gets the top half of
// the screen.
static constexpr uint16_t ARTHUR_SCREEN_WIDTH = 320;
static constexpr uint16_t ARTHUR_SCREEN_HEIGHT = 200;
static constexpr uint16_t ARTHUR_BANNER_MARGIN = 14;

static constexpr uint16_t ARTHUR_WINDOW2_XPOS = ARTHUR_BANNER_MARGIN + 1;
static constexpr uint16_t ARTHUR_WINDOW2_YPOS = 1;
static constexpr uint16_t ARTHUR_WINDOW2_WIDTH = ARTHUR_SCREEN_WIDTH - (2 * ARTHUR_BANNER_MARGIN);
static constexpr uint16_t ARTHUR_WINDOW2_HEIGHT = ARTHUR_SCREEN_HEIGHT / 2;

// Where the images the graphics window stands in for sit on that
// screen, as 0-based coordinates.
//
// The map (picture 137) is the full width of the screen, at its origin.
// The banner is picture 54, 314x84, which RT-BANNER-OFFSET centers
// horizontally, and vertically along with the 100-pixel staffs that
// hang below it. Subtracting these turns the game’s screen coordinates
// into coordinates within the image Bocfel is actually showing.
static constexpr int ARTHUR_MAP_IMAGE_X = 0;
static constexpr int ARTHUR_MAP_IMAGE_Y = 0;
static constexpr int ARTHUR_BANNER_IMAGE_X = (ARTHUR_SCREEN_WIDTH - 314) / 2;
static constexpr int ARTHUR_BANNER_IMAGE_Y = (ARTHUR_SCREEN_HEIGHT - (84 + 100)) / 2;

// Arthur’s coordinates are 1-based and relative to window 2; this turns
// a pair of them into a position within the image which the graphics
// window is currently showing.
//
// For room pictures the screen dimensions above cancel out entirely:
// the game centers the picture in window 2, window 2 is centered on the
// screen, and so is the banner, so what comes out is just the picture
// centered in the banner: (314 - width) / 2 across, and (86 - height) /
// 2 down within the banner margin. The screen size only really matters
// to the compass rose, which is placed against window 2’s right edge
// rather than its center.
static ImageGeometry arthur_image_pos(int x, int y, int image_x, int image_y)
{
    // Both coordinates are 1-based, and the result is a 0-based offset
    // within the image, so each contributes a −1.
    return ImageGeometry((x - 1) + (ARTHUR_WINDOW2_XPOS - 1) - image_x,
                         (y - 1) + (ARTHUR_WINDOW2_YPOS - 1) - image_y);
}

// Map a pair of (palette image, requested image) to the ID of a
// precomputed palette image: this corresponds to the BPal chunk.
static std::map<std::pair<glui32, glui32>, glui32> palette_map;

// All APal images for this story.
static std::set<glui32> adaptive_images;

// The current palette: the last non-adaptive image drawn (Blorb §11.3).
static std::optional<glui32> current_palette;

static void build_palette_map()
{
    auto *map = giblorb_get_resource_map();

    if (map == nullptr) {
        return;
    }

    giblorb_result_t res;
    if (giblorb_load_chunk_by_type(map, giblorb_method_Memory, &res, blorbid("BPal"), 0) == giblorb_err_None) {
        if (res.length % 12 == 0) {
            auto *ptr = static_cast<unsigned char *>(res.data.ptr);
            for (size_t i = 0; i < res.length; i += 12) {
                auto source = be32(ptr + i);
                auto apal = be32(ptr + i + 4);
                auto id = be32(ptr + i + 8);
                palette_map.insert({{source, apal}, id});
                adaptive_images.insert(apal);
            }
        } else {
            show_message("Invalid BPal chunk detected; proceeding without adaptive palette");
        }
    } else if (hack == Hack::Arthur || hack == Hack::ZorkZero) {
        show_message("Blorb file is missing a BPal chunk: some colors will be wrong");
    }
}

// Set while adaptive decorations are being redrawn, this prevents the
// drawing code from possibly thinking that drawing the decorations
// should affect the current palette and trigger another decoration
// redraw (and unbounded recursion). That won’t happen with well-formed
// Blorbs, but malformed Blorbs could trigger it.
static bool drawing_adaptive_decorations = false;

// The Blorb standard’s recommendation for dealing with the adaptive
// palette isn’t sufficient for Arthur. It says that APal images (which
// in Arthur’s case is just the banner plus staffs) should take on the
// palette of the last-drawn image; but generally speaking, the banner
// and staffs are not redrawn. Rather, when new rooms are entered,
// they’re expected to already be on screen, and just the room image is
// drawn. For Arthur, when a non-APal picture sets a new palette, redraw
// the banner and staffs in it.
//
// Shogun’s Blorb file doesn’t have an APal entry, but the DOS version
// clearly does palette shifting, so to be future-looking, handle
// Shogun as we do Arthur: Shogun’s borders are not redrawn frequently
// enough to be affected by the palette, so force a redraw as is done
// with Arthur’s banner and staffs.
//
// This is called whenever the current palette is updated.
static void redraw_adaptive_decorations()
{
    drawing_adaptive_decorations = true;

    switch (hack) {
    case Hack::Arthur:
        graphics_window.draw_arthur_banner();
        break;
    case Hack::Shogun:
        graphics_window.draw_shogun_borders();
        break;
    case Hack::None: case Hack::ZorkZero: case Hack::Journey: case Hack::MysteriousAdventures:
        break;
    }

    drawing_adaptive_decorations = false;
}

// “Resolve” the palette for the specified image, which has two distinct
// meanings, but in both cases returns an image ID.
//
// 1. For an APal image, if a current palette exists, return the image
//    ID of associated BPal entry.
// 2. For a non-APal image, set the current palette to it, and then
//    return the original image. As a (necessary) side effect, this also
//    redraws decorations where necessary, to ensure they pick up the
//    new palette.
static glui32 resolve_palette(glui32 pic)
{
    if (adaptive_images.find(pic) != adaptive_images.end()) {
        if (current_palette.has_value()) {
            if (auto id = palette_map.find({*current_palette, pic}); id != palette_map.end()) {
                return id->second;
            }
        }
    } else if (!drawing_adaptive_decorations && current_palette != pic) {
        current_palette = pic;
        redraw_adaptive_decorations();
    }

    return pic;
}

// Wrapper around glk_image_draw_scaled() which handles adaptive palettes.
static void draw_image(winid_t win, glui32 pic, glsi32 val1, glsi32 val2, glui32 width, glui32 height)
{
    auto resolved = resolve_palette(pic);
    glk_image_draw_scaled(win, resolved, val1, val2, width, height);
}

#endif

void screen_recover_glk_windows()
{
    glui32 tmpwid, tmphgt;

    statuswin.id = nullptr;
    errorwin = nullptr;
    for (auto &win : windows) {
        win.id = nullptr;
    }

#ifdef ZTERP_GLK_GRAPHICS
    graphics_window.forget();
    mysterious_separator.forget();
    journey_window = nullptr;
#endif

    glui32 rock = 0;
    for (auto win = glk_window_iterate(nullptr, &rock); win != nullptr; win = glk_window_iterate(win, &rock)) {
#ifdef ZTERP_GLK_OVERLAY
        // Every Overlay is inactive after a restart, so nothing may
        // float, wherever the library put the window back. The games
        // redraw what they know about; the rest waits for the next
        // placement.
        garglk_window_clear_overlay(win);
#endif

        switch (static_cast<WindowRock>(rock)) {
        case WindowRock::None:
            break;
        case WindowRock::MainWin:
            mainwin->id = win;
            break;
        case WindowRock::UpperWin:
            upperwin->id = win;
            glk_window_get_size(upperwin->id, &tmpwid, &tmphgt);
            upper_window_width = tmpwid;
            upper_window_height = tmphgt;
            break;
        case WindowRock::StatusWin:
            statuswin.id = win;
            break;
        case WindowRock::ErrorWin:
            errorwin = win;
            break;
        case WindowRock::GraphicsWin:
        case WindowRock::GraphicsLeftBorder:
        case WindowRock::GraphicsRightBorder:
#ifdef ZTERP_GLK_GRAPHICS
            graphics_window.recover(win, static_cast<WindowRock>(rock));
#endif
            break;
        case WindowRock::MysteriousSeparatorWin:
#ifdef ZTERP_GLK_GRAPHICS
            mysterious_separator.recover(win, static_cast<WindowRock>(rock));
#endif
            break;
        case WindowRock::JourneyWin:
#ifdef ZTERP_GLK_GRAPHICS
            journey_window = win;
#endif
            break;
        case WindowRock::ArthurWin:
#ifdef ZTERP_GLK_GRAPHICS
            arthurwin->id = win;
#endif
            break;
        case WindowRock::ShogunMenuWin:
#ifdef ZTERP_GLK_OVERLAY
            shogunmenuwin->id = win;
#endif
            break;
        case WindowRock::HintWin:
#ifdef ZTERP_GLK_OVERLAY
            hintwin = win;
#endif
            break;
        case WindowRock::ZorkZeroEncWin:
#ifdef ZTERP_GLK_OVERLAY
            zorkzero_encwin = win;
#endif
            break;
        }
    }

    ZASSERT(mainwin->id != nullptr, "no main window after recovery");

    // Redirect windows 2-7 -- see note in init_screen().
    if (options.redirect_v6_windows) {
        for (int i = 2; i < 8; i++) {
            windows[i].id = windows[0].id;
        }
    }
}
#endif

// In all versions but 6, styles and colors are global and stored in
// mainwin. For V6, they’re tracked per window and thus stored in each
// individual window. For convenience, this macro returns the “style
// window” for any version.
static Window *style_window()
{
    return zversion == 6 ? curwin : mainwin;
}

static std::bitset<5> streams;
static std::optional<IO> scriptio, transio, perstransio;

#ifdef ZTERP_GLK_UNIX

void screen_clean_up_glk_streams()
{
    // transio may or may not be a Glk stream, but it’s safest to clean
    // it up.
    transio.reset();
}

#endif

#ifdef ZTERP_GLK

void screen_recover_glk_streams()
{
    transio.reset();
    streams.reset(OSTREAM_TRANSCRIPT);

    glui32 rock = 0;
    for (auto str = glk_stream_iterate(nullptr, &rock); str != nullptr; str = glk_stream_iterate(str, &rock)) {
        switch (static_cast<StreamRock>(rock)) {
        case StreamRock::None:
            break;
        case StreamRock::TranscriptStream:
            transio.emplace(IO::Mode::Append, IO::Purpose::Transcript, str);
            streams.set(OSTREAM_TRANSCRIPT);
            break;
        default:
            break;
        }
    }
}

#endif

class StreamTables {
public:
    void push(uint16_t addr, bool formatted) {
        ZASSERT(m_tables.size() < MAX_STREAM_TABLES, "too many stream tables");
        m_tables.emplace_back(addr, formatted);
    }

    void pop() {
        if (!m_tables.empty()) {
            m_tables.back().finish();
            m_tables.pop_back();
        }
    }

    void write(uint16_t c) {
        ZASSERT(!m_tables.empty(), "invalid stream table");
        m_tables.back().write(c);
    }

    [[nodiscard]] size_t size() const {
        return m_tables.size();
    }

    void clear() {
        m_tables.clear();
    }

private:
    static constexpr size_t MAX_STREAM_TABLES = 16;
    class Table {
    public:
        explicit Table(uint16_t addr, bool formatted) :
            m_addr(addr),
            m_formatted(formatted)
        {
            user_store_word(m_addr, 0);
        }

        Table(const Table &) = delete;
        Table &operator=(const Table &) = delete;

        void finish() const {
            user_store_word(m_addr, m_idx - 2);

            if (m_formatted) {
                user_store_word(m_addr + m_idx, 0);
            }

            if (zversion == 6) {
                store_word(0x30, m_idx - 2);
            }
        }

        void write(uint8_t c) {
            user_store_byte(m_addr + m_idx++, c);
        }

    private:
        uint16_t m_addr;
        bool m_formatted;
        uint16_t m_idx = 2;
    };

    std::list<Table> m_tables;
};

static StreamTables stream_tables;

static int istream = ISTREAM_KEYBOARD;
static std::optional<IO> istreamio;

struct Input {
    enum class Type { Char, Line } type;

    // ZSCII value of key read for @read_char.
    uint8_t key;

    // Unicode line of chars read for @read.
    std::array<uint16_t, 256> line;
    uint8_t maxlen;
    uint8_t len;
    uint8_t preloaded;

    // Character used to terminate input. If terminating keys are not
    // supported by the Glk implementation being used (or if Glk is not
    // used at all) this will be ZSCII_NEWLINE; or in the case of
    // cancellation, 0.
    uint8_t term;
};

// Convert a 15-bit color to a 24-bit color.
uint32_t screen_convert_color(uint16_t color)
{
    // Map 5-bit color values to 8-bit.
    const uint32_t table[] = {
        0x00, 0x08, 0x10, 0x19, 0x21, 0x29, 0x31, 0x3a,
        0x42, 0x4a, 0x52, 0x5a, 0x63, 0x6b, 0x73, 0x7b,
        0x84, 0x8c, 0x94, 0x9c, 0xa5, 0xad, 0xb5, 0xbd,
        0xc5, 0xce, 0xd6, 0xde, 0xe6, 0xef, 0xf7, 0xff
    };

    return table[(color >>  0) & 0x1f] << 16 |
           table[(color >>  5) & 0x1f] <<  8 |
           table[(color >> 10) & 0x1f] <<  0;
}

#ifdef GLK_MODULE_GARGLKTEXT
static glui32 zcolor_map[] = {
    static_cast<glui32>(zcolor_Current),
    static_cast<glui32>(zcolor_Default),

    0x000000,	// Black
    0xef0000,	// Red
    0x00d600,	// Green
    0xefef00,	// Yellow
    0x006bb5,	// Blue
    0xff00ff,	// Magenta
    0x00efef,	// Cyan
    0xffffff,	// White
    0xb5b5b5,	// Light grey
    0x8c8c8c,	// Medium grey
    0x5a5a5a,	// Dark grey
};

void update_color(int which, unsigned long color)
{
    if (which < 2 || which > 12) {
        return;
    }

    zcolor_map[which] = color;
}

// Provide descriptive aliases for Gargoyle styles.
enum {
    GStyleBoldItalicFixed = style_Note,
    GStyleBoldItalic      = style_Alert,
    GStyleBoldFixed       = style_User1,
    GStyleItalicFixed     = style_User2,
    GStyleBold            = style_Subheader,
    GStyleItalic          = style_Emphasized,
    GStyleFixed           = style_Preformatted,
};

static int gargoyle_style(const Style &style)
{
    if (style.test(STYLE_BOLD) && style.test(STYLE_ITALIC) && style.test(STYLE_FIXED)) {
        return GStyleBoldItalicFixed;
    } else if (style.test(STYLE_BOLD) && style.test(STYLE_ITALIC)) {
        return GStyleBoldItalic;
    } else if (style.test(STYLE_BOLD) && style.test(STYLE_FIXED)) {
        return GStyleBoldFixed;
    } else if (style.test(STYLE_ITALIC) && style.test(STYLE_FIXED)) {
        return GStyleItalicFixed;
    } else if (style.test(STYLE_BOLD)) {
        return GStyleBold;
    } else if (style.test(STYLE_ITALIC)) {
        return GStyleItalic;
    } else if (style.test(STYLE_FIXED)) {
        return GStyleFixed;
    }

    return style_Normal;
}

static glui32 gargoyle_color(const Color &color)
{
    switch (color.mode) {
    case Color::Mode::ANSI:
        return zcolor_map[color.value];
    case Color::Mode::True:
        return screen_convert_color(color.value);
    }

    return zcolor_Current;
}

#ifdef ZTERP_GLK_GRAPHICS
static void set_window_bg(winid_t win, const Color &bg)
{
    switch (bg.mode) {
    case Color::Mode::ANSI:
        // 1 is “default” which can’t directly be set by Glk; so use the
        // value we measured on startup.
        if (bg.value == 1 && default_bg.has_value()) {
            glk_window_set_background_color(win, *default_bg);
        } else if (bg.value >= 2 && bg.value <= 12) {
            glk_window_set_background_color(win, zcolor_map[bg.value]);
        }
        break;
    case Color::Mode::True:
        glk_window_set_background_color(win, screen_convert_color(bg.value));
        break;
    }
}
#endif
#endif

#ifdef ZTERP_GLK
// These functions make it so that code elsewhere needn’t check have_unicode before printing.
static void xglk_put_char(uint16_t c)
{
    if (!have_unicode) {
        glk_put_char(c > 255 ? LATIN1_QUESTIONMARK : c);
    } else {
        glk_put_char_uni(c);
    }
}

static void xglk_put_char_stream(strid_t s, uint32_t c)
{
    if (!have_unicode) {
        glk_put_char_stream(s, c > 255 ? LATIN1_QUESTIONMARK : c);
    } else {
        glk_put_char_stream_uni(s, c);
    }
}
#endif

static bool set_force_fixed = false;

#ifdef GLK_MODULE_GARGLKTEXT
// Apply the colors an overlay was placed with, if the current window is
// floating over artwork (see Overlay::Colors). Only the upper window and
// Zork Zero’s window 3 ever are.
static bool set_overlay_colors()
{
#ifdef ZTERP_GLK_OVERLAY
    const Overlay *overlay = nullptr;

    if (curwin == upperwin) {
        overlay = &upper_window_overlay;
    } else if (curwin == zorkzero_encwindow) {
        overlay = &zorkzero_enc_overlay;
    }

    if (overlay == nullptr || !overlay->active() || !overlay->colors().has_value()) {
        return false;
    }

    garglk_set_zcolors(overlay->colors()->fg, overlay->colors()->bg);

    return true;
#else
    return false;
#endif
}
#endif

static void set_window_style(const Window *win)
{
#ifdef ZTERP_GLK
    auto style = win->style;
    if (curwin->id == nullptr) {
        return;
    }

#ifdef GLK_MODULE_GARGLKTEXT
    if (curwin->font == Window::Font::Fixed || header_fixed_font) {
        style.set(STYLE_FIXED);
    }

    if (curwin == mainwin && set_force_fixed) {
        style.set(STYLE_FIXED);
    }

    if (options.disable_fixed) {
        style.reset(STYLE_FIXED);
    }

    glk_set_style(gargoyle_style(style));

    // Reverse video is asked for the same way wherever text goes:
    // Gargoyle paints reversed runs even in a transparent window, which
    // boxes the InvisiClues title.
    garglk_set_reversevideo(style.test(STYLE_REVERSE));

    // Text floating over artwork is drawn in the colors the placement
    // named; the rest keep the game’s.
    if (set_overlay_colors()) {
        return;
    }

    // Colors are per-window in V6, but global in V5.
    if (zversion == 6) {
        garglk_set_zcolors(gargoyle_color(win->fg_color), gargoyle_color(win->bg_color));
    } else {
        garglk_set_zcolors_stream(glk_window_get_stream(mainwin->id), gargoyle_color(win->fg_color), gargoyle_color(win->bg_color));
        if (upperwin->id != nullptr) {
            garglk_set_zcolors_stream(glk_window_get_stream(upperwin->id), gargoyle_color(win->fg_color), gargoyle_color(win->bg_color));
        }
    }
#else
    // Yes, there are three ways to indicate that a fixed-width font should be used.
    bool use_fixed_font = style.test(STYLE_FIXED) || curwin->font == Window::Font::Fixed || header_fixed_font;

    if (curwin == mainwin && set_force_fixed) {
        use_fixed_font = true;
    }

    // Glk can’t mix other styles with fixed-width, but the upper window
    // is always fixed, so if it is selected, there is no need to
    // explicitly request it here. In addition, the user can disable
    // fixed-width fonts or tell Bocfel to assume that the output font is
    // already fixed (e.g. in an xterm); in either case, there is no need
    // to request a fixed font.
    // This means that another style can also be applied if applicable.
    if (use_fixed_font && !options.disable_fixed && !options.assume_fixed && curwin != upperwin) {
        glk_set_style(style_Preformatted);
        return;
    }

    // According to standard 1.1, if mixed styles aren’t available, the
    // priority is Fixed, Italic, Bold, Reverse.
    if (style.test(STYLE_ITALIC)) {
        glk_set_style(style_Emphasized);
    } else if (style.test(STYLE_BOLD)) {
        glk_set_style(style_Subheader);
    } else if (style.test(STYLE_REVERSE)) {
        glk_set_style(style_Alert);
    } else {
        glk_set_style(style_Normal);
    }
#endif
#else
    zterp_os_set_style(win->style, win->fg_color, win->bg_color);
#endif
}

bool screen_toggle_force_fixed()
{
    set_force_fixed = !set_force_fixed;
    set_window_style(mainwin);
    return set_force_fixed;
}

static void set_current_style()
{
    set_window_style(style_window());
}

#ifdef ZTERP_GLK_OVERLAY
// Styles apply to the current Glk stream, so restore the selected
// window first.
static void restore_current_window()
{
    if (curwin->id != nullptr) {
        glk_set_window(curwin->id);
    }

    set_current_style();
}

// The window used when an opcode inspects window 0 itself.
static winid_t hint_grid_window()
{
    return hint_overlay.active() ? hintwin : nullptr;
}

// The window to use for I/O directed to window 0.
static winid_t hint_output_window()
{
    return curwin == mainwin ? hint_grid_window() : nullptr;
}

// Float window 3 over the encyclopedia page. Its geometry uses the
// artwork’s coordinate system and must be scaled like a picture.
static void zorkzero_place_enc_window(bool show)
{
    if (hack != Hack::ZorkZero) {
        return;
    }

    winid_t gwin = graphics_window.id();

    if (!show || gwin == nullptr || graphics_window.ratio() <= 0 ||
        zorkzero_enc_w <= 0 || zorkzero_enc_h <= 0)
    {
        if (zorkzero_enc_overlay.active()) {
            zorkzero_encwindow->id = nullptr;
            if (zorkzero_encwin != nullptr) {
                glk_window_clear(zorkzero_encwin);
            }
            zorkzero_enc_overlay.hide(zorkzero_encwin);
            restore_current_window();
        }
        return;
    }

    if (zorkzero_encwin == nullptr) {
        zorkzero_encwin = open_floating_window(wintype_TextBuffer, WindowRock::ZorkZeroEncWin);
        if (zorkzero_encwin == nullptr) {
            return;
        }
    }

    glsi32 gx, gy;
    garglk_window_get_origin_pixels(gwin, &gx, &gy);

    double left = gx + graphics_window.to_pixel_x(zorkzero_enc_x);
    double top = gy + graphics_window.to_pixel_y(zorkzero_enc_y);
    double right = gx + graphics_window.to_pixel_x(zorkzero_enc_x + zorkzero_enc_w);
    double bottom = gy + graphics_window.to_pixel_y(zorkzero_enc_y + zorkzero_enc_h);

    if (right <= left || bottom <= top) {
        return;
    }

    bool was_floating = zorkzero_enc_overlay.active();

    // Match the DOS foreground, but leave the artwork visible underneath.
    zorkzero_enc_overlay.show(zorkzero_encwin, left, top, right, bottom,
                              Overlay::Colors{0x000000, zcolor_Transparent});

    if (!was_floating) {
        glk_window_clear(zorkzero_encwin);
    }

    zorkzero_encwindow->id = zorkzero_encwin;

    glk_set_window(zorkzero_encwin);
    set_current_style();
}

// Float Shogun’s menu into the gap opened by MAKE-ROOM-FOR. A text buffer
// cannot report the cursor-derived vertical position, so anchor it at bottom.
static void shogun_place_menu_overlay(bool show)
{
    if (hack != Hack::Shogun || shogunmenuwin->id == nullptr) {
        return;
    }

    if (!show || shogun_menu_rows == 0 || shogun_menu_cols == 0) {
        shogun_menu_overlay.hide(shogunmenuwin->id);
        return;
    }

    double cellw, cellh;
    if (!cell_size(cellw, cellh)) {
        return;
    }

    glsi32 mx, my;
    garglk_window_get_origin_pixels(mainwin->id, &mx, &my);

    glui32 mwidth, mheight;
    garglk_window_get_size_pixels(mainwin->id, &mwidth, &mheight);

    glui32 mcols, mrows;
    garglk_cells_in_pixels(mwidth, mheight, &mcols, &mrows);

    glui32 columns = std::min(shogun_menu_cols, mcols);
    glui32 rows = std::min(shogun_menu_rows, mrows);

    if (columns == 0 || rows == 0) {
        return;
    }

    // Keep the box inside the main window even where the game’s
    // centering disagrees with our cell size.
    glui32 x = std::min(shogun_menu_x, mcols - columns);

    double left = mx + (x * cellw);

    double top = my + mheight - (rows * cellh);
    if (top < my) {
        top = my;
    }

    shogun_menu_overlay.show_grid(shogunmenuwin->id, left, top, columns, rows);
}
#endif

// The following implements a circular buffer to track the state of the
// screen so that recent history can be stored in save files for
// playback on restore.
static constexpr size_t HISTORY_SIZE = 2000;

class History {
public:
    struct Entry {
// Suppress a dubious shadow warning (see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=55776)
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
        // These values are part of the Bfhs chunk so must remain stable.
        enum class Type {
            Style = 0,
            FGColor = 1,
            BGColor = 2,
            InputStart = 3,
            InputEnd = 4,
            Char = 5,
        } type;
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

        union Contents {
            Color color;
            uint8_t style;
            uint32_t c;

            Contents() : c(0) { }
            explicit Contents(Color color_) : color(color_) { }
            explicit Contents(uint8_t style_) : style(style_) { }
            explicit Contents(uint32_t c_) : c(c_) { }
        } contents;

        explicit Entry(Type type_) : type(type_) { }
        Entry(Type type_, const Color &color) : type(type_), contents(color) { }
        Entry(Type type_, uint8_t style) : type(type_), contents(style) { }
        Entry(Type type_, uint32_t c) : type(type_), contents(c) { }

        static Entry style(uint8_t style) {
            return {Type::Style, style};
        }

        static Entry color(Type color_type, const Color &color) {
            return {color_type, color};
        }

        static Entry character(uint32_t c) {
            return {Type::Char, c};
        }
    };

    [[nodiscard]] std::deque<Entry>::size_type size() const {
        return m_entries.size();
    }

    void add_style() {
        auto style = mainwin->style;

        if (mainwin->font == Window::Font::Fixed || header_fixed_font) {
            style.set(STYLE_FIXED);
        }

        add(Entry::style(style.to_ulong()));
    }

    void add_fg_color(const Color &color) {
        add(Entry::color(Entry::Type::FGColor, color));
    }

    void add_bg_color(const Color &color) {
        add(Entry::color(Entry::Type::BGColor, color));
    }

    void add_input(const uint16_t *string, size_t len) {
        add(Entry(Entry::Type::InputStart));

        for (size_t i = 0; i < len; i++) {
            add(Entry::character(string[i]));
        }

        add(Entry::character(UNICODE_LINEFEED));
        add(Entry(Entry::Type::InputEnd));
    }

    void add_input_start() {
        add(Entry(Entry::Type::InputStart));
    }

    void add_input_end() {
        add(Entry(Entry::Type::InputEnd));
    }

    void add_char(uint32_t c) {
        add(Entry::character(c));
    }

    [[nodiscard]] const std::deque<Entry> &entries() const {
        return m_entries;
    }

private:
    void add(const Entry &entry) {
        while (m_entries.size() >= HISTORY_SIZE) {
            m_entries.pop_front();
        }

        m_entries.push_back(entry);
    }

    std::deque<Entry> m_entries;
};

static History history;

void screen_set_header_bit(bool set)
{
    if (set != header_fixed_font) {
        header_fixed_font = set;
        history.add_style();
        set_current_style();
    }
}

static void transcribe(uint32_t c)
{
    if (streams.test(OSTREAM_TRANSCRIPT)) {
        transio->putc(c);
    }

    if (perstransio.has_value()) {
        perstransio->putc(c);
    }
}

// Print out a character. The character is in “c” and is either Unicode
// or ZSCII; if the former, “unicode” is true. This is meant for any
// output produced by the game, as opposed to output produced by the
// interpreter, which should use Glk (or standard I/O) calls only, to
// avoid interacting with the Z-machine’s streams: interpreter-provided
// output should not be considered part of a transcript, nor should it
// be included in the memory stream.
static void put_char_base(uint16_t c, bool unicode)
{
    if (c == 0) {
        return;
    }

    if (streams.test(OSTREAM_MEMORY)) {
        // When writing to memory, ZSCII should always be used (§7.5.3).
        if (unicode) {
            c = unicode_to_zscii_q[c];
        }

        stream_tables.write(c);
    } else {
        // For screen and transcription, always prefer Unicode.
        if (!unicode) {
            // Tab (9) and sentence space (11) are defined for output
            // only in V6 (§3.8.2.3 and §3.8.2.4), but Zork I writes out
            // a tab, when you read the FCD#3 guidebook. In the DOS
            // interpreter, this prints out a bullet (CP-437 character
            // 9); on Apple ][ and C64 it prints nothing; and on
            // Macintosh and Amiga it prints a space. This was clearly
            // not intentional, and looks to be a vestige from at least
            // 1977 MDL Zork, which used tabs to center the guidebook’s
            // title. Odds are mainframe Zork just printed the tabs out
            // directly and it worked, and nobody noticed any issues as
            // it was ported to ZIL. In any case, ignore these ZSCII
            // characters when not on V6.
            if (zversion != 6 && (c == 9 || c == 11)) {
                return;
            }

            c = zscii_to_unicode[c];
        }

        if (c != 0) {
            uint8_t zscii = 0;

            // §16 makes no mention of what a newline in font 3 should map to.
            // Other interpreters that implement font 3 assume it stays a
            // newline, and this makes the most sense, so don’t do any
            // translation in that case.
            if (curwin->font == Window::Font::Character && !options.disable_graphics_font && c != UNICODE_LINEFEED) {
                zscii = unicode_to_zscii[c];

                // These four characters have a “built-in” reverse video (see §16).
                if (zscii >= 123 && zscii <= 126) {
                    style_window()->style.flip(STYLE_REVERSE);
                    set_current_style();
                }

                c = zscii_to_font3[zscii];
            }
#ifdef ZTERP_GLK
            if (streams.test(OSTREAM_SCREEN) && curwin->id != nullptr) {
                if (curwin == upperwin) {
                    // Interpreters seem to have differing ideas about what
                    // happens when the cursor reaches the end of a line in the
                    // upper window. Some wrap, some let it run off the edge (or,
                    // at least, stop the text at the edge). The standard, from
                    // what I can see, says nothing on this issue. Follow Windows
                    // Frotz and don’t wrap.

                    if (c == UNICODE_LINEFEED) {
                        if (upperwin->y < upper_window_height) {
                            // Glk wraps, so printing a newline when the cursor has
                            // already reached the edge of the screen will produce two
                            // newlines.
                            if (upperwin->x < upper_window_width) {
                                xglk_put_char(c);
                            }

                            // Even if a newline isn’t explicitly printed here
                            // (because the cursor is at the edge), setting
                            // upperwin->x to 0 will cause the next character to be on
                            // the next line because the text will have wrapped.
                            upperwin->x = 0;
                            upperwin->y++;
                        }
                    } else if (upperwin->x < upper_window_width && upperwin->y < upper_window_height) {
                        upperwin->x++;
                        xglk_put_char(c);
                    }
#ifdef ZTERP_GLK_GRAPHICS
#ifdef ZTERP_GLK_OVERLAY
                } else if (hint_output_window() != nullptr) {
                    // The hint grid is addressed like the upper window,
                    // so the cursor has to be tracked for @erase_line.
                    glui32 w;
                    glk_window_get_size(hint_output_window(), &w, nullptr);
                    if (c == UNICODE_LINEFEED || curwin->x + 1 >= w) {
                        curwin->x = 0;
                        curwin->y++;
                    } else {
                        curwin->x++;
                    }
                    xglk_put_char(c);
#endif
                } else if (hack == Hack::Arthur && curwin == arthurwin && arthurwin->id != nullptr) {
                    // In “room description” mode, this should perform
                    // line breaking, but it currently does not.
                    glui32 w;
                    glk_window_get_size(arthurwin->id, &w, nullptr);
                    if (c == UNICODE_LINEFEED || curwin->x == w) {
                        curwin->x = 0;
                        curwin->y++;
                    } else {
                        curwin->x++;
                    }
                    xglk_put_char(c);
#endif
                } else {
                    xglk_put_char(c);
                }
            }
#else
            if (streams.test(OSTREAM_SCREEN) && curwin == mainwin) {
#ifdef ZTERP_OS_DOS
                // DOS doesn’t support Unicode, but instead uses code
                // page 437. Special-case non-Glk DOS here, by writing
                // bytes (not UTF-8 characters) from code page 437.
                IO::standard_out().write8(unicode_to_437(c));
#else
                IO::standard_out().putc(c);
#endif
            }
#endif

            if (curwin->attributes & Attribute::Script) {
                // Don’t check streams here: for quote boxes (which are in the
                // upper window, and thus not transcribed), both Infocom and
                // Inform games turn off the screen stream and write a duplicate
                // copy of the quote, so it appears in a transcript (if any is
                // occurring). In short, assume that if a game is writing text
                // with the screen stream turned off, it’s doing so with the
                // expectation that it appear in a transcript, which means it also
                // ought to appear in the history.
                history.add_char(c);

                transcribe(c);
            }

            // If the reverse video bit was flipped (for the character font), flip it back.
            if (zscii >= 123 && zscii <= 126) {
                style_window()->style.flip(STYLE_REVERSE);
                set_current_style();
            }
        }
    }
}

static void put_char_u(uint16_t c)
{
    put_char_base(c, true);
}

void put_char(uint8_t c)
{
    put_char_base(c, false);
}

// Glk doesn’t allow control characters (apart from newline) to be
// written (§2.2). In most cases this isn’t a problem, but there are a
// couple of places where user-provided strings need to be printed out
// (/shownotes and history replay), which means we have no control over
// whether those contain control characters. In circumstances where
// possibly-uncontrolled input is being sent to Glk, it is passed
// through here first, which converts invalid characters to hex escape
// sequences.
//
// Since there’s really no need to allow user-provided control
// characters to be written out, this happens in non-Glk mode as well,
// including for transcripts and history.
static std::vector<uint32_t> cleanse_control(uint32_t c)
{
    std::vector<uint32_t> ret;
    if (c == 10 || (c >= 32 && c <= 126) || c >= 160) {
        ret.push_back(c);
    } else {
        std::ostringstream ss;
        ss << "\\x" << std::setw(2) << std::setfill('0') << std::hex << c;
        for (const auto esc : ss.str()) {
            ret.push_back(esc);
        }
    }

    return ret;
}

// Print a string directly to the main window. This is meant to print
// text from the interpreter, not the game. Text will also be written to
// any transcripts which are active, as well as to the history buffer.
// For convenience, carriage returns are ignored under the assumption
// that they are coming from a Windows text stream.
//
// This string should be UTF-8 encoded. If it’s not, invalid sequences
// will be represented as the Unicode replacement character.
void screen_print(const std::string &s)
{
    IO io(std::vector<uint8_t>(s.begin(), s.end()), IO::Mode::ReadOnly);
#ifdef ZTERP_GLK
    strid_t stream = glk_window_get_stream(mainwin->id);
#endif
    for (auto c = io.getc(false); c.has_value(); c = io.getc(false)) {
        if (c != UNICODE_CARRIAGE_RETURN) {
            for (const auto clean : cleanse_control(*c)) {
                transcribe(clean);
                history.add_char(clean);
#ifdef ZTERP_GLK
                xglk_put_char_stream(stream, clean);
#else
                IO::standard_out().putc(clean);
#endif
            }
        }
    }
}

// Print a Unicode character directly to the main window. This is the
// single-character analog of screen_print().
void screen_putc(uint32_t c)
{
    transcribe(c);
    history.add_char(c);
#ifdef ZTERP_GLK
    xglk_put_char_stream(glk_window_get_stream(mainwin->id), c);
#else
    IO::standard_out().putc(c);
#endif
}

void screen_printf(const char *fmt, ...)
{
    std::va_list ap;
    std::string message;

    va_start(ap, fmt);
    message = vstring(fmt, ap);
    va_end(ap);

    screen_print(message);
}

void screen_puts(const std::string &s)
{
    screen_print(s);
    screen_print("\n");
}

void show_message(const char *fmt, ...)
{
    std::va_list ap;
    std::string message;

    va_start(ap, fmt);
    message = vstring(fmt, ap);
    va_end(ap);

#ifdef ZTERP_GLK
    static glui32 error_lines = 0;

    if (errorwin != nullptr) {
        glui32 w, h;

        // Allow multiple messages to stack, but force at least 5 lines to
        // always be visible in the main window. This is less than perfect
        // because it assumes that each message will be less than the width
        // of the screen, but it’s not a huge deal, really; even if the
        // lines are too long, at least Gargoyle and glktermw are graceful
        // enough.
        glk_window_get_size(mainwin->id, &w, &h);

        if (h > 5) {
            glk_window_set_arrangement(glk_window_get_parent(errorwin), winmethod_Below | winmethod_Fixed | winmethod_NoBorder, ++error_lines, errorwin);
        }

        glk_put_char_stream(glk_window_get_stream(errorwin), LATIN1_LINEFEED);
    } else {
        errorwin = glk_window_open(mainwin->id, winmethod_Below | winmethod_Fixed | winmethod_NoBorder, error_lines = 2, wintype_TextBuffer, static_cast<glui32>(WindowRock::ErrorWin));
    }

    // If windows are not supported (e.g. in cheapglk or no Glk), messages
    // will not get displayed. If this is the case, print to the main
    // window.
    strid_t stream;
    if (errorwin != nullptr) {
        stream = glk_window_get_stream(errorwin);
        glk_set_style_stream(stream, style_Alert);
    } else {
        stream = glk_window_get_stream(mainwin->id);
        message = "\n[" + message + "]\n";
    }

    for (size_t i = 0; message[i] != 0; i++) {
        xglk_put_char_stream(stream, char_to_unicode(message[i]));
    }
#else
    {
        std::cout << "\n[" << message << "]\n";
    }
#endif
}

void screen_message_prompt(const std::string &message)
{
    screen_puts(message);
    if (curwin == mainwin) {
        screen_print("\n>");
    }
}

// See §7.
// This returns true if the stream was successfully selected.
// Deselecting a stream is always successful.
static bool output_stream(int16_t number, uint16_t table, bool formatted)
{
    ZASSERT(std::labs(number) <= (zversion >= 3 ? 4 : 2), "invalid output stream selected: %ld", static_cast<long>(number));

    if (number == 0) {
        return true;
    } else if (number > 0) {
        streams.set(number);
    } else if (number < 0) {
        if (number != -3 || stream_tables.size() == 1) {
            streams.reset(-number);
        }
    }

    if (number == 2) {
        store_word(0x10, word(0x10) | FLAGS2_TRANSCRIPT);
        if (!transio.has_value()) {
            try {
                // If autosave_librarystate, we open this as a Glk stream so
                // that it will be part of the librarystate.
                transio.emplace(options.transcript_name, options.overwrite_transcript ? IO::Mode::WriteOnly : IO::Mode::Append, IO::Purpose::Transcript, options.autosave_librarystate ? StreamRock::TranscriptStream : StreamRock::None);
            } catch (const IO::OpenError &) {
                store_word(0x10, word(0x10) & ~FLAGS2_TRANSCRIPT);
                streams.reset(OSTREAM_TRANSCRIPT);
                warning("unable to open the transcript");
            }
        }
    } else if (number == -2) {
        store_word(0x10, word(0x10) & ~FLAGS2_TRANSCRIPT);
        // If autosave_librarystate, we close the stream. (Keeping it open in
        // the background is unnecessary work.)
        if (options.transcript_name.has_value() && options.autosave_librarystate) {
            transio.reset();
        }
    }

    if (number == 3) {
        stream_tables.push(table, formatted);
    } else if (number == -3) {
        stream_tables.pop();
    }

    if (number == 4) {
        if (!scriptio.has_value()) {
            try {
                scriptio.emplace(options.record_name, IO::Mode::WriteOnly, IO::Purpose::Input);
            } catch (const IO::OpenError &) {
                streams.reset(OSTREAM_RECORD);
                warning("unable to open the script");
            }
        }
    }
    // XXX V6 has even more handling

    return number < 0 || streams.test(number);
}

bool output_stream(int16_t number, uint16_t table)
{
    return output_stream(number, table, false);
}

void zoutput_stream()
{
    output_stream(as_signed(zargs[0]), zargs[1], znargs > 2);
}

// See §10.
// This returns true if the stream was successfully selected.
bool input_stream(int which)
{
    istream = which;

    if (istream == ISTREAM_KEYBOARD) {
        istreamio.reset();
    } else if (istream == ISTREAM_FILE) {
        if (!istreamio.has_value()) {
            try {
                istreamio.emplace(options.replay_name, IO::Mode::ReadOnly, IO::Purpose::Input);
            } catch (const IO::OpenError &) {
                warning("unable to open the command script");
                istream = ISTREAM_KEYBOARD;
            }
        }
    } else {
        ZASSERT(false, "invalid input stream: %d", istream);
    }

    return istream == which;
}

void zinput_stream()
{
    input_stream(zargs[0]);
}

// This does not even pretend to understand V6 windows.
static void set_current_window(Window *window)
{
    curwin = window;

#ifdef ZTERP_GLK
    if (curwin == upperwin && upperwin->id != nullptr) {
        upperwin->x = upperwin->y = 0;
        // V3 (§8.6.1) and V4/V5 (§8.7.2) require the cursor to be moved
        // to the origin when the upper window is selected, but V6 doesn’t.
        if (zversion != 6) {
            glk_window_move_cursor(upperwin->id, 0, 0);
        }
    }

#ifdef ZTERP_GLK_OVERLAY
    // While the InvisiClues grid is up, everything the game aims at
    // window 0 is aimed at it instead.
    if (hint_output_window() != nullptr) {
        glk_set_window(hint_output_window());
        set_current_style();
        return;
    }

    // Show the encyclopedia overlay while window 3 is selected.
    // PICTURED-ENTRY switches back to window 0 after the keypress.
    if (hack == Hack::ZorkZero) {
        if (curwin == zorkzero_encwindow) {
            zorkzero_place_enc_window(true);
            if (zorkzero_enc_overlay.active()) {
                return;
            }

            // If the picture is not ready, send the article to the main
            // window so it remains readable.
            curwin->id = mainwin->id;
        }

        if (zorkzero_enc_overlay.active()) {
            zorkzero_place_enc_window(false);
        }
    }

    // Show the menu overlay while Shogun’s menu window is selected.
    // MENU-SELECT does not resize the window when it finishes, so a
    // switch to another window is the signal to hide it.
    if (hack == Hack::Shogun && shogunmenuwin->id != nullptr) {
        shogun_place_menu_overlay(curwin == shogunmenuwin);
    }
#endif

    glk_set_window(curwin->id);
#endif

    set_current_style();
}

// Find and validate a window. If window is -3 and the story is V6,
// return the current window.
static Window *find_window(uint16_t window)
{
    int16_t w = as_signed(window);

    ZASSERT(zversion == 6 ? w == -3 || (w >= 0 && w < 8) : w == 0 || w == 1, "invalid window selected: %d", w);

    if (w == -3) {
        return curwin;
    }

    return &windows[w];
}

#ifdef ZTERP_GLK
#ifdef ZTERP_GLK_OVERLAY
// While the upper window is floating, its height comes from the overlay
// rectangle, not from anything the tree was asked for, so take the
// tracked height from the window itself.
static void sync_upper_window_height()
{
    glui32 actual_height;

    glk_window_get_size(upperwin->id, nullptr, &actual_height);
    upper_window_height = actual_height;
}
#endif

static void perform_upper_window_resize(glui32 new_height)
{
    glui32 actual_height;

    auto location = is_game(Game::Journey) ?
        winmethod_Below :
        winmethod_Above;

    // A floating upper window takes no room in the layout, so the split
    // can be left to say whatever the game asked for even while the
    // window is elsewhere: Gargoyle hands the whole box to the sibling
    // regardless, and gives the split back when the overlay is cleared.
    glk_window_set_arrangement(glk_window_get_parent(upperwin->id), location | winmethod_Fixed | winmethod_NoBorder, new_height, upperwin->id);
    upper_window_height = new_height;

    // Glk might resize the window to a smaller height than was requested,
    // so track the actual height, not the requested height.
    glk_window_get_size(upperwin->id, nullptr, &actual_height);
    if (actual_height != upper_window_height) {
#ifdef ZTERP_GLK_OVERLAY
        // While overlaid the mismatch is expected and not a failure:
        // the overlay rectangle, not the request, decides the height.
        if (!upper_window_overlay.active())
#endif
        {
            // This message probably won’t be seen in a window since the
            // upper window is likely covering everything, but try anyway.
            show_message("Unable to fulfill window size request: wanted %lu, got %lu", static_cast<unsigned long>(new_height), static_cast<unsigned long>(actual_height));
        }
        upper_window_height = actual_height;
    }
}

// When resizing the upper window, the screen’s contents should not
// change (§8.6.1); however, the way windows are handled with Glk makes
// this slightly impossible. When an Inform game tries to display
// something with “box”, it expands the upper window, displays the quote
// box, and immediately shrinks the window down again. This is a
// problem under Glk because the window immediately disappears. Other
// games, such as Bureaucracy, expect the upper window to shrink as soon
// as it has been requested. Thus the following system is used:
//
// If a request is made to shrink the upper window, it is granted
// immediately if there has been user input since the last window resize
// request. If there has not been user input, the request is delayed
// until after the next user input is read.
static long delayed_window_shrink = -1;
static bool saw_input;

static void update_delayed()
{
    if (delayed_window_shrink != -1 && upperwin->id != nullptr) {
        perform_upper_window_resize(delayed_window_shrink);
        delayed_window_shrink = -1;
    }
}

static void clear_window(Window *window)
{
    if (window->id == nullptr) {
        return;
    }

    glk_window_clear(window->id);

#ifdef ZTERP_GLK_OVERLAY
    // Window 0 is the hint grid while that is up.
    if (window == mainwin && hint_grid_window() != nullptr) {
        glk_window_clear(hint_grid_window());
    }
#endif

    window->x = window->y = 0;
}
#endif

static void resize_upper_window(uint32_t nlines, bool from_game)
{
#ifdef ZTERP_GLK
    if (upperwin->id == nullptr) {
        return;
    }

    glui32 previous_height = upper_window_height;

    if (from_game) {
        delayed_window_shrink = nlines;
        if (upper_window_height <= nlines || saw_input) {
            update_delayed();
        }
        saw_input = false;

        // §8.6.1.1.2
        if (zversion == 3) {
            clear_window(upperwin);
        }
    } else {
        perform_upper_window_resize(nlines);
    }

    // If the window is being created, or if it’s shrinking and the cursor
    // is no longer inside the window, move the cursor to the origin.
    if (previous_height == 0 || upperwin->y >= nlines) {
        upperwin->x = upperwin->y = 0;
        if (nlines > 0) {
            glk_window_move_cursor(upperwin->id, 0, 0);
        }
    }

    // When a textgrid (the upper window) in Gargoyle is rearranged, it
    // forgets about reverse video settings, so reapply any styles.
    set_current_style();
#endif
}

void close_upper_window()
{
    // The upper window is never destroyed; rather, when it’s closed, it
    // shrinks to zero height.
    resize_upper_window(0, true);

#ifdef ZTERP_GLK
    delayed_window_shrink = -1;
    saw_input = false;
#endif

    set_current_window(mainwin);
}

std::pair<unsigned int, unsigned int> get_screen_size()
{
#ifdef ZTERP_GLK
    glui32 w, h;
    unsigned int width, height;

    // The main window can be proportional, and if so, its width is not
    // generally useful because games tend to care about width with a
    // fixed font. If a status window is available, or if an upper window
    // is available, use that to calculate the width, because these
    // windows will have a fixed-width font. The height is the combined
    // height of all windows.
    glk_window_get_size(mainwin->id, &w, &h);
    height = h;
    if (statuswin.id != nullptr) {
        glk_window_get_size(statuswin.id, &w, &h);
        height += h;
    }
    if (upperwin->id != nullptr) {
        glk_window_get_size(upperwin->id, &w, &h);
        height += h;
    }
    width = w;
#else
    auto [width, height] = zterp_os_get_screen_size();
#endif

    // XGlk does not report the size of textbuffer windows, and
    // zterp_os_get_screen_size() may not be able to get the screen
    // size, so use reasonable defaults in those cases.
    if (width == 0) {
        width = 80;
    }
    if (height == 0) {
        height = 24;
    }

    // Terrible hack: Because V6 is not properly supported, the window to
    // which Journey writes its story is completely covered up by window
    // 1. For the same reason, only the bottom 6 lines of window 1 are
    // actually useful, even though the game expands it to cover the whole
    // screen. By pretending that the screen height is only 6, the main
    // window, where text is actually sent, becomes visible.
    if (is_game(Game::Journey) && height > 6) {
        height = 6;
    }

    return {width, height};
}

#ifdef ZTERP_GLK
#ifdef GLK_MODULE_LINE_TERMINATORS
static std::vector<glui32> term_keys;
#endif
static bool term_mouse = false;

static void term_keys_reset()
{
#ifdef GLK_MODULE_LINE_TERMINATORS
    term_keys.clear();
#endif
    term_mouse = false;
}

static void insert_key(uint32_t key)
{
#ifdef GLK_MODULE_LINE_TERMINATORS
    term_keys.push_back(key);
#endif
}

static void term_keys_add(uint8_t key)
{
    switch (key) {
    case ZSCII_UP:    insert_key(keycode_Up); break;
    case ZSCII_DOWN:  insert_key(keycode_Down); break;
    case ZSCII_LEFT:  insert_key(keycode_Left); break;
    case ZSCII_RIGHT: insert_key(keycode_Right); break;
    case ZSCII_F1:    insert_key(keycode_Func1); break;
    case ZSCII_F2:    insert_key(keycode_Func2); break;
    case ZSCII_F3:    insert_key(keycode_Func3); break;
    case ZSCII_F4:    insert_key(keycode_Func4); break;
    case ZSCII_F5:    insert_key(keycode_Func5); break;
    case ZSCII_F6:    insert_key(keycode_Func6); break;
    case ZSCII_F7:    insert_key(keycode_Func7); break;
    case ZSCII_F8:    insert_key(keycode_Func8); break;
    case ZSCII_F9:    insert_key(keycode_Func9); break;
    case ZSCII_F10:   insert_key(keycode_Func10); break;
    case ZSCII_F11:   insert_key(keycode_Func11); break;
    case ZSCII_F12:   insert_key(keycode_Func12); break;

    // Keypad 0–9 should be here, but Glk doesn’t support that.
    case ZSCII_KEY0: case ZSCII_KEY1: case ZSCII_KEY2: case ZSCII_KEY3:
    case ZSCII_KEY4: case ZSCII_KEY5: case ZSCII_KEY6: case ZSCII_KEY7:
    case ZSCII_KEY8: case ZSCII_KEY9:
        break;

    case ZSCII_CLICK_SINGLE:
        term_mouse = true;
        break;

    case ZSCII_CLICK_MENU: case ZSCII_CLICK_DOUBLE:
        break;

    case 255:
        for (int i = 129; i <= 154; i++) {
            term_keys_add(i);
        }
        for (int i = 252; i <= 254; i++) {
            term_keys_add(i);
        }
        break;

    default:
        break;
    }
}

// Look in the terminating characters table (if any) and reset the
// current state of terminating characters appropriately.
static void check_terminators()
{
    if (header.terminating_characters_table != 0) {
        term_keys_reset();

        for (uint32_t addr = header.terminating_characters_table; user_byte(addr) != 0; addr++) {
            term_keys_add(user_byte(addr));
        }
    }
}
#endif

// Decode and print a zcode string at address “addr”. This can be
// called recursively thanks to abbreviations; the initial call should
// have “in_abbr” set to false.
// Each time a character is decoded, it is passed to the function
// “outc”.
static int print_zcode(uint32_t addr, bool in_abbr, void (*outc)(uint8_t))
{
    enum class TenBit { None, Start, Half } tenbit = TenBit::None;
    int abbrev = 0, shift = 0;
    int c, lastc = 0; // initialize to appease g++
    uint16_t w;
    uint32_t counter = addr;
    int current_alphabet = 0;

    do {
        ZASSERT(counter < memory_size - 1, "string (@0x%lx) runs beyond the end of memory", static_cast<unsigned long>(addr));

        w = word(counter);

        for (int i : {10, 5, 0}) {
            c = (w >> i) & 0x1f;

            if (tenbit == TenBit::Start) {
                lastc = c;
                tenbit = TenBit::Half;
            } else if (tenbit == TenBit::Half) {
                outc((lastc << 5) | c);
                tenbit = TenBit::None;
            } else if (abbrev != 0) {
                uint32_t new_addr = user_word(header.abbr + 64 * (abbrev - 1) + 2 * c);

                // new_addr is a word address, so multiply by 2
                print_zcode(new_addr * 2, true, outc);

                abbrev = 0;
            } else {
                switch (c) {
                case 0:
                    outc(ZSCII_SPACE);
                    shift = 0;
                    break;
                case 1:
                    if (zversion == 1) {
                        outc(ZSCII_NEWLINE);
                        shift = 0;
                        break;
                    }
                    [[fallthrough]];
                case 2: case 3:
                    if (zversion >= 3 || (zversion == 2 && c == 1)) {
                        ZASSERT(!in_abbr, "abbreviation being used recursively");
                        abbrev = c;
                        shift = 0;
                    } else {
                        shift = c - 1;
                    }
                    break;
                case 4: case 5:
                    if (zversion <= 2) {
                        current_alphabet = (current_alphabet + (c - 3)) % 3;
                        shift = 0;
                    } else {
                        shift = c - 3;
                    }
                    break;
                case 6:
                    if (zversion <= 2) {
                        shift = (current_alphabet + shift) % 3;
                    }

                    if (shift == 2) {
                        shift = 0;
                        tenbit = TenBit::Start;
                        break;
                    }
                    [[fallthrough]];
                default:
                    if (zversion <= 2 && c != 6) {
                        shift = (current_alphabet + shift) % 3;
                    }

                    outc(atable[(26 * shift) + (c - 6)]);
                    shift = 0;
                    break;
                }
            }
        }

        counter += 2;
    } while ((w & 0x8000) == 0);

    return counter - addr;
}

// Prints the string at addr “addr”.
//
// Returns the number of bytes the string took up. “outc” is passed as
// the character-print function to print_zcode(); if it is null,
// put_char is used.
int print_handler(uint32_t addr, void (*outc)(uint8_t))
{
    return print_zcode(addr, false, outc != nullptr ? outc : put_char);
}

void zprint()
{
    pc += print_handler(pc, nullptr);
}

void znew_line()
{
    put_char(ZSCII_NEWLINE);
}

void zprint_ret()
{
    zprint();
    znew_line();
    zrtrue();
}

#ifdef ZTERP_GLK_GRAPHICS
static bool image_or_rect_size(glui32 pic, glui32 &width, glui32 &height)
{
    glui32 w, h;
    if (glk_image_get_info(pic, &w, &h)) {
        width = w;
        height = h;
        return true;
    } else {
        auto *map = giblorb_get_resource_map();
        if (map == nullptr) {
            return false;
        }
        giblorb_result_t res;
        if (giblorb_load_resource(map, giblorb_method_Memory, &res, giblorb_ID_Pict, pic) == giblorb_err_None) {
            if (res.length == 8 && res.chunktype == blorbid("Rect")) {
                auto *ptr = static_cast<unsigned char *>(res.data.ptr);
                width = be32(ptr + 0);
                height = be32(ptr + 4);
                return true;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
}

static bool zorkzero_has_border()
{
    // Whether in bordered or borderless mode (controlled by the MODE
    // game command) is tracked via a global variable, which differs for
    // each version of Zork Zero.
    static const std::unordered_map<std::string, uint8_t> gvars = {
        {"296-881019", 0x9f},
        {"366-890323", 0x7f},
        {"383-890602", 0x82},
        {"393-890714", 0x83},
    };

    try {
        return variable(gvars.at(get_story_id()) + 0x10) != 0;
    } catch (const std::out_of_range &) {
        return false;
    }
}

#ifdef ZTERP_GLK_OVERLAY
// A text grid’s rows fall on whole cells; the rows the Fanucci board is
// written at do not. The score line runs to the pixel above the cards
// (30 plus a row of 8, against a card top of 38) and the labels start
// at the pixel below (103, against a card bottom of 101), so rounding
// each row on its own can put the scores over the cards while the
// labels sit clear of them.
//
// Moving the top of the grid by part of a cell shifts every row. Try
// each possible offset within a cell and choose the one with the
// smallest maximum error.
static double zorkzero_fanucci_lattice_offset(double cellh)
{
    static const std::array<glui32, 3> anchors = {
        FANUCCI_SCORE_LOC, FANUCCI_DISCARD_LOC, FANUCCI_MENU_LOC,
    };

    std::vector<double> rows;

    for (auto anchor : anchors) {
        glui32 x, y;
        if (image_or_rect_size(anchor, x, y)) {
            rows.push_back(graphics_window.to_pixel_y(y) - graphics_window.to_pixel_y(0));
        }
    }

    double best_offset = 0.0;
    std::optional<double> best_error;

    for (glui32 offset = 0; offset < cellh; offset++) {
        double worst = 0.0;

        for (auto row : rows) {
            double cell = std::max(0.0, std::round((row - offset) / cellh));
            worst = std::max(worst, std::abs(row - (offset + (cell * cellh))));
        }

        if (!best_error.has_value() || worst < *best_error) {
            best_error = worst;
            best_offset = offset;
        }
    }

    return best_offset;
}

static void hide_upper_window_overlay()
{
    if (upper_window_overlay.hide(upperwin->id)) {
        sync_upper_window_height();
    }
}

// Use a row count to avoid Gargoyle converting a rounded pixel height
// back to cells and losing a row.
static void show_upper_window_overlay(double left, double top, double right,
                                      glui32 rows, const Overlay::Colors &colors)
{
    double cellw, cellh;
    if (!cell_size(cellw, cellh) || right <= left) {
        return;
    }

    glsi32 gx, gy;
    garglk_window_get_origin_pixels(graphics_window.id(), &gx, &gy);

    glui32 gheight;
    glk_window_get_size(graphics_window.id(), nullptr, &gheight);

    // User-selected rows can be taller than the artwork’s 8-pixel rows.
    // Move the block up to keep its bottom within the picture. If it is
    // still too tall, preserve the full grid rather than clipping it.
    double height = rows * cellh;
    if (top + height > gy + gheight) {
        top = std::max<double>(gy, gy + gheight - height);
    }

    glui32 columns;
    garglk_cells_in_pixels(static_cast<glui32>(std::round(right - left)), 0, &columns, nullptr);

    if (columns == 0 || rows == 0) {
        return;
    }

    upper_window_overlay.show_grid(upperwin->id, left, top, columns, rows, colors);

    sync_upper_window_height();
}

// Zork Zero and Shogun share the three-row InvisiClues header, with
// different placement and palettes. Arthur has no strip.
static void place_hint_header_overlay()
{
    glsi32 gx, gy;
    garglk_window_get_origin_pixels(graphics_window.id(), &gx, &gy);

    double left, right;
    glui32 bg;

    if (hack == Hack::ZorkZero) {
        glui32 gwidth;
        glk_window_get_size(graphics_window.id(), &gwidth, nullptr);

        left = gx;
        right = gx + gwidth;
        bg = 0x806060;
    } else {
        // P-HINT-LOC is the horizontal inset between the side panels.
        glui32 inset, inset_height;
        if (!image_or_rect_size(SHOGUN_HINT_LOC, inset, inset_height)) {
            return;
        }

        left = gx + graphics_window.to_pixel_x(inset);
        right = gx + graphics_window.to_pixel_x(SHOGUN_SCREEN_WIDTH - inset);
        bg = 0x886666;
    }

    show_upper_window_overlay(left, gy, right, 3, {0xffffff, bg});
}

// Replace window 0 with an addressable grid while the shared hint
// library has wrapping disabled.
static void place_hint_grid(bool show)
{
    if (hack != Hack::ZorkZero && hack != Hack::Arthur && hack != Hack::Shogun) {
        return;
    }

    if (!show) {
        if (hint_overlay.active()) {
            if (hintwin != nullptr) {
                glk_window_clear(hintwin);
            }
            hint_overlay.hide(hintwin);
            restore_current_window();
        }
        return;
    }

    if (hintwin == nullptr) {
        hintwin = open_floating_window(wintype_TextGrid, WindowRock::HintWin);
        if (hintwin == nullptr) {
            return;
        }
    }

    glsi32 mx, my;
    garglk_window_get_origin_pixels(mainwin->id, &mx, &my);

    glui32 mwidth, mheight;
    garglk_window_get_size_pixels(mainwin->id, &mwidth, &mheight);

    if (mwidth == 0 || mheight == 0) {
        return;
    }

    hint_overlay.show(hintwin, mx, my, mx + mwidth, my + mheight);

    if (curwin == mainwin) {
        glk_set_window(hintwin);
        set_current_style();
    }
}

// Float window 0 into the text area described by SPLIT-BY-PICTURE and
// ADJUST-TEXT-WINDOW.
static void zorkzero_place_sheet_text(bool show)
{
    if (hack != Hack::ZorkZero) {
        return;
    }

    winid_t gwin = graphics_window.id();

    glui32 split_x, split_y, bottom_x, bottom_y;
    bool have_layout = gwin != nullptr &&
                       zorkzero_sheet.has_value() &&
                       image_or_rect_size(zorkzero_sheet->split, split_x, split_y) &&
                       image_or_rect_size(zorkzero_sheet->bottom, bottom_x, bottom_y);

    if (!show || !have_layout || graphics_window.ratio() <= 0) {
        if (zorkzero_sheet_text_overlay.hide(mainwin->id)) {
            // The window is opaque again; restore its background color.
            set_current_style();
        }
        return;
    }

    glsi32 gx, gy;
    garglk_window_get_origin_pixels(gwin, &gx, &gy);

    double left = gx + graphics_window.to_pixel_x(split_x);
    double right = gx + graphics_window.to_pixel_x(ZORKZERO_SCREEN_WIDTH - split_x);
    double top = gy + graphics_window.to_pixel_y(split_y);
    double bottom = gy + graphics_window.to_pixel_y(ZORKZERO_SCREEN_HEIGHT - bottom_y);

    if (right <= left || bottom <= top) {
        return;
    }

    bool was_floating = zorkzero_sheet_text_overlay.active();

    zorkzero_sheet_text_overlay.show(mainwin->id, left, top, right, bottom);

    if (!was_floating) {
        // Do not expose old text through the sheet before the game clears it.
        glk_window_clear(mainwin->id);
    }

    set_current_style();
}

// Place Zork Zero’s upper window in its banner, hint strip, or Fanucci board.
// The banner bounds come from the HERE-LOC and REGION-LOC Blorb rectangles.
static void zorkzero_place_upper_window_overlay()
{
    if (hack != Hack::ZorkZero || upperwin->id == nullptr) {
        return;
    }

    winid_t gwin = graphics_window.id();
    auto type = graphics_window.type();

    bool banner = type == GraphicsWindow::Type::ZorkZeroBorder;
    bool hints = type == GraphicsWindow::Type::HintBorder;
    bool sheet = type == GraphicsWindow::Type::ZorkZeroSheet;

    bool board = sheet && zorkzero_sheet.has_value() && zorkzero_sheet->upper_window;

    // Autorestore leaves the ratio zero until the graphics window is sized.
    bool sized = gwin != nullptr && graphics_window.ratio() > 0;

    // A deferred shrink must not offset the sheet before its overlay is set.
    if (sheet && sized && !upper_window_overlay.active()) {
        perform_upper_window_resize(0);
    }

    place_hint_grid(hint_overlay.active());
    zorkzero_place_sheet_text(sheet && sized);

    if (!sized ||
        (!banner && !hints && !board) ||
        (banner && !zorkzero_has_border()))
    {
        hide_upper_window_overlay();
        return;
    }

    if (hints) {
        place_hint_header_overlay();
        return;
    }

    double cellw, cellh;
    if (!cell_size(cellw, cellh)) {
        return;
    }

    glsi32 gx, gy;
    garglk_window_get_origin_pixels(gwin, &gx, &gy);

    double left, right, top;
    glui32 rows;
    glui32 fg, bg;

    if (banner) {
        glui32 here_x, here_y, region_x, region_y;
        if (!image_or_rect_size(382, here_x, here_y) ||
            !image_or_rect_size(383, region_x, region_y))
        {
            return;
        }

        left = gx + graphics_window.to_pixel_x(here_x);
        right = gx + graphics_window.to_pixel_x(region_x);

        // Both rectangles sit on the same line, so either y will do.
        top = gy + graphics_window.to_pixel_y(here_y);

        // The room name and region on one row, Moves and Score on the next.
        rows = 2;

        // Match the original black text in the light banner gaps.
        fg = 0x000000;
        bg = 0xffffff;
    } else {
        glui32 split_x, split_y;
        if (!image_or_rect_size(zorkzero_sheet->split, split_x, split_y)) {
            return;
        }

        // Shift the grid’s cell lattice to best match the artwork.
        zorkzero_fanucci_grid_offset = zorkzero_fanucci_lattice_offset(cellh);

        left = gx + graphics_window.to_pixel_x(0);
        right = gx + graphics_window.to_pixel_x(ZORKZERO_SCREEN_WIDTH);
        top = gy + graphics_window.to_pixel_y(0) + zorkzero_fanucci_grid_offset;

        double bottom = gy + graphics_window.to_pixel_y(split_y);
        garglk_cells_in_pixels(0, bottom > top ? static_cast<glui32>(std::round(bottom - top)) : 0,
                               nullptr, &rows);

        // The transparent board uses the user’s colors.
        fg = zcolor_Default;
        bg = zcolor_Default;
    }

    show_upper_window_overlay(left, top, right, rows, {fg, bg});
}

// True while window 2 belongs to the graphics panel: a picture mode is
// up and arthurwin has no rows of its own. The float below gives it
// rows, so it counts too.
static bool arthur_window2_is_graphics()
{
    if (hack != Hack::Arthur || arthurwin->id == nullptr || graphics_window.id() == nullptr) {
        return false;
    }

    if (arthur_map_text_overlay.active()) {
        return true;
    }

    glui32 height;
    glk_window_get_size(arthurwin->id, nullptr, &height);

    return height == 0;
}

// RT-REDRAW-MAP can write a message to window 2 while its rows belong to the
// graphics panel. Float the otherwise zero-height text window over that panel.
static void place_arthur_map_text(bool show)
{
    if (hack != Hack::Arthur || arthurwin->id == nullptr) {
        return;
    }

    winid_t gwin = graphics_window.id();

    if (!show || gwin == nullptr || graphics_window.ratio() <= 0) {
        if (arthur_map_text_overlay.hide(arthurwin->id)) {
            glk_window_clear(arthurwin->id);
        }
        return;
    }

    glsi32 gx, gy;
    garglk_window_get_origin_pixels(gwin, &gx, &gy);

    glui32 gwidth, gheight;
    glk_window_get_size(gwin, &gwidth, &gheight);

    glui32 columns, rows;
    garglk_cells_in_pixels(gwidth, gheight, &columns, &rows);

    if (columns == 0 || rows == 0) {
        return;
    }

    arthur_map_text_overlay.show_grid(arthurwin->id, gx, gy, columns, rows);
}

// Shogun otherwise keeps window 1 in the layout as its status line.
static void shogun_place_hint_header_overlay()
{
    if (hack != Hack::Shogun || upperwin->id == nullptr) {
        return;
    }

    if (graphics_window.type() != GraphicsWindow::Type::HintBorder ||
        graphics_window.id() == nullptr ||
        graphics_window.ratio() <= 0)
    {
        hide_upper_window_overlay();
        return;
    }

    place_hint_header_overlay();
}
#endif

bool GraphicsWindow::create()
{
    if (m_id == nullptr) {
        m_id = glk_window_open(mainwin->id, winmethod_Above | winmethod_Fixed | winmethod_NoBorder, 0, wintype_Graphics, static_cast<glui32>(m_rocks.main));
    }

    return m_id != nullptr;
}

void GraphicsWindow::forget()
{
    m_id = nullptr;
    m_left_border = nullptr;
    m_right_border = nullptr;
    m_type = Type::None;
    m_ratio = 0.0;
    m_x_offset = 0.0;
    m_base_size = {0, 0};
}

void GraphicsWindow::recover(winid_t win, WindowRock rock)
{
    if (rock == m_rocks.main) {
        m_id = win;
    } else if (rock == m_rocks.left_border) {
        m_left_border = win;
    } else if (rock == m_rocks.right_border) {
        m_right_border = win;
    }
}

#ifdef GLK_MODULE_GARGLKTEXT
// Graphics windows are implemented separately from the Z-machine window
// array, so they don’t get informed about color changes. This function
// is used to broadcast a color change.
//
// Note that despite the fact that graphics windows don’t ride alongside
// Z-machine windows, the Z-machine window array _does_ track the
// colors, and we know which Z-machine windows map to which graphics
// windows for various games.
static void update_graphics_bg()
{
    switch (hack) {
    case Hack::Arthur:
        graphics_window.set_bg_color(windows[2].bg_color);
        break;
    case Hack::ZorkZero: case Hack::Shogun:
        graphics_window.set_bg_color(windows[7].bg_color);
        break;
    case Hack::Journey:
        if (journey_window != nullptr) {
            set_window_bg(journey_window, windows[3].bg_color);
        }
        break;
    case Hack::None: case Hack::MysteriousAdventures:
        break;
    }
}
#endif

bool GraphicsWindow::resize(Type type)
{
    if (m_id == nullptr) {
        return false;
    }

    if (type == m_type) {
        return true;
    }

    glk_window_clear(m_id);

    // The images for Zork Zero games (Tower of Bozbar, Peggleboz,
    // Snarfem, and Double Fanucci) are 320x200, but the bottom ≅83
    // pixels are a box intended to hold all the text on the screen; and
    // since Glk doesn’t support text in graphics windows, the text will
    // go below the entire image. That’s a complete waste of space and
    // requires a tall window to fit both the original 320x200 (or
    // 320x240 in aspect-correct mode) image, as well as all the text.
    // So instead, pretend the image is 320x117, preventing the bottom
    // 83 pixels from appearing. Snarfem requires a bit more space, so
    // it is given its own window type. The Snarfem image itself is fine
    // at 117, but the numbered boxes are then drawn below that, so the
    // graphics window needs to be large enough to accommodate them.
    //
    // Those cuts are only made where overlays aren’t available. Where
    // they are, the text is floated into the box the sheet keeps for it
    // and Type::ZorkZeroSheet stands in for all four at their real
    // size. See zorkzero_place_sheet_text().
    static const std::unordered_map<GraphicsWindow::Type, WindowLayout> window_layouts = {
        {GraphicsWindow::Type::ArthurIntro, {{292, 196}, {}}},
        {GraphicsWindow::Type::ArthurBanner, {{314, 84}, BorderWidths{2, 2}}},
        {GraphicsWindow::Type::ArthurMap, {{320, 96}, {}}},
        {GraphicsWindow::Type::ArthurDemon, {{254, 164}, {}}},
        {GraphicsWindow::Type::ZorkZeroBorder, {{320, 39}, BorderWidths{43, 42}}},
        {GraphicsWindow::Type::ZorkZero320, {{320, 200}, {}}},
        {GraphicsWindow::Type::ZorkZeroGame, {{320, 117}, {}}},
        {GraphicsWindow::Type::ZorkZeroSheet, {{ZORKZERO_SCREEN_WIDTH, ZORKZERO_SCREEN_HEIGHT}, {}}},
        {GraphicsWindow::Type::ZorkZeroSnarfem, {{320, 132}, {}}},
        {GraphicsWindow::Type::ShogunTitle, {{320, 200}, {}}},
        {GraphicsWindow::Type::ShogunNormal, {{320, 0}, BorderWidths{23, 23}}},
        {GraphicsWindow::Type::ShogunMaze, {{274, 140}, BorderWidths{23, 23}}},
        {GraphicsWindow::Type::HintBorder, {{320, 29}, BorderWidths{30, 30}}},
        {GraphicsWindow::Type::Mysterious, {{512, 208}, {}}},
        {GraphicsWindow::Type::MysteriousSeparator, {{512, 16}, {}}},
    };

    auto layout = window_layouts.find(type);
    if (layout == window_layouts.end() || !full_window_width.has_value()) {
        return false;
    }

    const auto &size = layout->second.size;

    m_ratio = std::min(*full_window_width / size.width, options.v6_hack_max_scale);

    // Scaling by width alone can make a full-screen picture taller than
    // the display, causing Glk to crop it. Limit the scale by the
    // available height as well. Horizontal centering already handles
    // pictures narrower than the window, and the window is resized to
    // the scaled picture height below.
    if (full_window_height.has_value()) {
        m_ratio = std::min(m_ratio, *full_window_height / (size.height * aspect_scale()));
    }

    m_x_offset = (*full_window_width - (size.width * m_ratio)) / 2;

    double window_height = size.height * m_ratio * aspect_scale();

    // A sheet occupies the entire display. Give its graphics window the
    // full display height so that floating the text windows does not
    // leave unused space in the layout. The picture stays at the top,
    // as every other full-screen picture does.
    if (type == Type::ZorkZeroSheet && full_window_height.has_value()) {
        window_height = *full_window_height;
    }

    m_base_size = size;

    glk_window_set_arrangement(glk_window_get_parent(m_id), winmethod_Above | winmethod_Fixed | winmethod_NoBorder, std::round(window_height), m_id);

    if (m_left_border != nullptr) {
        glk_window_close(m_left_border, nullptr);
        m_left_border = nullptr;
    }

    if (m_right_border != nullptr) {
        glk_window_close(m_right_border, nullptr);
        m_right_border = nullptr;
    }

    // Create border windows only for layouts and instances that define
    // them; see Rocks.
    if (layout->second.borders.has_value() &&
        m_rocks.left_border.has_value() && m_rocks.right_border.has_value())
    {
        const auto &borders = *layout->second.borders;

        // The borders have to enclose both the upper window and the
        // main window. Splitting the main window’s parent would
        // usually do that, but not always: the parent is whatever
        // pair the main window currently sits in, and the error
        // window (see show_message()) splits the main window from
        // below, which leaves the upper window outside the result.
        // The upper window’s parent always contains both, so use
        // that instead, falling back when there’s no upper window.
        winid_t split = glk_window_get_parent(upperwin->id != nullptr ? upperwin->id : mainwin->id);

        m_left_border = glk_window_open(split, winmethod_Left | winmethod_Fixed | winmethod_NoBorder, borders.left * ratio(), wintype_Graphics, static_cast<glui32>(*m_rocks.left_border));
        if (m_left_border != nullptr) {
            m_right_border = glk_window_open(split, winmethod_Right | winmethod_Fixed | winmethod_NoBorder, borders.right * ratio(), wintype_Graphics, static_cast<glui32>(*m_rocks.right_border));
            if (m_right_border == nullptr) {
                glk_window_close(m_left_border, nullptr);
                m_left_border = nullptr;
            }
        }
    }

    // Recorded before the upper window is measured below, because in Zork
    // Zero the overlay is what determines how wide that window is, and
    // zorkzero_place_upper_window_overlay() decides what to do from the type.
    m_type = type;

#ifdef ZTERP_GLK_OVERLAY
    zorkzero_place_upper_window_overlay();
    shogun_place_hint_header_overlay();
    place_arthur_map_text(false);
#endif

    if (upperwin->id != nullptr) {
        glui32 w;
        glk_window_get_size(upperwin->id, &w, nullptr);
        upper_window_width = w;
    }

#ifdef GLK_MODULE_GARGLKTEXT
    update_graphics_bg();
#endif

    return true;
}

void GraphicsWindow::destroy()
{
    if (m_id != nullptr) {
        m_type = Type::None;
        glk_window_clear(m_id);
        glk_window_set_arrangement(glk_window_get_parent(m_id), winmethod_Above | winmethod_Fixed | winmethod_NoBorder, 0, m_id);
    }

    if (m_left_border != nullptr) {
        glk_window_close(m_left_border, nullptr);
        m_left_border = nullptr;
    }

    if (m_right_border != nullptr) {
        glk_window_close(m_right_border, nullptr);
        m_right_border = nullptr;
    }

#ifdef ZTERP_GLK_OVERLAY
    // There is no longer a banner to float over; m_type is None by now,
    // so this takes the status line back out of the overlay.
    zorkzero_place_upper_window_overlay();
    shogun_place_hint_header_overlay();
    place_arthur_map_text(false);
#endif
}

void GraphicsWindow::clear()
{
    if (m_id != nullptr) {
        glk_window_clear(m_id);
    }
}

bool GraphicsWindow::is_zorkzero_fullscreen() const
{
    return m_type == Type::ZorkZero320 ||
           m_type == Type::ZorkZeroGame ||
           m_type == Type::ZorkZeroSheet ||
           m_type == Type::ZorkZeroSnarfem;
}

#ifdef GLK_MODULE_GARGLKTEXT
void GraphicsWindow::set_bg_color(const Color &bg)
{
    for (auto *win : {m_id, m_left_border, m_right_border}) {
        if (win != nullptr) {
            set_window_bg(win, bg);
        }
    }
}
#endif

#ifdef ZTERP_GLK_OVERLAY
// Convert a position in the artwork’s pixels to a cell in the grid
// floating over the Fanucci board. The game reads these positions back
// with @picture_data and addresses window 1 with them in units of
// FONT-X and FONT-Y, which Bocfel reports as 1, so a pixel is a cell.
static bool zorkzero_fanucci_cell(glui32 x, glui32 y, glui32 &col, glui32 &row)
{
    double cellw, cellh;
    if (!cell_size(cellw, cellh)) {
        return false;
    }

    // to_pixel_x() includes the offset that centers the artwork in the
    // window, which the grid is placed with as well, so it cancels out.
    //
    // Round to the nearest cell, not the one the pixel falls inside:
    // the artwork was laid out for an 8-pixel font and cells are
    // taller, so truncating can put a row a full cell high, dropping
    // the play menu onto the border it should sit below.
    //
    // Rows are measured from the grid’s own top, which is offset from the
    // sheet’s by zorkzero_fanucci_lattice_offset(); a row above it means the
    // cells are too tall for the artwork to have anywhere better to put it.
    double from_top = graphics_window.to_pixel_y(y) -
                      graphics_window.to_pixel_y(0) -
                      zorkzero_fanucci_grid_offset;

    col = std::round((graphics_window.to_pixel_x(x) - graphics_window.to_pixel_x(0)) / cellw);
    row = std::max(0.0, std::round(from_top / cellh));

    return true;
}

// The width of one menu column, in cells: SETUP-FANUCCI’s MENU-SPACE,
// which it picks from the interpreter number, times FONT-X, which is 1.
static glui32 zorkzero_fanucci_menu_space()
{
    switch (options.int_number) {
    case 2: case 9: case 10: // Apple IIe, IIc, IIgs
        return 9;
    case 6: // IBM PC: 14 on EGA/CGA’s 640-wide screen, 12 on MCGA’s 320
        return word(0x22) == 640 ? 14 : 12;
    default:
        return 13;
    }
}

// True while window 1 is floating over the board, which is when the
// positions in the board’s rectangles mean anything. Fanucci is the
// only sheet that puts anything there.
static bool zorkzero_fanucci_grid_floating()
{
    return upper_window_overlay.active() &&
           graphics_window.type() == GraphicsWindow::Type::ZorkZeroSheet &&
           zorkzero_sheet.has_value() &&
           zorkzero_sheet->upper_window;
}

// Where a position on the board lands in the grid floating over it: the
// score lines, the labels for the five card positions and the gap
// between them, and the play menu. False for anything else.
static bool zorkzero_fanucci_position(glui32 pic, glui32 &row, glui32 &col)
{
    if (pic != FANUCCI_MENU_LOC && pic != FANUCCI_SCORE_LOC &&
        pic != FANUCCI_DISCARD_LOC && pic != FANUCCI_CARD_1_LOC &&
        pic != FANUCCI_CARD_SPACE)
    {
        return false;
    }

    glui32 x, y;
    if (upperwin->id == nullptr ||
        !image_or_rect_size(pic, x, y) ||
        !zorkzero_fanucci_cell(x, y, col, row))
    {
        return false;
    }

    // F-CARD-SPACE is a distance, not a position: the gap from one
    // card’s label to the next. The game multiplies it out itself, so
    // there is nothing to keep on the grid.
    if (pic == FANUCCI_CARD_SPACE) {
        return true;
    }

    glui32 cols, rows;
    glk_window_get_size(upperwin->id, &cols, &rows);
    if (cols == 0 || rows == 0) {
        return false;
    }

    if (pic == FANUCCI_MENU_LOC) {
        // The menu’s five columns are MENU-SPACE cells apart whatever
        // the scale, that pitch being the game’s and in cells. On a
        // narrow window the block can reach past the sheet, so slide it
        // back on: the last column is where resigning and cheating live.
        glui32 width = FANUCCI_MENU_COLUMNS * zorkzero_fanucci_menu_space();
        if (col + width > cols) {
            col = width < cols ? cols - width : 0;
        }

        if (row + FANUCCI_MENU_ROWS > rows) {
            row = FANUCCI_MENU_ROWS < rows ? rows - FANUCCI_MENU_ROWS : 0;
        }
    } else if (row >= rows) {
        // A row past the bottom is worse than a misplaced one: a
        // @set_cursor below the upper window grows it to reach (see
        // zset_cursor()), which on this screen would mean a text grid
        // over the whole display.
        row = rows - 1;
    }

    return true;
}

// Remove all overlays and discard their geometry during init_screen().
// Otherwise a restart could direct output from the new game to an old
// overlay. Each placement sets its colors again, so they need not be
// reset here.
static void reset_overlays()
{
    place_hint_grid(false);
    place_arthur_map_text(false);
    zorkzero_place_enc_window(false);
    zorkzero_place_sheet_text(false);
    shogun_place_menu_overlay(false);
    upper_window_overlay.hide(upperwin->id);

    shogun_menu_rows = shogun_menu_cols = shogun_menu_x = 0;
    zorkzero_enc_x = zorkzero_enc_y = 0;
    zorkzero_enc_w = zorkzero_enc_h = 0;

    // The graphics window is not destroyed here, so clear the sheet
    // state explicitly to prevent its overlay from being restored.
    zorkzero_sheet.reset();
}
#endif

struct JourneyStamp {
    glui32 background;
    ImageGeometry geom;
};

static void close_journey_window()
{
    if (journey_window != nullptr) {
        glk_window_close(journey_window, nullptr);
        journey_window = nullptr;
    }
}

// This duplicates some code from draw_journey(), but it may not be
// worth pulling it out since the code is just different enough to make
// that annoying.
static void draw_journey_stamp(glui32 pic, glui32 w, glui32 h, const JourneyStamp &stamp)
{
    if (journey_window == nullptr || !full_window_width.has_value()) {
        return;
    }

    glui32 width = *full_window_width * 0.375;

    glui32 gwin_width, gwin_height;
    glk_window_get_size(journey_window, &gwin_width, &gwin_height);

    double multiplier = width / 120.0;

    glui32 base_background_width, base_background_height;
    glk_image_get_info(stamp.background, &base_background_width, &base_background_height);

    double background_width = std::round(base_background_width * multiplier);
    double background_height = std::round(base_background_height * multiplier * aspect_scale());

    double x = (stamp.geom.x * multiplier) + ((gwin_width - background_width) / 2.0);
    double y = (stamp.geom.y * multiplier * aspect_scale()) + ((gwin_height - background_height) / 2.0);

    draw_image(journey_window, pic, std::round(x), std::round(y), std::round(w * multiplier), std::round(h * multiplier * aspect_scale()));
}

static bool draw_journey_background(glui32 pic, glui32 w, glui32 h)
{
    close_journey_window();

    if (!full_window_width.has_value()) {
        return false;
    }
    glui32 width = *full_window_width * 0.375;

    // Try to somewhat match the bars in the menu area.
    int border_width = options.disable_graphics_font ? 10 : 1;

    journey_window = glk_window_open(mainwin->id, winmethod_Left | winmethod_Fixed | winmethod_NoBorder, width + border_width, wintype_Graphics, static_cast<glui32>(WindowRock::JourneyWin));
    if (journey_window == nullptr) {
        return false;
    }

#ifdef GLK_MODULE_GARGLKTEXT
    update_graphics_bg();
    glk_window_clear(journey_window);
#endif

    glui32 gwin_width, gwin_height;
    glk_window_get_size(journey_window, &gwin_width, &gwin_height);

    // The widest image is 119 pixels so scale everything based on that.
    double multiplier = width / 120.0;

    double image_width = std::round(w * multiplier);
    double image_height = std::round(h * multiplier * aspect_scale());

    double x = std::round((gwin_width - image_width) / 2.0);
    double y = std::round((gwin_height - image_height) / 2.0);

    // Picture 59 is a black room (G-BLACK), which is used by
    // CAVERN-GRAPHIC in BERN-I-LAV (Gate to the Plains), when you enter
    // the gate with no light. But it’s a bit more complicated than
    // that. On all machines except DOS, a flag called
    // BLACK-PICTURE-BORDER is set, which causes the graphics window to
    // be filled in black. When a room is drawn on top of it, it doesn't
    // quite fill the window, so it gains a black border. But also when
    // this flag is set, picture 59 is not used. Rather, the game just
    // clears the window (to black), and lets that stand in for a dark
    // room. It’s bigger than the room images, but maybe that’s by
    // design: an all-encompassing darkness. Or maybe not.
    //
    // In any case, when the flag is NOT set (which is to say, on DOS),
    // the game instead just draws G-BLACK. Except that the MCGA
    // graphics file, and thus the Blorb, do _not_ contain picture 59.
    // The EGA file _does_ contain it, so it appears to be an accidental
    // omission from the MCGA version. As such, when a request for
    // picture 59 comes in, draw a black rectangle the size of a room
    // image.
    //
    // For reference, the CGA version also doesn’t contain a picture 59,
    // but the background for the CGA version is already black, so that
    // might be an intentional omission; and who knows, it’s possible
    // that the (intentional?) omission of it in the CGA led to the
    // accidental(?) omission in MCGA. The world may never know.
    //
    // Final notes on black rooms: Arthur also has a black room (167).
    // Its CGA _also_ excludes it (again, presumably because CGA’s
    // background is already black). But the MCGA version _does_ include
    // it. This bolsters the idea that the omission in CGA is
    // intentional, but in MCGA is not.
    //
    // For the case where BLACK-PICTURE-BORDER is set, the special
    // picture value of 0 is used to draw this. Ideally this would just
    // clear the screen to the background color, but at the moment only
    // Gargoyle (or at least GLK_MODULE_GARGLKTEXT) gets the color
    // treatment; so instead draw a black rectangle the entire size of
    // the window. That gets at the original intent of the game, at
    // least, without looking like some partial color implementation.
    if (pic == 0) {
        glk_window_fill_rect(journey_window, 0x000000, 0, 0, gwin_width, gwin_height);
    } else if (pic == 59) {
        glk_window_fill_rect(journey_window, 0x000000, x, y, image_width, image_height);
    } else {
        draw_image(journey_window, pic, x, y, image_width, image_height);
    }

    glui32 color;
    if (!glk_style_measure(mainwin->id, style_Normal, stylehint_TextColor, &color)) {
        color = 0xe7e8e9;
    }

    glk_window_fill_rect(journey_window, color, width, 0, border_width, gwin_height);

    return true;
}

static bool draw_journey(glui32 pic, glui32 w, glui32 h)
{
    if (hack != Hack::Journey) {
        return false;
    }

    static const std::unordered_map<glui32, JourneyStamp> journey_stamps = {
        {6,   JourneyStamp{5,   ImageGeometry(0, 32)}},
        {8,   JourneyStamp{7,   ImageGeometry(0, 4)}},
        {12,  JourneyStamp{11,  ImageGeometry(23, 74)}},
        {21,  JourneyStamp{20,  ImageGeometry(0, 21)}},
        {46,  JourneyStamp{7,   ImageGeometry(0, 0)}},
        {81,  JourneyStamp{80,  ImageGeometry(0, 12)}},
        {82,  JourneyStamp{84,  ImageGeometry(0, 0)}},
        {85,  JourneyStamp{84,  ImageGeometry(0, 0)}},
        {88,  JourneyStamp{87,  ImageGeometry(0, 0)}},
        {91,  JourneyStamp{90,  ImageGeometry(0, 39)}},
        {101, JourneyStamp{100, ImageGeometry(0, 0)}},
        {115, JourneyStamp{114, ImageGeometry(49, 0)}},
        {129, JourneyStamp{128, ImageGeometry(1, 0)}},
        {139, JourneyStamp{138, ImageGeometry(0, 0)}},
        {141, JourneyStamp{140, ImageGeometry(0, 92)}},
        {145, JourneyStamp{144, ImageGeometry(26, 80)}},
        {147, JourneyStamp{146, ImageGeometry(0, 87)}},
        {158, JourneyStamp{157, ImageGeometry(33, 74)}},
    };

    try {
        const auto &stamp = journey_stamps.at(pic);
        draw_journey_stamp(pic, w, h, stamp);
        return true;
    } catch (const std::out_of_range &) {
    }

    close_journey_window();

    // This is the title; ignore it here to allow zdraw_picture() to
    // draw it inline in the main window.
    if (pic == 160) {
        return false;
    }

    return draw_journey_background(pic, w, h);
}

static bool draw_shogun(glui32 pic, glui32 w, glui32 h, glui32 x, glui32 y)
{
    if (hack != Hack::Shogun) {
        return false;
    }

    // Shogun is affected by palette changing, but in a way that’s not
    // compatible with Blorb’s adaptive palette. For Arthur and Zork
    // Zero, which have APal chunks, Infocom specified some images
    // without palettes. All other images (with palettes) specifically
    // set the palette to theirs. The non-paletted images just use
    // whatever the palette is set to. This is what APal images are:
    // “inherit” their palette (though this isn’t sufficient for Arthur,
    // either: see the comment to redraw_adaptive_decorations()).
    //
    // In the DOS version of Shogun, the borders adapt to the marginal
    // images that are drawn. But the borders _do_ have their own
    // palettes. So whenever a border is explicitly drawn, it uses its
    // own palette. Then when a marginal image is drawn, it shifts the
    // palette to its needs, which causes the already-on-screen borders
    // to shift as well. This is possibly an aesthetic choice by
    // Infocom, since MCGA has a 256-color palette, but they only use 16
    // entries. There’s no reason Infocom couldn’t have kept the borders
    // the same colors no matter what else was on screen, and they
    // didn’t. Interestingly, Mac borders also change, but to different
    // colors. Mac also has a 256-color palette, so you’d think they
    // would have matched the two versions, but they didn’t. Amiga
    // appears to follow DOS. And Apple II is just entirely different,
    // displaying a decoration on top rather than the sides.
    //
    // The reason, incidentally, why Shogun isn’t compatible with Blorb
    // is in part because Shogun has no “paletteless” images, but more
    // so because the way Shogun works is that it draws the borders
    // once, and then draws marginal images over and over. Due to how
    // hardware worked back then, the entire screen shared a single
    // palette. That means that even though the borders have their own
    // palettes, when a marginal image is drawn, it uses _its_ palette,
    // which, if it reuses palette entries that the borders did, the
    // borders would change. And it does reuse them. This is
    // incompatible with Blorb because it tries to simulate palette
    // shifting without actually _doing_ palette shifting: it does this
    // by saying “the last non-APal image that was drawn sets the
    // palette for future APal images”, which is reasonable enough,
    // except that borders are NOT redrawn frequently, so generally
    // don’t have the opportunity to take the new palette. This is in
    // fact the same reason that Arthur isn’t really compatible with
    // APal, either: it draws its banner and staffs once, and then
    // updates the room image periodically. The room palettes are never
    // given a chance to be applied to the already-existing banner, the
    // way that WOULD happen with real palette shifting. Since Bocfel is
    // aware of this, it fixes this up by simply knowing when to redraw
    // border images instead of relying on the game.
    //
    // The result of all of this is that if a request comes in for
    // picture 3 (a border), set the current palette to that image. This
    // is because the Shogun images have a palette, so when the game
    // itself requests a border draw, it will _take over_ the palette.
    // Which is to say, the rough rule is that Shogun borders take on
    // the palette of _later_ images, not the previous image.
    if (pic == 3) {
        current_palette = pic;
    }

    // The InvisiClues border: a strip across the top with a panel of
    // question marks down each side below it. As with the borders below,
    // only a DOS interpreter asks for the sides, so draw all three when
    // the strip is asked for and ignore the sides when they come.
    if (pic == 50) {
        if (!graphics_window.resize(GraphicsWindow::Type::HintBorder)) {
            return false;
        }
        graphics_window.draw(50, ImageGeometry{0, 0}, w, h);
        graphics_window.draw_shogun_borders();
        return true;
    } else if (pic == 61 || pic == 62) {
        return true;
    }

    if (pic == 1) {
        if (!graphics_window.resize(GraphicsWindow::Type::ShogunTitle)) {
            return false;
        }
        graphics_window.draw(1, ImageGeometry{0, 0}, w, h);
        return true;
    }

    // Different releases of Shogun did the borders differently, as in
    // Zork Zero: DOS split them up, and Blorbs are for DOS, so whenever
    // a drawing request comes in for the left border, unconditionally
    // draw the right as well. 59 is the right border, requested only
    // when the interpreter and game file are both DOS; ignore such
    // requests because it will have already been drawn by
    // graphics_window.draw_shogun_borders().
    if (pic == 3) {
        if (!graphics_window.resize(GraphicsWindow::Type::ShogunNormal)) {
            return false;
        }
        graphics_window.draw_shogun_borders();
        return true;
    } else if (pic == 59) {
        return true;
    }

    // Shogun calculates coordinates (for the maze) correctly.
    if (pic >= 38 && pic <= 44) {
        if (!graphics_window.resize(GraphicsWindow::Type::ShogunMaze)) {
            return false;
        }
        graphics_window.draw_shogun_borders();

        // The maze itself is offset one block from the background (so
        // it’s centered), but to calculate this, it uses the maze
        // window size, which isn’t currently tracked by Bocfel, so its
        // calculating is incorrect. However, since we know the
        // invisible picture size (7x7), it’s simple enough to manually
        // add this to the offsets for the maze pieces. Eventually, if
        // window sizes are properly tracked, this should not be
        // necessary.
        x--;
        y--;
        if (pic != 44) {
            x += SHOGUN_MAZE_BLOCK_WIDTH;
            y += SHOGUN_MAZE_BLOCK_HEIGHT;
        }

        ImageGeometry geom(x, y);
        graphics_window.draw(pic, geom, w, h);
        return true;
    }

    return false;
}

static bool draw_mysterious(glui32 pic, glui32 w, glui32 h)
{
    if (hack != Hack::MysteriousAdventures ||
        mysterious_max_image == 0 ||
        pic > (mysterious_max_image - 3))
    {
        return false;
    }

    if (!graphics_window.resize(GraphicsWindow::Type::Mysterious)) {
        return false;
    }

    // Delay creation till here, to ensure the upper window is created
    // first, so this comes _below_ the upper window.
    if (mysterious_separator.create() && mysterious_separator.resize(GraphicsWindow::Type::MysteriousSeparator)) {
        glui32 sepw, seph;
        if (pic == mysterious_max_image - 3 && glk_image_get_info(mysterious_max_image - 3, &sepw, &seph)) {
            ImageGeometry geom{0, 0};
            mysterious_separator.draw(mysterious_max_image - 3, geom, sepw, seph);

            return true;
        }
    }

    ImageGeometry geom{0, 0};
    graphics_window.draw(pic, geom, w, h);

    return true;
}
#endif

void zerase_window()
{
#ifdef ZTERP_GLK
    switch (as_signed(zargs[0])) {
    case -2:
        for (auto &window : windows) {
            clear_window(&window);
        }
        break;
    case -1:
        close_upper_window();
#ifdef ZTERP_GLK_GRAPHICS
        graphics_window.destroy();
#endif
        [[fallthrough]];
    case 0:
        // 8.7.3.2.1 says V5+ should have the cursor set to 1, 1 of the
        // erased window; V4 the lower window goes bottom left, the upper
        // to 1, 1. Glk doesn’t give control over the cursor when
        // clearing, and that doesn’t really seem to be an issue; so just
        // call glk_window_clear().
        clear_window(mainwin);
        break;
    case 1:
        clear_window(upperwin);
        break;
#ifdef ZTERP_GLK_GRAPHICS
    case 2:
        // In Arthur, the banner/map window is 2, which is represented
        // by the graphics window.
        if (hack == Hack::Arthur) {
#ifdef ZTERP_GLK_OVERLAY
            // RT-REDRAW-MAP clears window 2 before it works out whether
            // there is a map, so this is the way out of the float as
            // well as the way in.
            place_arthur_map_text(false);
#endif
            graphics_window.clear();
            if (arthurwin->id != nullptr) {
                glk_window_clear(arthurwin->id);
            }
        }
#ifdef ZTERP_GLK_OVERLAY
        // In Shogun it’s MENU-WINDOW, which is cleared both before the
        // entries are printed and once a choice has been made.
        if (hack == Hack::Shogun && shogunmenuwin->id != nullptr) {
            glk_window_clear(shogunmenuwin->id);
        }
#endif
        break;
    case 3:
        // Journey uses @erase_window 3 to clear the graphics window,
        // which we normally don’t want; but in one situation it clears
        // the window (to a black background) without drawing a room
        // image, for the dark cavern. For this particular @erase_window
        // call, draw “picture 0” which is interpreted to mean a black
        // rectangle.
        if (hack == Hack::Journey) {
            static const std::set<std::pair<std::string, unsigned long>> draw_black = {
                {"26-890316", 0x112f6},
                {"30-890322", 0x113be},
                {"77-890616", 0x11955},
                {"83-890706", 0x119a5},
            };

            // Image 52 is G-DARK-CAVERN, i.e. the image that is the
            // “lit up” counterpart to the black cavern. If it doesn’t
            // exist, there’s probably no Blorb file loaded.
            glui32 w, h;
            if (draw_black.find({get_story_id(), current_instruction}) != draw_black.end() && glk_image_get_info(52, &w, &h)) {
                draw_journey_background(0, w, h);
            }
        }
        break;
#endif
    default:
        break;
    }

    // glk_window_clear() kills reverse video in Gargoyle. Reapply style.
    set_current_style();
#endif
}

#ifdef ZTERP_GLK_GRAPHICS
// @erase_line in a text grid the game addresses through curwin: to the
// end of the line for 1, otherwise the V6 count of cells. Writing to
// the stream leaves curwin’s cursor alone, so only Glk’s needs
// putting back.
static void erase_grid_line(winid_t win)
{
    glui32 window_width;
    glk_window_get_size(win, &window_width, nullptr);
    auto erase_end = zargs[0] == 1 ? window_width : curwin->x + (zargs[0] - 1);
    auto *stream = glk_window_get_stream(win);
    for (glui32 i = curwin->x; i < std::min(window_width, erase_end); i++) {
        glk_put_char_stream(stream, ' ');
    }

    glk_window_move_cursor(win, curwin->x, curwin->y);
}
#endif

void zerase_line()
{
#ifdef ZTERP_GLK
#ifdef ZTERP_GLK_GRAPHICS
    // Selectively handle @erase_line for version 6 games.
    if ((hack == Hack::Journey || hack == Hack::Shogun) && curwin == upperwin && upperwin->id != nullptr && zargs[0] > 1) {
        for (glui32 i = upperwin->x; i < std::min<glui32>(upper_window_width, curwin->x + (zargs[0] - 1)); i++) {
            xglk_put_char(UNICODE_SPACE);
        }

        glk_window_move_cursor(curwin->id, curwin->x, curwin->y);

        return;
    }
#ifdef ZTERP_GLK_OVERLAY
    else if (hint_output_window() != nullptr && zargs[0] > 0) {
        erase_grid_line(hint_output_window());
        return;
    }
#endif
    else if (hack == Hack::Arthur && curwin == arthurwin && arthurwin->id != nullptr && zargs[0] > 0) {
        erase_grid_line(arthurwin->id);
        return;
    }
#endif

    // XXX V6 does pixel handling here.
    if (zargs[0] != 1 || curwin != upperwin || upperwin->id == nullptr) {
        return;
    }

    for (uint16_t i = upperwin->x; i < upper_window_width; i++) {
        xglk_put_char(UNICODE_SPACE);
    }

    glk_window_move_cursor(upperwin->id, upperwin->x, upperwin->y);
#endif
}

// XXX This is more complex in V6 and needs to be updated when V6 windowing is implemented.
static void set_cursor(uint16_t y, uint16_t x)
{
#ifdef ZTERP_GLK
    // All the windows in V6 can have their cursor positioned; if full
    // V6 ever comes about this should be fixed. For now just hardcode
    // some Arthur handling: with Arthur, the cursor can be set in
    // either the upper window (as normal) or window 2; window 2 is the
    // window used for text/graphics above the game, e.g. room images,
    // map, inventory, etc.
#ifdef ZTERP_GLK_GRAPHICS
    // Shogun positions the cursor in window 2 as well, walking its menu
    // to redraw the highlight. That window only exists when overlays
    // do, hence the second test.
    bool positionable = curwin == upperwin ||
                        (hack == Hack::Arthur && curwin == arthurwin);
#ifdef ZTERP_GLK_OVERLAY
    positionable = positionable ||
                   (hack == Hack::Shogun && curwin == shogunmenuwin && shogunmenuwin->id != nullptr) ||
                   hint_output_window() != nullptr;
#endif
    if (!positionable) {
#else
    if (curwin != upperwin) {
#endif
        return;
    }

    // -1 and -2 are V6 only, but at least Zracer passes -1 (or it’s
    // trying to position the cursor to line 65535; unlikely!)
    if (as_signed(y) == -1 || as_signed(y) == -2) {
        return;
    }

#ifdef ZTERP_GLK_OVERLAY
    // When RT-REDRAW-MAP has no map to draw, it positions window 2 while
    // that window belongs to the graphics panel. Float the text window
    // over the panel for its message; see place_arthur_map_text().
    //
    // The position it asks for cannot be used: the game centers the
    // message by halving what is left of window 2’s width after the
    // text, and the two are in different units. The width is in the
    // artwork’s pixels, so the game can place the map itself, the text
    // in characters. Center it here instead, from the count the
    // interpreter left at 0x30 on its way out of stream 3.
    if (curwin == arthurwin && arthur_window2_is_graphics()) {
        place_arthur_map_text(true);

        glui32 columns, rows;
        glk_window_get_size(arthurwin->id, &columns, &rows);

        glui32 width = word(0x30);

        curwin->x = width < columns ? (columns - width) / 2 : 0;
        curwin->y = rows > 0 ? (rows - 1) / 2 : 0;

        glk_window_move_cursor(arthurwin->id, curwin->x, curwin->y);

        return;
    }
#endif

    // §8.7.2.3 says 1,1 is the top-left, but at least one program (Paint
    // and Corners) uses @set_cursor 0 0 to go to the top-left; so
    // special-case it.
    if (y == 0) {
        y = 1;
    }

    // This handles 0, but also takes care of working around a bug in Inform’s
    // “box” statement, which causes “x” to be negative if the box’s text is
    // wider than the screen.
    if (as_signed(x) < 1) {
        x = 1;
    }

    // This is actually illegal, but some games (e.g. Beyond Zork) expect it to work.
    if (curwin == upperwin && y > upper_window_height) {
        resize_upper_window(y, true);
    }

#ifdef ZTERP_GLK_GRAPHICS
    auto *cursorwin = (hack == Hack::Arthur && curwin == arthurwin) ? curwin : upperwin;
#ifdef ZTERP_GLK_OVERLAY
    // Shogun’s menu window is addressed the same way: MENU-SELECT walks
    // the entries with CCURSET to redraw the highlight as it moves.
    if (hack == Hack::Shogun && curwin == shogunmenuwin) {
        cursorwin = curwin;
    }

    // So is the hint grid, by H-PUT-UP-FROBS and H-NEW-CURSOR.
    if (hint_output_window() != nullptr) {
        curwin->x = x - 1;
        curwin->y = y - 1;
        glk_window_move_cursor(hint_output_window(), x - 1, y - 1);
        return;
    }
#endif
#else
    auto *cursorwin = upperwin;
#endif

    if (cursorwin->id != nullptr) {
        cursorwin->x = x - 1;
        cursorwin->y = y - 1;

        glk_window_move_cursor(cursorwin->id, x - 1, y - 1);
    }
#endif
}

void zset_cursor()
{
#ifdef ZTERP_GLK_GRAPHICS
    if (hack == Hack::MysteriousAdventures) {
        zargs[0] -= 180;
    }
#endif

    set_cursor(zargs[0], zargs[1]);
}

void zget_cursor()
{
#ifdef ZTERP_GLK
    user_store_word(zargs[0] + 0, upperwin->y + 1);
    user_store_word(zargs[0] + 2, upperwin->x + 1);
#else
    user_store_word(zargs[0] + 0, 1);
    user_store_word(zargs[0] + 2, 1);
#endif
}

static bool prepare_color_opcode(int16_t &fg, int16_t &bg, Window *&win)
{
    // Glk (apart from Gargoyle) has no color support.
#if !defined(ZTERP_GLK) || defined(GLK_MODULE_GARGLKTEXT)
    if (options.disable_color) {
        return false;
    }

    fg = as_signed(zargs[0]);
    bg = as_signed(zargs[1]);
    win = znargs == 3 ? find_window(zargs[2]) : style_window();

    return true;
#else
    return false;
#endif
}

void zset_colour()
{
    int16_t fg, bg;
    Window *win;

    if (prepare_color_opcode(fg, bg, win)) {
        // XXX -1 is a valid color in V6.
        if (fg >= 1 && fg <= 12) {
            win->fg_color = Color(Color::Mode::ANSI, fg);
        }
        if (bg >= 1 && bg <= 12) {
            win->bg_color = Color(Color::Mode::ANSI, bg);
        }

        if (win == mainwin) {
            history.add_fg_color(win->fg_color);
            history.add_bg_color(win->bg_color);
        }

        set_current_style();

#if defined(ZTERP_GLK_GRAPHICS) && defined(GLK_MODULE_GARGLKTEXT)
        update_graphics_bg();
#endif
    }
}

void zset_true_colour()
{
    int16_t fg, bg;
    Window *win;

    if (prepare_color_opcode(fg, bg, win)) {
        if (fg >= 0) {
            win->fg_color = Color(Color::Mode::True, fg);
        } else if (fg == -1) {
            win->fg_color = Color();
        }

        if (bg >= 0) {
            win->bg_color = Color(Color::Mode::True, bg);
        } else if (bg == -1) {
            win->bg_color = Color();
        }

        if (win == mainwin) {
            history.add_fg_color(win->fg_color);
            history.add_bg_color(win->bg_color);
        }

        set_current_style();
    }
}

// V6 has per-window styles, but all others have a global style; in this
// case, track styles via the main window.
void zset_text_style()
{
    // A style of 0 means all others go off.
    if (zargs[0] == 0) {
        style_window()->style.reset();
    } else if (zargs[0] < 16) {
        style_window()->style |= zargs[0];
    }

    if (style_window() == mainwin) {
        history.add_style();
    }

    set_current_style();
}

static bool is_valid_font(Window::Font font)
{
    return font == Window::Font::Normal ||
          (font == Window::Font::Character && !options.disable_graphics_font) ||
          (font == Window::Font::Fixed     && !options.disable_fixed);
}

void zset_font()
{
    Window *win = curwin;

    if (zversion == 6 && znargs == 2 && as_signed(zargs[1]) != -3) {
        ZASSERT(zargs[1] < 8, "invalid window selected: %d", as_signed(zargs[1]));
        win = &windows[zargs[1]];
    }

    if (static_cast<Window::Font>(zargs[0]) == Window::Font::Query) {
        store(static_cast<uint16_t>(win->font));
    } else if (is_valid_font(static_cast<Window::Font>(zargs[0]))) {
        store(static_cast<uint16_t>(win->font));
        win->font = static_cast<Window::Font>(zargs[0]);
        set_current_style();
        if (win == mainwin) {
            history.add_style();
        }
    } else {
        store(0);
    }
}

void zprint_table()
{
    uint16_t text = zargs[0], width = zargs[1], height = zarg_or(2, 1), skip = zarg_or(3, 0);
    uint16_t n = 0;

#ifdef ZTERP_GLK
    uint16_t start = 0; // initialize to appease g++

    if (curwin == upperwin) {
        start = upperwin->x + 1;
    }
#endif

    for (uint16_t i = 0; i < height; i++) {
        for (uint16_t j = 0; j < width; j++) {
            put_char(user_byte(text + n++));
        }

        if (i + 1 != height) {
            n += skip;
#ifdef ZTERP_GLK
            if (curwin == upperwin) {
                set_cursor(upperwin->y + 2, start);
            } else
#endif
            {
                put_char(ZSCII_NEWLINE);
            }
        }
    }
}

void zprint_char()
{
    put_char(zargs[0]);
}

void zprint_num()
{
    for (const auto &c : std::to_string(as_signed(zargs[0]))) {
        put_char(c);
    }
}

void zprint_addr()
{
    print_handler(zargs[0], nullptr);
}

void zprint_paddr()
{
    print_handler(unpack_string(zargs[0]), nullptr);
}

// @scroll_window window pixels. A Glk text buffer cannot scroll, but
// Shogun uses this to make room at the bottom for a menu. Printing the
// requested number of newlines has the same effect.
//
// MAKE-ROOM-FOR is the only caller, and GET-FROM-MENU is the only
// caller of that. The picture code’s two calls are removed with
// MARGINAL-PIC and the CENTER-PIC-X patch, so every remaining request
// is for a menu.
//
// zget_wind_prop() reports the cursor below the last line, causing
// MAKE-ROOM-FOR to scroll by the full requested amount.
void zscroll_window()
{
#ifdef ZTERP_GLK_OVERLAY
    if (hack != Hack::Shogun || find_window(zargs[0]) != mainwin) {
        return;
    }

    auto *stream = glk_window_get_stream(mainwin->id);
    for (int i = 0; i < as_signed(zargs[1]); i++) {
        xglk_put_char_stream(stream, UNICODE_LINEFEED);
    }
#endif
}

// @window_size window height width. Zork Zero uses this for the
// encyclopedia text box and Shogun uses it for the menu window. The
// former supplies artwork pixels while the latter supplies cells.
void zwindow_size()
{
#ifdef ZTERP_GLK_OVERLAY
    if (hack == Hack::ZorkZero && find_window(zargs[0]) == zorkzero_encwindow) {
        // These are artwork pixels measured against the 320-wide page.
        // zorkzero_place_enc_window() scales them to the graphics window.
        zorkzero_enc_h = as_signed(zargs[1]);
        zorkzero_enc_w = as_signed(zargs[2]);

        // PICTURED-ENTRY positions the window and then sizes it, so this
        // is the point at which the page’s text area is fully described.
        if (zorkzero_enc_overlay.active()) {
            zorkzero_place_enc_window(true);
        }
    } else if (hack == Hack::Shogun && find_window(zargs[0]) == shogunmenuwin) {
        // Cells, since zget_wind_prop() reports a font one unit square.
        shogun_menu_rows = zargs[1];
        shogun_menu_cols = zargs[2];

        // WINDEF sets the position and then the size, so this is the point at
        // which the box is fully described. If it’s already up, move it now.
        if (shogun_menu_overlay.active()) {
            shogun_place_menu_overlay(true);
        }
    }
#endif
}

// @move_window window y x, both 1-based. Zork Zero uses both
// coordinates for the encyclopedia text. Shogun supplies the menu’s
// horizontal position; its vertical position is determined separately.
void zmove_window()
{
#ifdef ZTERP_GLK_OVERLAY
    if (hack == Hack::ZorkZero && find_window(zargs[0]) == zorkzero_encwindow) {
        // PICTURED-ENTRY reads ENC-TXT-LOC with PICINF-PLUS-ONE, so
        // what arrives is a Z-machine position with a 1,1 origin. The
        // artwork’s is 0,0, which to_pixel_x() and to_pixel_y() expect.
        zorkzero_enc_y = std::max(0, as_signed(zargs[1]) - 1);
        zorkzero_enc_x = std::max(0, as_signed(zargs[2]) - 1);
    } else if (hack == Hack::Shogun && find_window(zargs[0]) == shogunmenuwin) {
        shogun_menu_x = as_signed(zargs[2]) > 0 ? zargs[2] - 1 : 0;
    }
#endif
}

// XXX This is more complex in V6 and needs to be updated when V6 windowing is implemented.
void zsplit_window()
{
#ifdef ZTERP_GLK_GRAPHICS
    if (hack == Hack::MysteriousAdventures) {
        zargs[0] = 3;
    }
#endif

    if (zargs[0] == 0) {
        close_upper_window();
    } else {
        resize_upper_window(zargs[0], true);
    }
}

void zset_window()
{
    set_current_window(find_window(zargs[0]));
}

#ifdef ZTERP_GLK
static void window_change()
{
#ifdef ZTERP_GLK_GRAPHICS
    // This calls V-$REFRESH, i.e. the “refresh” or “$refresh” verb.
    auto zorkzero_refresh = [](){
        static const std::map<std::string, uint32_t> refresh_addrs = {
            {"296-881019", 0x150b8},
            {"366-890323", 0x133fc},
            {"383-890602", 0x134d8},
            {"393-890714", 0x13614},
        };

        if (auto addr = refresh_addrs.find(get_story_id()); addr != refresh_addrs.end()) {
            // The “1” argument means “don't clear”. Whether clearing
            // would be good or not is immaterial: Glk doesn’t allow
            // windows to be erased when there's line input pending, and
            // most of the time, that will be the case.
            internal_call((addr->second - header.routines_offset) / 4, {1});
        }
    };

    // This has to happen before anything below resizes a graphics
    // window, since it updates full_window_width.
    find_window_size();

    auto current_type = graphics_window.type();

    graphics_window.destroy();
    close_journey_window();
    mysterious_separator.destroy();

    // Shogun won’t redraw its borders on resize, so do it here.
    if (current_type == GraphicsWindow::Type::ShogunNormal ||
        current_type == GraphicsWindow::Type::HintBorder ||
        current_type == GraphicsWindow::Type::ShogunMaze)
    {
        graphics_window.resize(current_type);

        // The sides are all there is to the other two, but the InvisiClues
        // border has a strip across the top as well, and nothing will ask
        // for it again: DISPLAY-BORDER is only called on the way in.
        if (current_type == GraphicsWindow::Type::HintBorder) {
            glui32 w, h;
            if (glk_image_get_info(50, &w, &h)) {
                graphics_window.draw(50, ImageGeometry{0, 0}, w, h);
            }
        }

        graphics_window.draw_shogun_borders();
    }

    // As with Shogun, borders aren’t redrawn on resize in Zork Zero.
    // But Zork Zero’s graphics are much more complicated: there are
    // borders, a compass, games, the rebus, encyclopedia, etc. And
    // unlike Arthur, Zork Zero doesn’t redraw on room change. To get it
    // to redraw, call V-$REFRESH.
    if (hack == Hack::ZorkZero) {
        zorkzero_refresh();
    }
#endif

    // Track the new size of the upper window. This has to come after
    // the Shogun/Zork Zero redraws above to ensure the window size is
    // properly calculated with borders in mind.
    if (zversion >= 3 && upperwin->id != nullptr) {
        glui32 w, h;

        glk_window_get_size(upperwin->id, &w, &h);

        upper_window_width = w;

        resize_upper_window(h, false);
    }

    // §8.4
    // Only 0x20 and 0x21 are mentioned; what of 0x22 and 0x24? Zoom and
    // Windows Frotz both update the V5 header entries, so do that here,
    // too.
    //
    // Also, no version restrictions are given, but assume V4+ per §11.1.
    if (zversion >= 4) {
        auto [width, height] = get_screen_size();

        store_byte(0x20, height > 254 ? 254 : height);
        store_byte(0x21, width > 255 ? 255 : width);

        if (zversion >= 5) {
            store_word(0x22, width > UINT16_MAX ? UINT16_MAX : width);
            store_word(0x24, height > UINT16_MAX ? UINT16_MAX : height);
        }
    } else {
        zshow_status();
    }

    // Set the “request redraw” flag for AMFV. It may seem like
    // this is a no-brainer to set for all V4+ games, but that’s not so:
    // this can be more destructive than is necessary. Most games will
    // clear the screen when this bit is set, losing any on-screen text.
    // It’s really only AMFV where this is completely non-destructive,
    // as it just does a status line redraw, not a full screen redraw.
    //
    // Beyond Zork, Zork Zero, and Shogun all clear the screen. We
    // already take care of Zork Zero and Shogun manually above, and
    // it’s trivial for the user to type REFRESH in Beyond Zork, and
    // only slightly less trivial to select the “Refresh” menu option in
    // Journey; and Journey will redraw the graphics the next time you
    // change rooms, anyway.
    if (is_game(Game::AMFV)) {
        store_word(0x10, word(0x10) | FLAGS2_REDRAW);
    }

#ifdef ZTERP_GLK_GRAPHICS
    if (hack == Hack::ZorkZero) {
        // Zork Zero calls “@picture_data 383” to figure out how wide
        // the status bar is. Bocfel hijacks this call to return the
        // upper window width, which is where the status will actually
        // be drawn. But starting in release 383 (coincidentally), Zork
        // Zero caches this value so as not to keep calling
        // @picture_data as earlier versions did. That means the width
        // is wrong after a resize. Zork Zero stores cached values in a
        // table called SL-LOC-TBL, and the 3rd word in that table is
        // the width of the window, i.e. the cached value we care about.
        // If necessary, find the address of SL-LOC-TBL and update the
        // relevant cached entry.
        static const std::unordered_map<std::string, uint16_t> sl_loc_tbl = {
            {"383-890602", 0x70a3},
            {"393-890714", 0x70a5},
        };

        if (auto addr = sl_loc_tbl.find(get_story_id()); addr != sl_loc_tbl.end()) {
            store_word(addr->second + 6, upper_window_width + 1);
        }

        // If the window is widened, there will be artifacts from the
        // previous status bar, so clear them. Then force another
        // refresh. Refresh must be called twice:
        //
        // The first refresh causes the border to be redrawn, which
        // resizes the upper window. The upper window width is then
        // calculated and stored in SL-LOC-TBL. With the actual value
        // now available, the second refresh causes the status bar to
        // properly be drawn.
        //
        // In short, the first refresh is for the border, the second is
        // for the status bar.
        clear_window(upperwin);
        zorkzero_refresh();
    }
#endif

#ifdef ZTERP_GLK_OVERLAY
    // Restore the hint grid after the main-window layout changes. The
    // game does not know about this window and cannot redraw it.
    place_hint_grid(hint_overlay.active());
#endif
}
#endif

#ifdef ZTERP_GLK
static bool timer_running;

static void start_timer(uint16_t n)
{
    if (!timer_available()) {
        return;
    }

    if (timer_running) {
        die("nested timers unsupported");
    }
    glk_request_timer_events(n * 100);
    timer_running = true;
}

static void stop_timer()
{
    if (!timer_available()) {
        return;
    }

    glk_request_timer_events(0);
    timer_running = false;
}

// Where character input is requested, and so where it must be canceled.
static winid_t char_input_window()
{
#ifdef ZTERP_GLK_OVERLAY
    // Read from the hint grid, not the buffer beneath it: asking the
    // buffer would leave it holding unread text, raising a [more] prompt,
    // which quite correctly hides the overlay standing on top of it.
    if (hint_output_window() != nullptr) {
        return hint_output_window();
    }
#endif

    return curwin->id;
}

static void request_char()
{
    if (have_unicode) {
        glk_request_char_event_uni(char_input_window());
    } else {
        glk_request_char_event(char_input_window());
    }
}

static void request_line(Line &line, glui32 maxlen)
{
    // If the game has specified any line terminators, enable them.
#ifdef GLK_MODULE_LINE_TERMINATORS
    if (glk_gestalt(gestalt_LineTerminators, 0)) {
        glk_set_terminators_line_event(curwin->id, term_keys.data(), term_keys.size());
    }
#endif

    if (have_unicode) {
        glk_request_line_event_uni(curwin->id, line.unicode.data(), maxlen, line.len);
    } else {
        glk_request_line_event(curwin->id, line.latin1.data(), maxlen, line.len);
    }
}

// If an interrupt is called, cancel any pending read events. They will
// be restarted after the interrupt returns. If this was a line input
// event, “line” will be updated with the length of the input that had
// been entered at the time of cancellation.
static void cancel_read_events(Line &line)
{
    event_t ev;

    glk_cancel_char_event(char_input_window());
    glk_cancel_line_event(curwin->id, &ev);
    if (upperwin->id != nullptr) {
        glk_cancel_mouse_event(upperwin->id);
    }

    // If the pending read was a line input, set the line length to the
    // amount read before cancellation (this will already have been
    // stored in the line struct by Glk).
    if (ev.type == evtype_LineInput) {
        line.len = ev.val1;
    }
}

// This is a wrapper around internal_call() which handles screen-related
// issues: input events are canceled (in case the interrupt routine
// calls input functions) and the window is restored if the interrupt
// changes it. The value from the internal call is returned. This does
// not restart the canceled read events.
static uint16_t handle_interrupt(uint16_t addr, Line *line)
{
    Window *saved = curwin;

    if (line != nullptr) {
        cancel_read_events(*line);
    }

    uint16_t ret = internal_call(addr);

    // It’s possible for an interrupt to switch windows; if it does,
    // simply switch back. This is the easiest way to deal with an
    // undefined bit of the Z-machine.
    if (curwin != saved) {
        set_current_window(saved);
    }

    return ret;
}

// Request mouse events in all supported windows, which is to say the
// upper window (if it exists) and the graphics window (if it exists).
static void request_mouse_events()
{
    if (upperwin->id != nullptr) {
        glk_request_mouse_event(upperwin->id);
    }

#ifdef ZTERP_GLK_GRAPHICS
    if (graphics_window.id() != nullptr) {
        glk_request_mouse_event(graphics_window.id());
    }
#endif
}

// All read events are canceled during an interrupt, but after an
// interrupt returns, it’s possible that the read should be restarted,
// so this function does that.
static void restart_read_events(Line &line, const Input &input, bool enable_mouse)
{
    switch (input.type) {
    case Input::Type::Char:
        request_char();
        break;
    case Input::Type::Line:
        request_line(line, input.maxlen);
        break;
    }

    if (enable_mouse) {
        request_mouse_events();
    }
}
#endif

void screen_flush()
{
#ifdef ZTERP_GLK
    event_t ev;

    glk_select_poll(&ev);
    switch (ev.type) {
    case evtype_None:
        break;
    case evtype_Arrange:
        window_change();
        break;
#ifdef GLK_MODULE_SOUND2
    case evtype_SoundNotify: {
        sound_stopped(ev.val2);
        uint16_t sound_routine = sound_get_routine(ev.val2);
        if (sound_routine != 0) {
            handle_interrupt(sound_routine, nullptr);
        }
        break;
    }
#endif
    default:
        // No other events should arrive. Timers are only started in
        // get_input() and are stopped before that function returns.
        // Input events will not happen with glk_select_poll(), and no
        // other event type is expected to be raised.
        break;
    }
#endif
}

// These are cursor, function, and keypad keys.
template <typename T>
static bool special_zscii(T c)
{
    return c >= 129 && c <= 154;
}

// This is called when input stream 1 (read from file) is selected. If
// it succefully reads a character/line from the file, it fills the
// struct at “input” with the appropriate information and returns true.
// If it fails to read (likely due to EOF) then it sets the input stream
// back to the keyboard and returns false.
static bool istream_read_from_file(Input &input)
{
    if (input.type == Input::Type::Char) {
        std::optional<uint32_t> c;

        // If there are carriage returns in the input, this is almost
        // certainly a command script from a Windows system being run on a
        // non-Windows system, so ignore them.
        do {
            c = istreamio->getc(true);
        } while (c == UNICODE_CARRIAGE_RETURN);

        if (!c.has_value()) {
            input_stream(ISTREAM_KEYBOARD);
            return false;
        }

        // Don’t translate special ZSCII characters (cursor keys, function keys, keypad).
        if (special_zscii(c)) {
            input.key = *c;
        } else {
            input.key = unicode_to_zscii_q[*c];
        }
    } else {
        std::vector<uint16_t> line;

        try {
            line = istreamio->readline();
        } catch (const IO::EndOfFile &) {
            input_stream(ISTREAM_KEYBOARD);
            return false;
        }

        // As above, ignore carriage returns.
        if (!line.empty() && line.back() == UNICODE_CARRIAGE_RETURN) {
            line.pop_back();
        }

        if (line.size() > input.maxlen) {
            line.resize(input.maxlen);
        }

        input.len = line.size();

#ifdef ZTERP_GLK
        if (curwin->id != nullptr) {
            glk_set_style(style_Input);
            for (const auto &c : line) {
                xglk_put_char(c);
            }
            xglk_put_char(UNICODE_LINEFEED);
            set_current_style();
        }
#else
        for (const auto &c : line) {
            IO::standard_out().putc(c);
        }
        IO::standard_out().putc(UNICODE_LINEFEED);
#endif

        std::copy(line.begin(), line.end(), input.line.begin());
    }

#ifdef ZTERP_GLK
    // It’s possible that output is buffered, meaning that until
    // glk_select() is called, output will not be displayed. When reading
    // from a command-script, flush on each command so that output is
    // visible while the script is being replayed.
    screen_flush();

    saw_input = true;
#endif

    return true;
}

#ifdef GLK_MODULE_LINE_TERMINATORS
// Glk returns terminating characters as keycode_*, but we need them as
// ZSCII. This should only ever be called with values that are matched
// in the switch, because those are the only ones that Glk was told are
// terminating characters. In the event that another keycode comes
// through, though, treat it as Enter.
static uint8_t zscii_from_glk(glui32 key)
{
    switch (key) {
    case keycode_Up:     return ZSCII_UP;
    case keycode_Down:   return ZSCII_DOWN;
    case keycode_Left:   return ZSCII_LEFT;
    case keycode_Right:  return ZSCII_RIGHT;
    case keycode_Func1:  return ZSCII_F1;
    case keycode_Func2:  return ZSCII_F2;
    case keycode_Func3:  return ZSCII_F3;
    case keycode_Func4:  return ZSCII_F4;
    case keycode_Func5:  return ZSCII_F5;
    case keycode_Func6:  return ZSCII_F6;
    case keycode_Func7:  return ZSCII_F7;
    case keycode_Func8:  return ZSCII_F8;
    case keycode_Func9:  return ZSCII_F9;
    case keycode_Func10: return ZSCII_F10;
    case keycode_Func11: return ZSCII_F11;
    case keycode_Func12: return ZSCII_F12;
    }

    return ZSCII_NEWLINE;
}
#endif

// Attempt to read input from the user. The input type can be either a
// single character or a full line. If “timer” is not zero, a timer is
// started that fires off every “timer” tenths of a second (if the value
// is 1, it will timeout 10 times a second, etc.). Each time the timer
// times out the routine at address “routine” is called. If the routine
// returns true, the input is canceled.
//
// The function returns true if input was stored, false if there was a
// cancellation as described above.
static bool get_input(uint16_t timer, uint16_t routine, Input &input)
{
    // If either of these is zero, no timeout should happen.
    if (timer == 0) {
        routine = 0;
    }
    if (routine == 0) {
        timer = 0;
    }

    // Flush all streams when input is requested.
#ifndef ZTERP_GLK
    IO::standard_out().flush();
#endif
    if (scriptio.has_value()) {
        scriptio->flush();
    }
    if (transio.has_value()) {
        transio->flush();
    }
    if (perstransio.has_value()) {
        perstransio->flush();
    }

    // Update the status of line terminators.
#ifdef ZTERP_GLK
    check_terminators();
#endif

    // Generally speaking, newline will be the reason the line input
    // stopped, so set it by default. It will be overridden where
    // necessary.
    input.term = ZSCII_NEWLINE;

    if (istream == ISTREAM_FILE && istream_read_from_file(input)) {
#ifdef ZTERP_GLK
        saw_input = true;
#endif
        return true;
    }
#ifdef ZTERP_GLK
    enum class InputStatus { Waiting, Received, Canceled } status = InputStatus::Waiting;
    Line line;
    Window *saved = nullptr;
    // Mouse support is turned on if the game requests it via Flags2
    // and, for @read, if it adds single click to the list of
    // terminating keys.
    bool enable_mouse = mouse_available() &&
                        ((input.type == Input::Type::Char) ||
                         (input.type == Input::Type::Line && term_mouse));

    // In V6, input might be requested on an unsupported window. If so,
    // switch to the main window temporarily.
    if (curwin->id == nullptr) {
        saved = curwin;
        curwin = mainwin;
        glk_set_window(curwin->id);
    }

    if (enable_mouse) {
        request_mouse_events();
    }

    switch (input.type) {
    case Input::Type::Char:
        request_char();
        break;
    case Input::Type::Line:
#ifdef ZTERP_GLK_GRAPHICS
        // When in borderless mode, there’s no direct way to detect when
        // the game is finished with a 320x200/Game window, so infer
        // it from line input being requested.
        if (hack == Hack::ZorkZero &&
            !zorkzero_has_border() &&
            graphics_window.is_zorkzero_fullscreen())
        {
            graphics_window.destroy();
        }
#endif
        line.len = input.preloaded;
        for (int i = 0; i < input.preloaded; i++) {
            if (have_unicode) {
                line.unicode[i] = input.line[i];
            } else {
                line.latin1 [i] = input.line[i] > 255 ? LATIN1_QUESTIONMARK : input.line[i];
            }
        }

        request_line(line, input.maxlen);
        break;
    }

    if (timer != 0) {
        start_timer(timer);
    }

    while (status == InputStatus::Waiting) {
        event_t ev;

        glk_select(&ev);

        switch (ev.type) {
        case evtype_Arrange:
            window_change();
            break;

        case evtype_Timer:
            // Per Glk, timer events shouldn’t arrive after a call to
            // glk_request_timer_events(0), but at least GlkOte/RemGlk
            // can send such events, and the fix there may be a lot more
            // difficult than simply ignoring spurious events here, so
            // do that.
            if (timer == 0) {
                break;
            }

            stop_timer();

            if (handle_interrupt(routine, &line) != 0) {
                status = InputStatus::Canceled;
                input.term = 0;
            } else {
                restart_read_events(line, input, enable_mouse);
                start_timer(timer);
            }

            break;

        case evtype_CharInput:
            ZASSERT(input.type == Input::Type::Char, "got unexpected evtype_CharInput");
#ifdef ZTERP_GLK_OVERLAY
            // request_char() reads from the hint grid instead of the
            // text buffer used for window 0.
            ZASSERT(ev.win == curwin->id ||
                    ev.win == hint_output_window(),
                    "got evtype_CharInput on unexpected window");
#else
            ZASSERT(ev.win == curwin->id, "got evtype_CharInput on unexpected window");
#endif

            status = InputStatus::Received;

            // As far as the Standard is concerned, there is no difference in
            // behavior for @read_char between versions 4 and 5, as §10.7.1 claims
            // that all characters defined for input can be returned by @read_char.
            // However, according to Infocom’s EZIP documentation, “keys which do
            // not have a single ASCII value are ignored,” with the exception that
            // up, down, left, and right map to 14, 13, 11, and 7, respectively.
            // From looking at V4 games’ source code, it seems like only Bureaucracy
            // makes use of these codes, and then only partially. Since the down
            // arrow maps to 13, it is indistinguishable from ENTER. Bureaucracy
            // treats 14 like ^, meaning “back up a field”. The other arrows aren’t
            // handled specifically, but what Bureaucracy does with all characters
            // it doesn’t handle specifically is print them out. In the DOS version
            // of Bureaucracy, hitting the left arrow inserts a mars symbol (which
            // is what code page 437 uses to represent value 11). Hitting the right
            // arrow results in a beep since 7 is the bell in ASCII. Up and down
            // work as expected, but left and right appear to have just been ignored
            // and untested. On the Apple II, up and down work, but left and right
            // cause nothing to be printed. However, Bureaucracy does advance its
            // cursor location when the arrows are hit (as it does with every
            // character), which causes interesting behavior: hit the right arrow a
            // couple of times and then hit backspace: the cursor will jump to the
            // right first due to the fact that the cursor position was advanced
            // without anything actually being printed. This can’t be anything but a
            // bug.
            //
            // At any rate, given how haphazard the arrow keys act under V4, they
            // are just completely ignored here. This does no harm for Bureaucracy,
            // since ENTER and ^ can be used. In fact, all input which is not also
            // defined for output (save for BACKSPACE, given that Bureaucracy
            // properly handles it) is ignored. As a result, Bureaucracy will not
            // accidentally print out invalid values (since only valid values will
            // be read). This violates both the Standard (since that allows all
            // values defined for input) and the EZIP documentation (since that
            // mandates ASCII only), but hopefully provides the best experience
            // overall: Unicode values are allowed, but Bureaucracy no longer tries
            // to print out invalid characters.
            //
            // Note that if function keys are supported in V4, Bureaucracy will pass
            // them along to @print_char, even though they’re not valid for output,
            // so ignoring things provides better results than following the
            // Standard.
            if (zversion == 4) {
                switch (ev.val1) {
                case keycode_Delete: input.key = ZSCII_DELETE; break;
                case keycode_Return: input.key = ZSCII_NEWLINE; break;

                default:
                    input.key = ZSCII_QUESTIONMARK;
                    if (ev.val1 >= 32 && ev.val1 <= 126) {
                        input.key = ev.val1;
                    } else if (ev.val1 < UINT16_MAX) {
                        uint8_t c = unicode_to_zscii[ev.val1];

                        if (c != 0) {
                            input.key = c;
                        }
                    } else if (ev.val1 > 0xffffffff - keycode_MAXVAL) {
                        status = InputStatus::Waiting;
                        request_char();
                    }
                }
            } else {
                switch (ev.val1) {
                case keycode_Delete: input.key = ZSCII_DELETE; break;
                case keycode_Return: input.key = ZSCII_NEWLINE; break;
                case keycode_Escape: input.key = ZSCII_ESCAPE; break;
                case keycode_Up:     input.key = ZSCII_UP; break;
                case keycode_Down:   input.key = ZSCII_DOWN; break;
                case keycode_Left:   input.key = ZSCII_LEFT; break;
                case keycode_Right:  input.key = ZSCII_RIGHT; break;
                case keycode_Func1:  input.key = ZSCII_F1; break;
                case keycode_Func2:  input.key = ZSCII_F2; break;
                case keycode_Func3:  input.key = ZSCII_F3; break;
                case keycode_Func4:  input.key = ZSCII_F4; break;
                case keycode_Func5:  input.key = ZSCII_F5; break;
                case keycode_Func6:  input.key = ZSCII_F6; break;
                case keycode_Func7:  input.key = ZSCII_F7; break;
                case keycode_Func8:  input.key = ZSCII_F8; break;
                case keycode_Func9:  input.key = ZSCII_F9; break;
                case keycode_Func10: input.key = ZSCII_F10; break;
                case keycode_Func11: input.key = ZSCII_F11; break;
                case keycode_Func12: input.key = ZSCII_F12; break;

                default:
                    input.key = ZSCII_QUESTIONMARK;

                    if (ev.val1 <= UINT16_MAX) {
                        uint8_t c = unicode_to_zscii[ev.val1];

                        if (c != 0) {
                            input.key = c;
                        }
                    }

                    break;
                }
            }

            break;

        case evtype_LineInput:
            ZASSERT(input.type == Input::Type::Line, "got unexpected evtype_LineInput");
            ZASSERT(ev.win == curwin->id, "got evtype_LineInput on unexpected window");
            line.len = ev.val1;
#ifdef GLK_MODULE_LINE_TERMINATORS
            if (zversion >= 5 && ev.val2 != 0) {
                input.term = zscii_from_glk(ev.val2);
            }
#endif
            status = InputStatus::Received;
            break;
        case evtype_MouseInput:
            if (ev.win == upperwin->id) {
#ifdef ZTERP_GLK_GRAPHICS
                if (hack == Hack::ZorkZero) {
                    // Fanucci’s play menu hit-tests clicks against the
                    // upper window with WITHIN?, which is inclusive at
                    // both edges. Boxes are MENU-SPACE by FONT-Y with a
                    // pitch to match, so neighbors share an edge, and at
                    // FONT-Y of 1 every click lands on one. PICK-PLAY
                    // walks them in order, so without this each click
                    // picks the entry above and left of the one aimed at.
                    ev.val1++;
                    ev.val2++;
                }
#endif
                zterp_mouse_click(ev.val1 + 1, ev.val2 + 1);
#ifdef ZTERP_GLK_GRAPHICS
            } else if (ev.win == graphics_window.id() && graphics_window.ratio() > 0) {
                // The ratio is zero until the window has been sized,
                // which is the state a Glk autorestore leaves it in:
                // the window is back, but nothing has recalculated its
                // geometry yet. Ignore clicks until it has.

                // Clicks arrive in window pixels; hand the game the
                // same coordinate space it drew in. A click in the
                // padding to the left of the image converts to a
                // negative x, which the game has no way to express.
                auto x = static_cast<glui32>(std::max(0.0, graphics_window.from_pixel_x(ev.val1)));
                auto y = static_cast<glui32>(graphics_window.from_pixel_y(ev.val2));

                // Compensate for the maze being offset one block.
                if (hack == Hack::Shogun) {
                    if (x >= SHOGUN_MAZE_BLOCK_WIDTH) {
                        x -= SHOGUN_MAZE_BLOCK_WIDTH;
                    }

                    if (y >= SHOGUN_MAZE_BLOCK_HEIGHT) {
                        y -= SHOGUN_MAZE_BLOCK_HEIGHT;
                    }
                }

                // As with the upper window, Glk reports 0-based
                // coordinates, but the games work in the Z-machine’s
                // 1-based space: Zork Zero’s hitboxes come from
                // PICINF-PLUS-ONE, and Arthur subtracts the window
                // origin reported by zget_wind_prop().
                zterp_mouse_click(x + 1, y + 1);
#endif
            }
            status = InputStatus::Received;

            switch (input.type) {
            case Input::Type::Char:
                input.key = ZSCII_CLICK_SINGLE;
                break;
            case Input::Type::Line:
                glk_cancel_line_event(curwin->id, &ev);
                line.len = ev.val1;
                input.term = ZSCII_CLICK_SINGLE;
                break;
            }

            break;
#ifdef GLK_MODULE_SOUND2
        case evtype_SoundNotify: {
            sound_stopped(ev.val2);
            uint16_t sound_routine = sound_get_routine(ev.val2);
            if (sound_routine != 0) {
                handle_interrupt(sound_routine, &line);
                restart_read_events(line, input, enable_mouse);
            }
            break;
        }
#endif
        }
    }

    stop_timer();

    if (enable_mouse) {
        glk_cancel_mouse_event(upperwin->id);
    }

    switch (input.type) {
    case Input::Type::Char:
        glk_cancel_char_event(char_input_window());
        break;
    case Input::Type::Line:
        // Copy the Glk line into the internal input structure.
        input.len = line.len;
        for (glui32 i = 0; i < line.len; i++) {
            if (have_unicode) {
                input.line[i] = line.unicode[i] > UINT16_MAX ? UNICODE_REPLACEMENT : line.unicode[i];
            } else {
                input.line[i] = static_cast<unsigned char>(line.latin1[i]);
            }
        }

        // When line input echoing is turned off (which is the case on
        // Glk implementations that support it), input won’t be echoed
        // to the screen after it’s been entered. This will echo it
        // where appropriate, for both canceled and completed input.
        if (curwin->has_echo) {
            glk_set_style(style_Input);
            for (glui32 i = 0; i < input.len; i++) {
                xglk_put_char(input.line[i]);
            }

            if (input.term == ZSCII_NEWLINE) {
                xglk_put_char(UNICODE_LINEFEED);
            }
            set_current_style();
        }

        // If the current window is the upper window, the position of
        // the cursor needs to be tracked, so after a line has
        // successfully been read, advance the cursor to the initial
        // position of the next line, or if a terminating key was used
        // or input was canceled, to the end of the input.
        if (curwin == upperwin) {
            if (input.term != ZSCII_NEWLINE) {
                upperwin->x += input.len;
            }

            if (input.term == ZSCII_NEWLINE || upperwin->x >= upper_window_width) {
                upperwin->x = 0;
                if (upperwin->y < upper_window_height) {
                    upperwin->y++;
                }
            }

            glk_window_move_cursor(upperwin->id, upperwin->x, upperwin->y);
        }

#ifdef ZTERP_GLK_GRAPHICS
        if (hack == Hack::Arthur && arthurwin->id != nullptr) {
            switch (input.term) {
            // Switching to text: close the graphics window and open the
            // text window.
            case ZSCII_F3: case ZSCII_F4: case ZSCII_F5:
                graphics_window.destroy();
                glk_window_set_arrangement(glk_window_get_parent(arthurwin->id), winmethod_Above | winmethod_Fixed | winmethod_NoBorder, 12, arthurwin->id);
                break;
            // Switching to no window. Close graphics and text.
            case ZSCII_F6:
                graphics_window.destroy();
                glk_window_set_arrangement(glk_window_get_parent(arthurwin->id), winmethod_Above | winmethod_Fixed | winmethod_NoBorder, 0, arthurwin->id);
                break;
            }
        }
#endif
    }

    saw_input = true;

    if (errorwin != nullptr) {
        glk_window_close(errorwin, nullptr);
        errorwin = nullptr;
    }

    if (saved != nullptr) {
        curwin = saved;
        glk_set_window(curwin->id);
    }

    return status != InputStatus::Canceled;
#else
    switch (input.type) {
    case Input::Type::Char: {
        std::vector<uint16_t> line;

        try {
            line = IO::standard_in().readline();
        } catch (const IO::EndOfFile &) {
            zquit();
        }

        if (line.empty()) {
            input.key = ZSCII_NEWLINE;
        } else if (line[0] == UNICODE_DELETE) {
            input.key = ZSCII_DELETE;
        } else if (line[0] == UNICODE_ESCAPE) {
            input.key = ZSCII_ESCAPE;
        } else {
            input.key = unicode_to_zscii[line[0]];
            if (input.key == 0) {
                input.key = ZSCII_NEWLINE;
            }
        }
        break;
    }
    case Input::Type::Line:
        input.len = input.preloaded;

        if (input.maxlen > input.preloaded) {
            std::vector<uint16_t> line;

            try {
                line = IO::standard_in().readline();
            } catch (const IO::EndOfFile &) {
                zquit();
            }

            if (line.size() > input.maxlen - input.preloaded) {
                line.resize(input.maxlen - input.preloaded);
            }

            std::copy(line.begin(), line.end(), &input.line[input.preloaded]);
            input.len += line.size();
        }
        break;
    }

    return true;
#endif
}

void zread_char()
{
    uint16_t timer = zarg_or(1, 0);
    uint16_t routine = zarg_or(2, 0);
    Input input;

    input.type = Input::Type::Char;

    if (options.autosave && !in_interrupt()) {
        SaveType savetype = options.autosave_librarystate ? SaveType::AutosaveLib : SaveType::Autosave;
        do_save(savetype, SaveOpcode::ReadChar);
    }

    if (!get_input(timer, routine, input)) {
        store(0);
        return;
    }

#ifdef ZTERP_GLK
    update_delayed();
#endif

    if (streams.test(OSTREAM_RECORD)) {
        // Write out cursor, function, and keypad keys as-is: these are
        // Unicode control characters, so won’t clash with any valid
        // output. This allows them to be replayed properly.
        if (special_zscii(input.key)) {
            scriptio->putc(input.key);
        } else {
            scriptio->putc(zscii_to_unicode[input.key]);
        }
    }

    store(input.key);
}

// §8.2.3.2 says the hours can be assumed to be in the range [0, 23] and
// can be reduced modulo 12 for 12-hour time. Cutthroats, however, sets
// the hours to 111 if the watch is dropped, on the assumption that the
// interpreter will convert 24-hour time to 12-hour time simply by
// subtracting 12, resulting in an hours of 99 (the minutes are set to
// 99 for a final time of 99:99). Perform the calculation as in
// Cutthroats, which results in valid times for [0, 23] as well as
// allowing the 99:99 hack to work.
std::string screen_format_time(long hours, long minutes)
{
    return fstring("Time: %ld:%02ld%s ", hours <= 12 ? (hours + 11) % 12 + 1 : hours - 12, minutes, hours < 12 ? "am" : "pm");
}

void zshow_status()
{
#ifdef ZTERP_GLK
    glui32 width, height;
    std::string rhs;
    long first = as_signed(variable(0x11)), second = as_signed(variable(0x12));

    if (statuswin.id == nullptr) {
        return;
    }

    strid_t stream = glk_window_get_stream(statuswin.id);

    glk_window_clear(statuswin.id);

    glk_window_get_size(statuswin.id, &width, &height);

#ifdef GLK_MODULE_GARGLKTEXT
    garglk_set_reversevideo_stream(stream, 1);
#else
    glk_set_style_stream(stream, style_Alert);
#endif
    for (glui32 i = 0; i < width; i++) {
        xglk_put_char_stream(stream, UNICODE_SPACE);
    }

    glk_window_move_cursor(statuswin.id, 1, 0);

    // Variable 0x10 is global variable 1.
    print_object(variable(0x10), [](uint8_t c) {
        xglk_put_char_stream(glk_window_get_stream(statuswin.id), zscii_to_unicode[c]);
    });

    if (status_is_time()) {
        rhs = screen_format_time(first, second);
        if (rhs.size() > width) {
            rhs = fstring("%02ld:%02ld", first, second);
        }
    } else {
        // Planetfall and Stationfall are score games, except the value
        // in the second global variable is the current Galactic
        // Standard Time, not the number of moves. Most of Infocom’s
        // interpreters displayed the score and moves as “score/moves”
        // so it didn’t look flat-out wrong to have a value that wasn’t
        // actually the number of moves. When it’s spelled out as
        // “Moves”, though, then the display is obviously wrong. For
        // these two games, rewrite “Moves” as “Time”. Note that this is
        // how it looks in the Solid Gold version, which is V5, meaning
        // Infocom had full control over the status line, implying it is
        // the most correct display.
        if (is_game(Game::Planetfall) || is_game(Game::Stationfall)) {
            rhs = fstring("Score: %ld  Time: %ld ", first, second);
        } else {
            rhs = fstring("Score: %ld  Moves: %ld ", first, second);
        }

        if (rhs.size() > width) {
            rhs = fstring("%ld/%ld", first, second);
        }
    }

    if (rhs.size() <= width) {
        glk_window_move_cursor(statuswin.id, width - rhs.size(), 0);
        glk_put_string_stream(stream, rhs.data());
    }
#endif
}

#ifdef ZTERP_GLK
// These track the position of the cursor in the upper window at the
// beginning of a @read call so that the cursor can be replaced in case
// of a meta-command. They are also stored in the Scrn chunk of any
// meta-saves. They are in Z-machine coordinate format, not Glk, which
// means they are 1-based.
static long starting_x, starting_y;
#endif

// Attempt to read and parse a line of input. On success, return true.
// Otherwise, return false to indicate that input should be requested
// again.
static bool read_handler()
{
    uint16_t text = zargs[0], parse = zargs[1];
    ZASSERT(zversion >= 5 || user_byte(text) > 0, "text buffer cannot be zero sized");
    uint8_t maxchars = zversion >= 5 ? user_byte(text) : user_byte(text) - 1;
    std::array<uint8_t, 256> zscii_string;
    Input input;
    input.type = Input::Type::Line;
    input.maxlen = maxchars;
    input.preloaded = 0;
    uint16_t timer = zarg_or(2, 0);
    uint16_t routine = zarg_or(3, 0);

    if (options.autosave && !in_interrupt()) {
        SaveType savetype = options.autosave_librarystate ? SaveType::AutosaveLib : SaveType::Autosave;
        do_save(savetype, SaveOpcode::Read);
    }

#ifdef ZTERP_GLK
    starting_x = upperwin->x + 1;
    starting_y = upperwin->y + 1;
#endif

    if (zversion <= 3) {
        zshow_status();
    }

    if (zversion >= 5) {
        int i;

        input.preloaded = user_byte(text + 1);
        ZASSERT(input.preloaded <= maxchars, "too many preloaded characters: %d when max is %d", input.preloaded, maxchars);

        for (i = 0; i < input.preloaded; i++) {
            input.line[i] = zscii_to_unicode[user_byte(text + i + 2)];
        }
        // Under Gargoyle, preloaded input generally works as it’s supposed to.
        // Under Glk, it can fail one of two ways:
        //
        // 1. The preloaded text is printed out once, but is not editable.
        // 2. The preloaded text is printed out twice, the second being editable.
        //
        // I have chosen option #2. For non-Glk, option #1 is done by necessity.
        //
        // The “normal” mode of operation for preloaded text seems to be that a
        // particular string is printed, and then that string is preloaded,
        // allowing the already-printed text to be edited. However, there is no
        // requirement that the preloaded text already be on-screen. For
        // example, what should happen if the text “ZZZ” is printed out, but the
        // text “AAA” is preloaded? Infocom’s interpreters display “ZZZ” (and
        // not “AAA”), and allow the user to edit it; but any characters not
        // edited by the user (that is, not backspaced over) are actually stored
        // as “A” characters. The on-screen “Z” characters are not stored.
        //
        // This isn’t possible under Glk, and as it stands, isn’t possible under
        // Gargoyle either (although it could be extended to support it). For
        // now I am not extending Gargoyle: the usual case (preloaded text
        // matching on-screen text) does work with Gargoyle’s unput extensions,
        // and the unusual/unexpected case (preloaded text not matching) at
        // least is usable: the whole preloaded string is displayed and can be
        // edited. I suspect nobody but Infocom ever uses preloaded text, and
        // Infocom uses it in the normal way, so things work just fine.
#ifdef GARGLK
        input.line[i] = 0;
        if (curwin->id != nullptr) {
            // Convert the Z-machine’s 16-bit string to a 32-bit string for Glk.
            std::vector<glui32> line32;
            std::copy(input.line.begin(), input.line.begin() + input.preloaded + 1, std::back_inserter(line32));
            // If the preloaded text would wrap backward in the upper window
            // (to the previous line), limit it to just the current line. For
            // example:
            //
            // |This text bre|
            // |aks          |
            //
            // If the string “breaks” is preloaded, only try to unput the “aks”
            // part. Backing all the way up to “bre” works (as in, is
            // successfully unput) in Gargoyle, but input won’t work: the cursor
            // will be placed at line 1, right after the “e”, and no input will
            // be allowed. At least by keeping the cursor on the second line,
            // proper user input will occur.
            if (curwin == upperwin) {
                uint16_t max = std::min<uint16_t>(input.preloaded, upperwin->x);
                uint8_t start = input.preloaded - max;
                glui32 unput = garglk_unput_string_count_uni(&line32[start]);

                // Since the preloaded text might not have been on the screen
                // (or only partially so), reduce the current and starting X
                // coordinates by the number of unput characters, since that is
                // where Gargoyle will logically be starting input.
                curwin->x -= unput;
                starting_x -= unput;
            } else {
                garglk_unput_string_uni(line32.data());
            }
        }
#endif
    }

    if (!get_input(timer, routine, input)) {
        if (zversion >= 5) {
            store(0);
        }
        return true;
    }

#ifdef ZTERP_GLK
    update_delayed();
#endif

    if (curwin == mainwin) {
        history.add_input(input.line.data(), input.len);
    }

    if (zversion != 6 && options.enable_escape) {
        transcribe(033);
        transcribe('[');
        for (const auto c : *options.escape_string) {
            transcribe(c);
        }
    }

    for (int i = 0; i < input.len; i++) {
        if (zversion != 6) {
            transcribe(input.line[i]);
        }

        if (streams.test(OSTREAM_RECORD)) {
            scriptio->putc(input.line[i]);
        }
    }

    if (zversion != 6 && options.enable_escape) {
        transcribe(033);
        transcribe('[');
        transcribe('0');
        transcribe('m');
    }

    if (zversion != 6) {
        transcribe(UNICODE_LINEFEED);
    }

    if (streams.test(OSTREAM_RECORD)) {
        scriptio->putc(UNICODE_LINEFEED);
    }

    if (!options.disable_meta_commands) {
        input.line[input.len] = 0;

        if (input.line[0] == '/') {
#ifdef ZTERP_GLK
            // If the game is currently in the upper window, blank out
            // everything the user typed so that a re-request of input has a
            // clean slate to work with. Replace the cursor where it was at the
            // start of input.
            if (curwin == upperwin) {
                set_cursor(starting_y, starting_x);
                for (int i = 0; i < input.len; i++) {
                    put_char_u(UNICODE_SPACE);
                }
                set_cursor(starting_y, starting_x);
            }
#endif

            auto [result, say] = handle_meta_command(input.line.data(), input.len);
            switch (result) {
            case MetaResult::Rerequest:
                // The game still wants input, so try again. If this is the main
                // window, print a prompt that hopefully meshes well with the
                // game. If it’s the upper window, don’t print anything, because
                // that will almost certainly do more harm than good.
#ifdef ZTERP_GLK
                if (curwin != upperwin)
#endif
                {
                    screen_print("\n>");
                }

                // Any preloaded text is probably going to have been printed by
                // the game first, which allows Gargoyle to unput the preloaded
                // text. But when re-requesting input, the preloaded text will
                // not be on the screen anymore. Print it again so that it can
                // be unput.
                //
                // If this implementation is not Gargoyle, don’t print
                // anything. The original text (if there in fact was
                // any) will still be on the screen, and the preloaded
                // text will be displayed (and editable) by Glk.
#ifdef GARGLK
                for (int i = 0; i < input.preloaded; i++) {
                    put_char(zscii_to_unicode[user_byte(text + i + 2)]);
                }
#endif

                return false;
            case MetaResult::Say: {
                // Convert the UTF-8 result to Unicode using memory-backed I/O.
                IO io(std::vector<uint8_t>(say.begin(), say.end()), IO::Mode::ReadOnly);
                std::vector<uint16_t> string;

                for (auto c = io.getc(true); c.has_value(); c = io.getc(true)) {
                    string.push_back(*c);
                }

                if (string.size() > input.maxlen) {
                    string.resize(input.maxlen);
                }

                input.len = string.size();
                std::copy(string.begin(), string.end(), input.line.begin());
            }
            }
        }

        // V1–4 do not have @save_undo, so simulate one each time @read is
        // called.
        //
        // Although V5 introduced @save_undo, not all games make use of it
        // (e.g. Hitchhiker’s Guide). Until @save_undo is called, simulate
        // it each @read, just like in V1–4. If @save_undo is called, all
        // of these interpreter-generated save states are forgotten and the
        // game’s calls to @save_undo take over.
        //
        // Because V1–4 games will never call @save_undo, seen_save_undo
        // will never be true. Thus there is no need to test zversion.
        if (!seen_save_undo && !in_interrupt()) {
            push_save(SaveStackType::Game, SaveType::Meta, SaveOpcode::Read, nullptr);
        }
    }

    for (int i = 0; i < input.len; i++) {
        zscii_string[i] = unicode_to_zscii_q[unicode_tolower(input.line[i])];
    }

    if (zversion >= 5) {
        user_store_byte(text + 1, input.len); // number of characters read

        for (int i = 0; i < input.len; i++) {
            user_store_byte(text + i + 2, zscii_string[i]);
        }

        if (parse != 0) {
            tokenize(text, parse, 0, false);
        }

        store(input.term);
    } else {
        for (int i = 0; i < input.len; i++) {
            user_store_byte(text + i + 1, zscii_string[i]);
        }

        user_store_byte(text + input.len + 1, 0);

        tokenize(text, parse, 0, false);
    }

    return true;
}

void zread()
{
    while (!read_handler()) {
    }
}

void zprint_unicode()
{
    if (valid_unicode(zargs[0])) {
        put_char_u(zargs[0]);
    } else {
        put_char_u(UNICODE_REPLACEMENT);
    }
}

void zcheck_unicode()
{
    uint16_t res = 0;

    // valid_unicode() will tell which Unicode characters can be printed;
    // and if the Unicode character is in the Unicode input table, it can
    // also be read. If Unicode is not available, then any character >255
    // is invalid for both reading and writing.
    if (have_unicode || zargs[0] < 256) {
        // §3.8.5.4.5: “Unicode characters U+0000 to U+001F and U+007F to
        // U+009F are control codes, and must not be used.”
        //
        // Even though control characters can be read (e.g. delete and
        // linefeed), when they are looked at through a Unicode lens, they
        // should be considered invalid. I don’t know if this is the right
        // approach, but nobody seems to use @check_unicode, so it’s not
        // especially important. One implication of this is that it’s
        // impossible for this implementation of @check_unicode to return 2,
        // since a character must be valid for output before it’s even
        // checked for input, and all printable characters are also
        // readable.
        //
        // For what it’s worth, interpreters seem to disagree on this pretty
        // drastically:
        //
        // • Zoom 1.1.5 returns 1 for all control characters.
        // • Fizmo 0.7.8 returns 3 for characters 8, 10, 13, and 27, 1 for
        //   all other control characters.
        // • Frotz 2.44 and Nitfol 0.5 return 0 for all control characters.
        // • Filfre 1.1.1 returns 3 for all control characters.
        // • Windows Frotz 1.19 returns 2 for characters 8, 13, and 27, 0
        //   for other control characters in the range 00 to 1f. It returns
        //   a mixture of 2 and 3 for control characters in the range 7F to
        //   9F based on whether the specified glyph is available.
        if (valid_unicode(zargs[0])) {
            res |= 0x01;
            if (unicode_to_zscii[zargs[0]] != 0) {
                res |= 0x02;
            }
        }
    }

#ifdef ZTERP_GLK
    if (glk_gestalt(gestalt_CharOutput, zargs[0]) == gestalt_CharOutput_CannotPrint) {
        res &= ~static_cast<uint16_t>(0x01);
    }
    if (!glk_gestalt(gestalt_CharInput, zargs[0])) {
        res &= ~static_cast<uint16_t>(0x02);
    }
#endif

    store(res);
}

#ifdef ZTERP_GLK_GRAPHICS
static uint16_t blorb_reln;

struct ScaleInfo {
    uint32_t px;
    uint32_t py;
    double stdratio;
    double minratio;
    double maxratio;
};
static std::map<uint32_t, ScaleInfo> picture_scale;
#endif

#ifdef ZTERP_GLK_BLORB
// If possible, load information for image scaling from the Blorb file.
// Errors here aren’t fatal, since the images will just be drawn in
// their original resolution if scale data can’t be loaded.
void screen_load_scale_info()
{
#ifdef ZTERP_GLK_GRAPHICS
    auto *map = giblorb_get_resource_map();
    if (map == nullptr) {
        return;
    }

    giblorb_result_t res;
    if (giblorb_load_chunk_by_type(map, giblorb_method_Memory, &res, blorbid("RelN"), 0) == giblorb_err_None) {
        if (res.length == 2) {
            auto *ptr = static_cast<unsigned char *>(res.data.ptr);
            blorb_reln = (ptr[0] << 8) | ptr[1];
        }
        giblorb_unload_chunk(map, res.chunknum);
    }

    if (giblorb_load_chunk_by_type(map, giblorb_method_Memory, &res, blorbid("Reso"), 0) != giblorb_err_None) {
        return;
    }

    if (res.length >= 24 && (res.length - 24) % 28 == 0) {
        auto *ptr = static_cast<unsigned char *>(res.data.ptr);
        auto px = be32(ptr + 0);
        auto py = be32(ptr + 4);

        // The next 16 bytes are the minimum and maximum window sizes,
        // which aren’t used here.
        if (px != 0 && py != 0) {
            for (size_t i = 24; i < res.length; i += 28) {
                auto num = be32(ptr + i);
                double ratnum = be32(ptr + i +  4);
                double ratden = be32(ptr + i +  8);
                double minnum = be32(ptr + i + 12);
                double minden = be32(ptr + i + 16);
                double maxnum = be32(ptr + i + 20);
                double maxden = be32(ptr + i + 24);

                if (ratden == 0) {
                    continue;
                }

                auto stdratio = ratnum / ratden;
                auto minratio = (minnum == 0 || minden == 0) ? 0 : (minnum / minden);
                auto maxratio = (maxnum == 0 || maxden == 0) ? std::numeric_limits<double>::max() : (maxnum / maxden);

                ScaleInfo scale_info = {
                    px,
                    py,
                    stdratio,
                    minratio,
                    maxratio,
                };

                picture_scale.insert({num, std::move(scale_info)});
            }
        }
    }

    giblorb_unload_chunk(map, res.chunknum);
#endif
}
#endif

#ifdef ZTERP_GLK_GRAPHICS
static std::optional<ImageGeometry> arthur_geom(glui32 pic, int x, int y)
{
    switch (pic) {

    // Banner and map.
    case 54: case 137:
        return ImageGeometry{0, 0};

    // Rooms (centered in window 2 by RT-ROOM-PIC).
    case 4:   case 7:   case 10:  case 11:  case 12:  case 13:  case 14:
    case 15:  case 16:  case 17:  case 18:  case 19:  case 20:  case 21:
    case 22:  case 23:  case 24:  case 25:  case 26:  case 27:  case 28:
    case 29:  case 30:  case 31:  case 32:  case 33:  case 34:  case 35:
    case 36:  case 37:  case 38:  case 39:  case 40:  case 41:  case 42:
    case 43:  case 44:  case 45:  case 46:  case 47:  case 48:  case 49:
    case 50:  case 51:  case 52:  case 53:  case 55:  case 56:  case 57:
    case 58:  case 59:  case 60:  case 61:  case 62:  case 63:  case 64:
    case 65:  case 66:  case 67:  case 68:  case 69:  case 70:  case 71:
    case 72:  case 73:  case 74:  case 75:  case 76:  case 77:  case 78:
    case 81:  case 86:  case 89:  case 101: case 102: case 154: case 157:
    case 162: case 163: case 165: case 166: case 167:

    // Stamps (placed by RT-UPDATE-PICT-WINDOW using invisible pictures).
    case 6:  case 9:  case 80:  case 83:  case 88:  case 91:  case 93: case 95:
    case 97: case 99: case 104: case 156: case 158: case 161: case 169:
        return arthur_image_pos(x, y, ARTHUR_BANNER_IMAGE_X, ARTHUR_BANNER_IMAGE_Y);

    // All map images (106-149) except the map background itself (137).
    case 106: case 107: case 108: case 109: case 110: case 111: case 112:
    case 113: case 114: case 115: case 116: case 117: case 118: case 119:
    case 120: case 121: case 122: case 123: case 124: case 125: case 126:
    case 127: case 128: case 129: case 130: case 131: case 132: case 133:
    case 134: case 135: case 136: case 138: case 139: case 140: case 141:
    case 142: case 143: case 144: case 145: case 146: case 147: case 148:
    case 149:
        return arthur_image_pos(x, y, ARTHUR_MAP_IMAGE_X, ARTHUR_MAP_IMAGE_Y);
    }

    return std::nullopt;
};

void GraphicsWindow::draw(glui32 pic, const ImageGeometry &geom, glui32 w, glui32 h) const
{
    if (m_id == nullptr) {
        return;
    }

    draw_image(m_id, pic,
               to_pixel_x(geom.x), to_pixel_y(geom.y),
               std::round(m_ratio * w), std::round(m_ratio * h * aspect_scale()));

    if (hack == Hack::ZorkZero && !is_game(Game::ZorkZeroDOS)) {
        // Only the DOS version splits the border into a top and sides,
        // and the DOS version is what the “official” Blorb file is
        // meant to be used with. For all other Zork Zero releases, when
        // a request comes in for the top image, draw the side images as
        // well, knowing that’s what’s intended.
        static const std::map<glui32, std::pair<glui32, glui32>> borderpairs = {
            {5, {497, 498}},
            {6, {501, 502}},
            {7, {499, 500}},
            {8, {503, 504}},
        };

        if (auto borderpair = borderpairs.find(pic); borderpair != borderpairs.end()) {
            auto [left, right] = borderpair->second;

            // m_type instead of a name: 503/504 belong to the
            // InvisiClues frame, whose windows are the width of its own
            // panels, and naming the banner here would right-align the
            // right one against an edge 12 pixels outside the window.
            draw_border(m_type, GraphicsWindow::Border::Left, left);
            draw_border(m_type, GraphicsWindow::Border::Right, right);
        }
    }
}

void GraphicsWindow::draw_centered(glui32 pic, glui32 w, glui32 h) const
{
    draw(pic, ImageGeometry((m_base_size.width - w) / 2, (m_base_size.height - h) / 2), w, h);
}

void GraphicsWindow::draw_border(Type type, Border border, glui32 pic) const
{
    glui32 width;

    winid_t id = border == Border::Left ? m_left_border : m_right_border;

    if (id == nullptr || !glk_image_get_info(pic, &width, nullptr)) {
        return;
    }

    int offset = 0;
    // Because the Zork Zero banner images vary in size, but the window
    // size is fixed, offset each right-side border image to ensure the
    // right edge lines up. The InvisiClues panels are all one width and
    // their window is that width, so they need none of this.
    if (type == GraphicsWindow::Type::ZorkZeroBorder && border == Border::Right) {
        offset = 42 - width;
    }

    // For whatever reason, the castle banner is shorter than the other
    // banners: it’s 34 pixels high, while they’re 39. But the graphics
    // window for the castle banner can’t simply be 34 pixels high,
    // because the “south” direction of the compass rose hangs off the
    // bottom of the image. It clearly needs those extra 5 pixels to
    // draw in (and the banner image is clearly cut off: the compass
    // rose is missing its bottom; this is also true of DOS MCGA, though
    // not EGA or CGA).
    //
    // What this all means is that there is a gap between the bottom of
    // the banner and the top of the pillars that shouldn’t be there.
    // The solution is to scale the image as a single unit and draw it
    // into both windows: each shows its own slice of one continuous
    // picture, and Glk clips whatever falls outside.
    double overhang = 0;
    if (type == Type::ZorkZeroBorder && (pic == 497 || pic == 498)) {
        overhang = to_pixel_y(m_base_size.height) - to_pixel_y(34);
    }

    glk_window_clear(id);

    glui32 border_width, border_height;
    glk_window_get_size(id, &border_width, &border_height);
    draw_image(id, pic, offset * ratio(), -overhang, width * ratio(), border_height + overhang);

    if (overhang > 0) {
        glui32 graphics_width;
        glk_window_get_size(m_id, &graphics_width, nullptr);

        // Derive this from the border window’s own position rather than
        // from the right edge: computing it independently rounds
        // differently at some scales, which shows up as a one-pixel jog
        // where the two halves of the image meet.
        double x = border == Border::Left ?
            0 :
            graphics_width - border_width + (offset * ratio());

        draw_image(m_id, pic, x, to_pixel_y(m_base_size.height) - overhang, width * ratio(), border_height + overhang);
    }
}

// Picture 54 is the banner, which is the window: it sits at the origin.
// 170 and 171 are the staffs which hang below it on either side.
void GraphicsWindow::draw_arthur_banner() const
{
    // The banner shares this window with the map, the intro pictures
    // and the demon, so it can only be painted when it’s what the
    // window is currently showing. The staffs need no such test: their
    // border windows only exist for this type.
    if (m_type != Type::ArthurBanner) {
        return;
    }

    draw(54, ImageGeometry{0, 0}, 314, 84);
    draw_border(Type::ArthurBanner, Border::Left, 170);
    draw_border(Type::ArthurBanner, Border::Right, 171);
}

bool GraphicsWindow::draw_zorkzero_border(glui32 pic) const
{
    static const std::unordered_map<glui32, Border> borders = {
        {497, Border::Left},
        {498, Border::Right},
        {499, Border::Left},
        {500, Border::Right},
        {501, Border::Left},
        {502, Border::Right},
        {503, Border::Left},
        {504, Border::Right},
    };

    auto border = borders.find(pic);
    if (border == borders.end()) {
        return false;
    }

    // Pictures 497-502 belong to the banner and 503/504 belong to the
    // InvisiClues screen. GraphicsWindow::Type selects the proper edge
    // for the right-hand image.
    draw_border(m_type, border->second, pic);

    return true;
}

void GraphicsWindow::draw_shogun_borders() const
{
    // The InvisiClues screen has a border of its own: panels of
    // question marks instead of the vines, and hung below a strip that
    // spans the whole width instead of beside a strip of nothing.
    if (m_type == Type::HintBorder) {
        draw_border(Type::HintBorder, Border::Left, 61);
        draw_border(Type::HintBorder, Border::Right, 62);
    } else {
        draw_border(Type::ShogunNormal, Border::Left, 3);
        draw_border(Type::ShogunNormal, Border::Right, 59);
    }
}

static bool draw_arthur(glui32 pic, glui32 w, glui32 h, glui32 x, glui32 y)
{
    if (hack != Hack::Arthur) {
        return false;
    }

    // These are the border staffs. Similar to the banner, Arthur does
    // not draw these frequently, instead drawing them once and
    // expecting them to remain on the screen. To get around this, when
    // room images are drawn, Bocfel unconditionally draws the staffs as
    // well, ensuring they both exist and are drawn in the correct
    // palette. As such, ignore any requests from the game to draw them.
    if (pic == 170 || pic == 171) {
        return true;
    }

    auto type = pic == 1 || pic == 2 || pic == 3 || pic == 84 ? GraphicsWindow::Type::ArthurIntro :
                pic == 85 ?                                     GraphicsWindow::Type::ArthurDemon :
                (pic >= 106 && pic <= 149) ?                    GraphicsWindow::Type::ArthurMap   :
                                                                GraphicsWindow::Type::ArthurBanner;

    if (!graphics_window.resize(type)) {
        return false;
    }

    // If pictures are being drawn, we’re definitely in a graphics mode,
    // so ensure the text window is closed.
    if (arthurwin->id != nullptr) {
        glui32 textheight = 0;
        glk_window_get_size(arthurwin->id, nullptr, &textheight);
        if (textheight != 0) {
            glk_window_set_arrangement(glk_window_get_parent(arthurwin->id), winmethod_Above | winmethod_Fixed | winmethod_NoBorder, 0, arthurwin->id);
        }
    }

    // Intro, ending, and demon pictures. These are only drawn through
    // RT-CENTER-PIC, but it’s not particularly easy to get that routine
    // to work: it relies on the size of window 0 which just isn’t
    // something we can reliably provide (faking 320x200 would maybe be
    // good enough, but on the other hand, manually implementing
    // RT-CENTER-PIC is definitely good enough, so that's what we do).
    if (pic == 1 || pic == 2 || pic == 3 || pic == 84 || pic == 85) {
        graphics_window.draw_centered(pic, w, h);
        return true;
    }

    auto geom = arthur_geom(pic, x, y);
    if (geom.has_value()) {
        graphics_window.draw(pic, *geom, w, h);
        return true;
    }

    return false;
}

static bool draw_zorkzero(glui32 pic, glui32 w, glui32 h, double x, double y)
{
    if (hack != Hack::ZorkZero) {
        return false;
    }

#ifdef ZTERP_GLK_OVERLAY
    // The four sheet pictures and the location of each text area.
    static const std::unordered_map<glui32, ZorkZeroSheet> zorkzero_sheets = {
        {41, {428, 426, false}},   // Tower of Bozbar: B-SPLIT, B-BOTTOM
        {49, {402, 424, false}},   // Peggleboz: PBOZ-SPLIT, PBOZ-BOTTOM
        {73, {427, 425, false}},   // Snarfem: SN-SPLIT, SN-BOTTOM
        {99, {422, 423, true}},    // Double Fanucci: F-SPLIT, F-BOTTOM
    };
#endif

    if (graphics_window.draw_zorkzero_border(pic)) {
        return true;
    }

    auto type = GraphicsWindow::Type::None;

    // Banners.
    if (pic >= 5 && pic <= 7) {
        type = GraphicsWindow::Type::ZorkZeroBorder;

        // Due to size/transparency, the previous banner might leak
        // through if this isn’t cleared first.
        graphics_window.clear();

    // Invisiclues.
    } else if (pic == 8) {
        type = GraphicsWindow::Type::HintBorder;

    // Title, Encyclopedia, Map, Rebus.
    } else if (pic == 1 || pic == 25 || pic == 163 || (pic >= 34 && pic <= 40)) {
        type = GraphicsWindow::Type::ZorkZero320;

    // The Tower of Bozbar, Peggleboz, Snarfem, and Double Fanucci.
#ifdef ZTERP_GLK_OVERLAY
    // Use the table above to identify sheets and locate the text window
    // floated over each one. See zorkzero_place_sheet_text().
    } else if (auto sheet = zorkzero_sheets.find(pic); sheet != zorkzero_sheets.end()) {
        zorkzero_sheet = sheet->second;
        type = GraphicsWindow::Type::ZorkZeroSheet;
#else
    // With nowhere to float the text, each sheet is cut off above the box
    // it would have gone in, and the prose falls below the picture instead.
    } else if (pic == 41 || pic == 49 || pic == 99) {
        type = GraphicsWindow::Type::ZorkZeroGame;

    // Snarfem’s cut is its own: its numbered boxes are drawn below
    // where the other three stop.
    } else if (pic == 73) {
        type = GraphicsWindow::Type::ZorkZeroSnarfem;
#endif
    }

    if (type != GraphicsWindow::Type::None && !graphics_window.resize(type)) {
        return false;
    }

    // The picture number can’t be used to determine whether this is a
    // map or border request, because some overlap, e.g. the compass
    // directions. Track which mode we’re in via the graphics window,
    // and if 320/game mode, assume the game’s got its geometry correct,
    // and plot the pictures just as they’re requested.
    if (graphics_window.is_zorkzero_fullscreen()) {
        ImageGeometry geom{x - 1, y - 1};
        graphics_window.draw(pic, geom, w, h);
        return true;
    }

    if ((pic >= 5 && pic <= 8)  ||  // Banner
        (pic >= 9 && pic <= 24) ||  // Compass directions
        (pic >= 479 && pic <= 485)) // Up & down (plus “box covers”)
    {
        ImageGeometry geom{x - 1, y - 1};
        graphics_window.draw(pic, geom, w, h);
        return true;
    }

    return false;
}

// If this has to reduce the scaled value to UINT16_MAX, the image will
// almost certainly be squashed horizontally or vertically (meaning this
// will not take into account the original aspect ratio of the image),
// but that’s really not something to worry about.
static uint16_t scale_picture(uint32_t num, uint16_t val)
{
    double r = 1.0;

    ScaleInfo scale_info;
    try {
        scale_info = picture_scale.at(num);
    } catch (const std::out_of_range &) {
        return val;
    }

    if (full_window_width.has_value() && full_window_height.has_value()) {
        // Scale according to §11.2 of the Blorb 2.0.4 specification.
        auto erf = std::min(static_cast<double>(*full_window_width)  / scale_info.px,
                            static_cast<double>(*full_window_height) / scale_info.py);

        if (erf * scale_info.stdratio < scale_info.minratio) {
            r = scale_info.minratio;
        } else if (erf * scale_info.stdratio > scale_info.maxratio) {
            r = scale_info.maxratio;
        } else {
            r = erf * scale_info.stdratio;
        }
    }

    auto max = static_cast<double>(std::numeric_limits<uint16_t>::max());
    return std::min(std::round(val * r), max);
}
#endif

#ifndef ZTERP_NO_V6
void zdraw_picture()
{
#ifdef ZTERP_GLK_GRAPHICS
    uint16_t pic = zargs[0];
    uint16_t x = zargs[2];
    uint16_t y = zargs[1];

    // This is ugly, but Journey pic 59 (G-BLACK) does not exist in
    // Blorb files (see draw_journey_background() for a detailed
    // explanation). If picture 59 is requested, temporarily set it to a
    // “known good” picture so that glk_image_get_info() gives useful
    // feedback.
    if (hack == Hack::Journey && pic == 59) {
        pic = 52;
    }

    glui32 w, h;
    if (!glk_gestalt(gestalt_Graphics, 0) || !glk_image_get_info(pic, &w, &h)) {
        return;
    }

    pic = zargs[0];

    // These functions take care of drawing in graphics windows; if they
    // fail to draw, assume a marginal image. These will also fail if V6
    // hacks are turned off, effectively making _all_ images marginal.
    if (draw_arthur(pic, w, h, x, y) ||
        draw_zorkzero(pic, w, h, x, y) ||
        draw_journey(pic, w, h) ||
        draw_shogun(pic, w, h, x, y) ||
        draw_mysterious(pic, w, h)) {
        return;
    }

    if (!glk_gestalt(gestalt_DrawImage, wintype_TextBuffer)) {
        return;
    }

    glui32 align = imagealign_InlineUp;
    auto *picwin = curwin->id;

    if (hack == Hack::ZorkZero) {
        if (pic == 2 || pic == 3 || pic == 4 || (pic >= 204 && pic <= 329) || pic == 440) {
            align = imagealign_MarginLeft;
        }
    }

    if (hack == Hack::Shogun) {
        static const std::unordered_set<glui32> right = {7, 8, 9, 10, 11, 12, 13, 14, 22, 24, 25, 26, 28, 32, 37};
        static const std::unordered_set<glui32> left = {15, 17, 20, 27, 29, 30, 33, 36};

        if (right.find(pic) != right.end()) {
            align = imagealign_MarginRight;
        } else if (left.find(pic) != left.end()) {
            align = imagealign_MarginLeft;
        }

        // This is P-CREST, the only image in Shogun which gets its own
        // window (4). As such, curwin->id is null here, so redirect to
        // the main window.
        if (pic == 31) {
            picwin = mainwin->id;
        }
    }

    if (picwin != nullptr) {
        draw_image(picwin, pic, align, 0, scale_picture(pic, w), std::round(scale_picture(pic, h) * aspect_scale()));
    }
#endif
}

void zpicture_data()
{
    auto pic = zargs[0];
    auto array = zargs[1];

#ifdef ZTERP_GLK_GRAPHICS
    auto *map = giblorb_get_resource_map();

    if (map == nullptr || !glk_gestalt(gestalt_Graphics, 0)) {
        if (pic == 0) {
            user_store_word(array + 0, 0);
            user_store_word(array + 2, 0);
        }

        branch_if(false);
    } else {
        if (pic == 0) {
            glui32 num = 0;
            giblorb_count_resources(map, giblorb_ID_Pict, &num, nullptr, nullptr);

            user_store_word(array + 0, num > UINT16_MAX ? UINT16_MAX : num);
            user_store_word(array + 2, blorb_reln);

            branch_if(num != 0);
        } else {
            if (hack == Hack::ZorkZero) {
                // The following are very hacky, and hopefully will be
                // fixed up at some point.
                if (pic == 1) {
                    // Title screen (P-TITLE). Zork Zero wants to split
                    // the window based on the height of the title
                    // screen, but V6 screen splitting isn’t supported,
                    // so return the size as 0; the split won’t occur,
                    // and the title will appear as intended.
                    user_store_word(array + 0, 0);
                    user_store_word(array + 2, 0);
                    branch_if(true);
                    return;
                } else if (pic == 382) {
                    // This is HERE-LOC, which is meant to return where
                    // to draw, for the status line, the name of the
                    // current room, plus moves. Since the status line
                    // here is just a text grid, it should be drawn at
                    // 0,0. Zork Zero adds 1 to the values returned
                    // here, so zeros are correct.
                    user_store_word(array + 0, 0);
                    user_store_word(array + 2, 0);
                    branch_if(true);
                    return;
                } else if (pic == 383) {
                    // This is REGION-LOC, which is meant to return
                    // where to draw, for the status line, the name of
                    // the current region (Flatheadia, Antharia, etc),
                    // plus moves. As with HERE-LOC above, this is drawn
                    // in a text grid, and Zork Zero expects this to be
                    // the very right-hand side of the region area, as
                    // it calculates the actual location based on the
                    // width of the region text; so returning the whole
                    // width of the upper window is correct.
                    user_store_word(array + 0, 0);
                    user_store_word(array + 2, upper_window_width);
                    branch_if(true);
                    return;
                }
#ifdef ZTERP_GLK_OVERLAY
                // While the Fanucci board is displayed, window 1 floats
                // over it. Convert artwork coordinates to grid cells so
                // the score, labels, and menu align with the sheet.
                glui32 row, col;
                if (zorkzero_fanucci_grid_floating() &&
                    zorkzero_fanucci_position(pic, row, col))
                {
                    user_store_word(array + 0, row);
                    user_store_word(array + 2, col);
                    branch_if(true);
                    return;
                }
#endif
                if (pic == 384) {
                    // Fanucci menu location (F-MENU-LOC). With nothing to
                    // float the menu into it goes at the top left of the
                    // upper window, which sits under the board instead of
                    // on it (see the Fanucci patch in patches.cpp).
                    user_store_word(array + 0, 0);
                    user_store_word(array + 2, 0);
                    branch_if(true);
                    return;
                } else if (pic == 385) {
                    // Fanucci score location (J-SCORE-LOC): below the
                    // menu’s three rows, with a blank row between.
                    user_store_word(array + 0, 4);
                    user_store_word(array + 2, 0);
                    branch_if(true);
                    return;
                }
            } else if (hack == Hack::Journey) {
                // As noted in draw_journey_background(), an image for
                // 59 is accidentally missing, so we fake it; since we
                // can handle drawing it, if the game asks, give it an
                // equivalently-sized picture to what we’ll be drawing.
                if (pic == 59) {
                    pic = 52;
                }
            }

            glui32 w, h;
            if (image_or_rect_size(pic, w, h)) {
                // This is the charging boar’s rectangle, and here the
                // game appears to be off by one: with the game’s
                // values, the tree, which is present in the stamp
                // (instead of it just using transparency) clearly
                // shifts one pixel to the left. With this correction,
                // the tree perfectly lines up with the room’s tree.
                if (hack == Hack::Arthur && pic == 92) {
                    w++;
                }

                user_store_word(array + 0, h);
                user_store_word(array + 2, w);
                branch_if(true);
            } else {
                branch_if(false);
            }
        }
    }
#else
    if (pic == 0) {
        user_store_word(array + 0, 0);
        user_store_word(array + 2, 0);
    }

    // No pictures means no valid pictures, so never branch.
    branch_if(false);
#endif
}

// Attributes are set and can be read back with @get_wind_prop, but
// except for the scripting attribute, none of these actually _do_
// anything: Glk gives no control over scrolling, wrapping, or
// buffering. It would be possible to implement buffering/wrapping for
// text grids, which would improve the room description mode of Arthur,
// but that’s not currently done.
void zwindow_style()
{
    auto *win = find_window(zargs[0]);

    switch (zarg_or(2, 0)) {
    case 0:
        win->attributes = zargs[1];
        break;
    case 1:
        win->attributes |= zargs[1];
        break;
    case 2:
        win->attributes &= ~zargs[1];
        break;
    case 3:
        win->attributes ^= zargs[1];
        break;
    }

    win->attributes &= 0x0f;

#ifdef ZTERP_GLK_OVERLAY
    // A window that does not wrap is one the game means to address, not
    // print to, and the main window cannot be addressed, being a Glk
    // text buffer. In these three games that is the InvisiClues screen
    // asking. See place_hint_grid().
    if (win == mainwin) {
        place_hint_grid((win->attributes & Attribute::Wrap) == 0);
    }
#endif
}

void zget_wind_prop()
{
    uint8_t font_width = 1, font_height = 1;
    uint16_t val;

#ifdef ZTERP_GLK_GRAPHICS
    if (hack == Hack::Shogun && zargs[0] == 0 && zargs[1] == 4) {
        // Check if this is the start of LEAVE-MAZE. This maybe should
        // occur instead during SETUP-TEXT-AND-STATUS but that would
        // have to deal with WINDEF which doesn’t currently get called
        // with good information (likely because there’s some
        // discrepancy in what Bocfel reports to the game regarding font
        // size, display size, etc). For now, it’s enough to “know” that
        // this instruction is part of a routine that handles leaving
        // the maze, and erase the maze screen.
        static const std::set<std::pair<std::string, unsigned long>> leave_maze = {
            {"292-890314", 0x3d4a9},
            {"295-890321", 0x3d691},
            {"311-890510", 0x3d63d},
            {"322-890706", 0x3d8a5},
        };

        if (leave_maze.find({get_story_id(), current_instruction}) != leave_maze.end()) {
            graphics_window.destroy();
            // Force a redraw of the borders.
            if (graphics_window.resize(GraphicsWindow::Type::ShogunNormal)) {
                graphics_window.draw_shogun_borders();
            }
        }
    } else if (hack == Hack::Arthur && zargs[0] == 7) {
        // Window 7 is the whole screen, which Arthur measures in order
        // to center the banner (RT-BANNER-OFFSET). Nothing else reads
        // it, so it can just be the screen a real V6 interpreter would
        // have; see ARTHUR_BANNER_IMAGE_X, which is the other half of
        // that same calculation.
        switch (zargs[1]) {
        case 2: store(ARTHUR_SCREEN_HEIGHT); return;
        case 3: store(ARTHUR_SCREEN_WIDTH);  return;
        }
    } else if (hack == Hack::Arthur && find_window(zargs[0]) == arthurwin) {
        // Window 2 handles both text and graphics (e.g. inventory and
        // map). But since Glk doesn’t support the mixing of text and
        // graphics in this way, two distinct Glk windows are used. If
        // arthurwin is non-zero height, we’re in “text mode”;
        // otherwise one of the graphics modes.
        glui32 text_height = 0;
        if (arthurwin->id != nullptr) {
            glk_window_get_size(arthurwin->id, nullptr, &text_height);
        }

#ifdef ZTERP_GLK_OVERLAY
        // The map message gives window 2 rows without putting it in a
        // text mode, and the map geometry is still the right answer
        // while it is up. See place_arthur_map_text().
        if (arthur_map_text_overlay.active()) {
            text_height = 0;
        }
#endif

        if (text_height != 0) {
            // Reporting the height of the text window is what allows
            // Arthur to display the inventory over multiple columns if
            // necessary.
            if (zargs[1] == 2) {
                store(text_height);
                return;
            }
        } else {
            // Note this doesn’t care which picture is currently being
            // shown: window 2’s geometry is fixed, and the game asks
            // about it in order to decide what to draw, i.e. before
            // there is anything on screen to look at.
            switch (zargs[1]) {
            case 0: store(ARTHUR_WINDOW2_YPOS);   return;
            case 1: store(ARTHUR_WINDOW2_XPOS);   return;
            case 2: store(ARTHUR_WINDOW2_HEIGHT); return;
            case 3: store(ARTHUR_WINDOW2_WIDTH);  return;
            }
        }
    }
#endif

    Window *win = find_window(zargs[0]);

    // These are mostly bald-faced lies.
    switch (zargs[1]) {
    case 0: // y coordinate
        val = 0;
        break;
    case 1: // x coordinate
        val = 0;
        break;
    case 2:  // y size
        val = word(0x24) * font_height;
#ifdef ZTERP_GLK_OVERLAY
        // The hint grid is shorter than the screen, starting below the
        // border strip, and DO-HINTS divides this by the font height to
        // decide how many topics fit in a column. The screen height
        // overfills the first and runs it off the bottom.
        if (win == mainwin && hint_grid_window() != nullptr) {
            glui32 h;
            glk_window_get_size(hint_grid_window(), nullptr, &h);
            val = h * font_height;
        }
#endif
        break;
    case 3:  // x size
        val = word(0x22) * font_width;
#ifdef ZTERP_GLK_OVERLAY
        // A floating upper window can be narrower than the screen.
        // JUSTIFIED-LINE reads WWIDE to lay out the InvisiClues header,
        // so report the overlay width to keep the title and legend
        // aligned with the strip.
        if (upper_window_overlay.active() && win == upperwin) {
            val = upper_window_width * font_width;
        }

        // H-PUT-UP-FROBS uses the hint grid width to position the second
        // column.
        if (win == mainwin && hint_grid_window() != nullptr) {
            glui32 w;
            glk_window_get_size(hint_grid_window(), &w, nullptr);
            val = w * font_width;
        }
#endif

#ifdef ZTERP_GLK_GRAPHICS
        // Shogun’s borders narrow the upper window, and the width at
        // 0x22 is only measured at startup and on a resize, against
        // whichever border is up at the time. The InvisiClues frame is
        // wider than the vine border, so after a resize during hints
        // the header describes the frame until the next resize, and
        // the status line justifies against the wrong edge. The layout
        // always knows the width, so answer from it whenever a border
        // is up; the clipping in put_char_base() uses the same value.
        if (hack == Hack::Shogun && win == upperwin && upper_window_width != 0 &&
            graphics_window.type() != GraphicsWindow::Type::None)
        {
            val = upper_window_width * font_width;
        }
#endif
        break;
    case 4:  // y cursor
#ifdef ZTERP_GLK
        val = curwin->y + 1;
#ifdef ZTERP_GLK_OVERLAY
        // A Glk text buffer has no cursor to report, so this would
        // answer “line 1” however much has been printed. Shogun asks
        // before opening its menu (MAKE-ROOM-FOR) to see how much room
        // is left below the cursor, takes that to mean the whole
        // screen, and never scrolls.
        //
        // Report the line after the last line instead. This tells the
        // game that no room remains and makes it scroll by the number
        // of lines needed for the menu.
        if (hack == Hack::Shogun && win == mainwin) {
            val = (word(0x24) * font_height) + 1;
        }
#endif
#else
        val = 1;
#endif
        break;
    case 5:  // x cursor
#ifdef ZTERP_GLK
        val = curwin->x + 1;
#else
        val = 1;
#endif
        break;
    case 6: // left margin size
        val = 0;
        break;
    case 7: // right margin size
        val = 0;
        break;
    case 8: // newline interrupt routine
        val = 0;
        break;
    case 9: // interrupt countdown
        val = 0;
        break;
    case 10: // text style
        val = win->style.to_ulong();
        break;
    case 11: // colour data
        val = (win->bg_color.as_zcolor() << 8) | win->fg_color.as_zcolor();
        break;
    case 12: // font number
        val = static_cast<uint16_t>(win->font);
        break;
    case 13: // font size
        val = (font_height << 8) | font_width;
        break;
    case 14: // attributes
        val = win->attributes;
        break;
    case 15: // line count
        val = 0;
        break;
    case 16: // true foreground colour
        val = 0;
        break;
    case 17: // true background colour
        val = 0;
        break;
    default:
        die("unknown window property: %u", static_cast<unsigned int>(zargs[1]));
    }

    store(val);
}

// Output should be to the currently-selected window, but since V6 is
// only marginally supported, other windows are not active. Send to the
// main window for the time being.
void zprint_form()
{
    Window *saved = curwin;

    curwin = mainwin;
#ifdef ZTERP_GLK
    glk_set_window(mainwin->id);
#endif

    uint32_t addr = zargs[0];
    for (uint16_t count = user_word(addr); count != 0; count = user_word(addr)) {
        for (uint16_t i = 0; i < count; i++) {
            put_char(user_byte(addr + 2 + i));
        }

        put_char(ZSCII_NEWLINE);

        addr += 2 + count;
    }

    curwin = saved;
#ifdef ZTERP_GLK
    glk_set_window(curwin->id);
#endif
}

void zmake_menu()
{
    branch_if(false);
}

void zbuffer_screen()
{
    store(0);
}

void zjourney_dial()
{
#ifdef ZTERP_GLK_GRAPHICS
    if (journey_window == nullptr) {
        return;
    }

    glui32 w, h;
    if (!glk_image_get_info(zargs[0], &w, &h)) {
        return;
    }

    auto stamp = zargs[1] == 0 ? JourneyStamp{150, ImageGeometry(0, 0)} :
                                 JourneyStamp{150, ImageGeometry(52, 0)};

    draw_journey_stamp(zargs[0], w, h, stamp);
#endif
}

// This is a replacement for GET-FROM-MENU, which does not work properly
// with Glk. zargs[0] is a packed string, which is the prompt for the
// menu. zargs[1] is an LTABLE representing the menu items. zargs[2] is
// a routine to call with the selected menu item, which will return true
// on success or false if the user should be asked to select again.
// zargs[3] is the default (selected) menu item (1-based).
//
// This replacement is only used when overlapping windows are not
// available. Otherwise GET-FROM-MENU is left unchanged, and the call
// frame handling below is unnecessary.
void zshogun_menu()
{
    // Ordinals are ZSCII 1-9 then a-b. The color menu on Amiga goes to
    // 11, necessitating the alphabetic characters.
    std::vector<uint8_t> ordinals = {
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62,
    };

    auto table = zargs[1];
    auto nentries = user_word(table);

    ZASSERT(nentries <= ordinals.size(), "too many menu entries");
    ordinals.resize(nentries);

    // This looks weird but is necessary, due to the fact that the
    // internal_call() below can take you to a part of the game where
    // @save might be called. Normally internal_call() creates a sort of
    // “phantom” call frame: it’s a real call frame, but not valid in
    // the context of Quetzal, as it contains “internal only”
    // information (namely, that the return value needs to be
    // transferred back to internal_call() before it returns). But if
    // you can save in the internal call, that means restore can
    // transfer control there, too. And that means that a phantom call
    // frame is unacceptable: it’s OK to have a phantom call frame when
    // nobody can see it, but saving the game accesses the call stack,
    // and thus exposes the phantom call frame.
    //
    // Right now, pc is pointing to the store byte. In the absence of a
    // @restore, that’s fine: the phantom call frame will include a pc
    // that points to the store byte instead of the next instruction,
    // but since the return will restore the pc inside of the internal
    // call, the fact that it was pointing to a non-instruction is
    // immaterial: it’s never executed. But as soon as @restore is
    // involved, pc _must_ be pointing to an instruction. So what this
    // does is advance pc to the next instruction, while holding onto
    // the store variable so it can manually be set before returning.
    // That also means that a restore from here will exit this loop
    // (because this loop doesn’t exist in the game). Shogun proper
    // will, if you save a game at the end-of-scene menu, restore you
    // back to that menu.
    //
    // Not only do we need to advance the pc, we need store_var for two
    // reasons: first, to store the result. But second, to hand to
    // internal_call(), so that when it creates the now-valid stack
    // frame, it can be told where to store the return value for a
    // @restore.
    //
    // One side effect: When you restore a save file that was made
    // inside the menu, it’s supposed to take you back to the menu. But
    // we can’t do that: all the menu code is here, in the interpreter,
    // so the game can’t restore to it. In the Shogun source,
    // GET-FROM-MENU is passed a callback function (CONTINUE-MENU-F)
    // that is called whenever a menu item is selected. When that
    // function returns, it returns to GET-FROM-MENU, so it can just
    // call it again if it wants. We’ve hijacked the call to it, so
    // _this_ function is calling CONTINUE-MENU-F. But since it’s called
    // from interpreter code, not game code, when CONTINUE-MENU-F
    // returns, we have no choice but to return from @shogun_menu. That
    // is to say, _on restore_, whenever CONTINUE-MENU-F returns, that
    // is identical to causing @shogun_menu to return, which prevents
    // the menu loop from running.
    uint8_t store_var = byte(pc++);

    while (true) {
        put_char(ZSCII_NEWLINE);
        print_handler(unpack_string(zargs[0]), nullptr);
        put_char(ZSCII_NEWLINE);
        put_char(ZSCII_NEWLINE);

        for (int i = 0; i < nentries; i++) {
            auto addr = word(table + ((i + 1) * 2));
            int len = byte(addr++);
            ZASSERT(addr + len <= header.static_end, "menu table out of bounds (0x%lx to 0x%lx)", static_cast<unsigned long>(addr), static_cast<unsigned long>(addr + len));
            std::stringstream ss;
            ss << static_cast<char>(ordinals.at(i)) << ". ";
            ss.write(reinterpret_cast<char *>(&memory[addr]), len);
            if (zargs[3] == i + 1) {
                ss << "[default]";
            }
            for (const auto &c : ss.str()) {
                put_char(c);
            }
            put_char(ZSCII_NEWLINE);
        }

        Input input;

        input.type = Input::Type::Char;

        get_input(0, 0, input);

        // Have ENTER select the default entry as in the original.
        auto it = input.key == ZSCII_NEWLINE && zargs[3] != 0 && (zargs[3] - 1) < nentries ?
            ordinals.begin() + (zargs[3] - 1) :
            std::find(ordinals.begin(), ordinals.end(), input.key);

        if (it == ordinals.end()) {
            continue;
        }

        uint8_t val = (it - ordinals.begin()) + 1;

        interrupt_override = true;
        auto result = internal_call(zargs[2], {val, zargs[1]}, store_var);
        interrupt_override = false;
        if (result != 0) {
            store_variable(store_var, result);
            return;
        }
    }
}

void zshogun_flush_old_picture()
{
#ifdef ZTERP_GLK
    if (curwin->id != nullptr) {
        glk_window_flow_break(curwin->id);
    }
#endif
}
#endif

#ifdef GLK_MODULE_GARGLKTEXT
// Glk does not guarantee great control over how various styles are
// going to look, but Gargoyle does. Abusing the Glk “style hints”
// functions allows for quite fine-grained control over style
// appearance. First, clear the (important) attributes for each style,
// and then recreate each in whatever mold is necessary. Re-use some
// that are expected to be correct (emphasized for italic, subheader for
// bold, and so on).
static void set_default_styles()
{
    std::array<int, 7> styles = { style_Subheader, style_Emphasized, style_Alert, style_Preformatted, style_User1, style_User2, style_Note };

    for (const auto &style : styles) {
        glk_stylehint_set(wintype_AllTypes, style, stylehint_Weight, 0);
        glk_stylehint_set(wintype_AllTypes, style, stylehint_Oblique, 0);

        // This sets wintype_TextGrid to be proportional, which of course is
        // wrong; but text grids are required to be fixed, so Gargoyle
        // simply ignores this hint for those windows.
        glk_stylehint_set(wintype_AllTypes, style, stylehint_Proportional, 1);
    }
}
#endif

bool create_mainwin()
{
#ifdef ZTERP_GLK

#ifdef GLK_MODULE_UNICODE
    have_unicode = glk_gestalt(gestalt_Unicode, 0);
#endif

#ifdef GLK_MODULE_GARGLKTEXT
    set_default_styles();

    glk_stylehint_set(wintype_AllTypes, GStyleBold, stylehint_Weight, 1);

    glk_stylehint_set(wintype_AllTypes, GStyleItalic, stylehint_Oblique, 1);

    glk_stylehint_set(wintype_AllTypes, GStyleBoldItalic, stylehint_Weight, 1);
    glk_stylehint_set(wintype_AllTypes, GStyleBoldItalic, stylehint_Oblique, 1);

    glk_stylehint_set(wintype_AllTypes, GStyleFixed, stylehint_Proportional, 0);

    glk_stylehint_set(wintype_AllTypes, GStyleBoldFixed, stylehint_Weight, 1);
    glk_stylehint_set(wintype_AllTypes, GStyleBoldFixed, stylehint_Proportional, 0);

    glk_stylehint_set(wintype_AllTypes, GStyleItalicFixed, stylehint_Oblique, 1);
    glk_stylehint_set(wintype_AllTypes, GStyleItalicFixed, stylehint_Proportional, 0);

    glk_stylehint_set(wintype_AllTypes, GStyleBoldItalicFixed, stylehint_Weight, 1);
    glk_stylehint_set(wintype_AllTypes, GStyleBoldItalicFixed, stylehint_Oblique, 1);
    glk_stylehint_set(wintype_AllTypes, GStyleBoldItalicFixed, stylehint_Proportional, 0);
#endif

#ifdef ZTERP_GLK_GRAPHICS
    find_window_size();
#endif

    mainwin->id = glk_window_open(nullptr, 0, 0, wintype_TextBuffer, static_cast<glui32>(WindowRock::MainWin));
    if (mainwin->id == nullptr) {
        return false;
    }
    glk_set_window(mainwin->id);

#if defined(ZTERP_GLK_GRAPHICS) && defined(GLK_MODULE_GARGLKTEXT)
    glui32 bg;
    if (glk_style_measure(mainwin->id, style_Normal, stylehint_BackColor, &bg)) {
        default_bg = bg;
    }
#endif

#ifdef GLK_MODULE_LINE_ECHO
    mainwin->has_echo = glk_gestalt(gestalt_LineInputEcho, 0);
    if (mainwin->has_echo) {
        glk_set_echo_line_event(mainwin->id, 0);
    }
#endif

    return true;
#else
    return true;
#endif
}

void create_statuswin()
{
#ifdef ZTERP_GLK
    statuswin.id = glk_window_open(mainwin->id, winmethod_Above | winmethod_Fixed | winmethod_NoBorder, 1, wintype_TextGrid, static_cast<glui32>(WindowRock::StatusWin));
#endif
}

bool have_statuswin()
{
#ifdef ZTERP_GLK
    return statuswin.id != nullptr;
#else
    return false;
#endif
}

void create_upperwin()
{
#ifdef ZTERP_GLK
    // The upper window appeared in V3. */
    if (zversion >= 3) {
        auto location = is_game(Game::Journey) ?
            winmethod_Below :
            winmethod_Above;

        upperwin->id = glk_window_open(mainwin->id, location | winmethod_Fixed | winmethod_NoBorder, 0, wintype_TextGrid, static_cast<glui32>(WindowRock::UpperWin));
        upperwin->x = upperwin->y = 0;
        upper_window_height = 0;

        if (upperwin->id != nullptr) {
            glui32 w, h;

            glk_window_get_size(upperwin->id, &w, &h);
            upper_window_width = w;

            if (h != 0 || upper_window_width == 0) {
                glk_window_close(upperwin->id, nullptr);
                upperwin->id = nullptr;
            }
        }
    }

#ifdef ZTERP_GLK_GRAPHICS
    // For the “bordered” graphics games (Arthur, Zork Zero, and
    // Shogun), resize the graphics window before starting: the games
    // request the upper window size early on and store it, meaning the
    // border windows must already exist or else the upper window size
    // will be reported as larger than it will become.
    if (hack == Hack::ZorkZero) {
        graphics_window.resize(GraphicsWindow::Type::ZorkZeroBorder);
    } else if (hack == Hack::Shogun) {
        graphics_window.resize(GraphicsWindow::Type::ShogunNormal);
    } else if (hack == Hack::Arthur) {
        graphics_window.resize(GraphicsWindow::Type::ArthurBanner);
    }
#endif
#endif
}

bool have_upperwin()
{
#ifdef ZTERP_GLK
    return upperwin->id != nullptr;
#else
    return false;
#endif
}

// Write out the screen state for a Scrn chunk.
//
// This implements version 0 as described in stack.cpp.
IFF::TypeID screen_write_scrn(IO &io)
{
    io.write32(0);

    io.write8(curwin - windows.data());
#ifdef ZTERP_GLK
    io.write16(upper_window_height);
    io.write16(starting_x);
    io.write16(starting_y);
#else
    io.write16(0);
    io.write16(0);
    io.write16(0);
#endif

    for (int i = 0; i < (zversion == 6 ? 8 : 2); i++) {
        io.write8(windows[i].style.to_ulong());
        io.write8(static_cast<uint8_t>(windows[i].font));
        io.write8(static_cast<uint8_t>(windows[i].fg_color.mode));
        io.write16(windows[i].fg_color.value);
        io.write8(static_cast<uint8_t>(windows[i].bg_color.mode));
        io.write16(windows[i].bg_color.value);
    }

    return IFF::TypeID("Scrn");
}

static void try_load_color(Color::Mode mode, uint16_t value, Color &color)
{
    switch (mode) {
    case Color::Mode::ANSI:
        if (value >= 1 && value <= 12) {
            color = Color(Color::Mode::ANSI, value);
        }
        break;
    case Color::Mode::True:
        if (value < 32768) {
            color = Color(Color::Mode::True, value);
        }
        break;
    }
}

// Read and restore the screen state from a Scrn chunk. Since this
// actively touches the screen, it should only be called once the
// restore function has determined that the restore is successful.
void screen_read_scrn(IO &io, uint32_t size)
{
    uint32_t version;
    size_t data_size = zversion == 6 ? 71 : 23;
    uint8_t current_window;
    uint16_t new_upper_window_height;
    struct {
        uint8_t style;
        Window::Font font;
        Color::Mode fgmode;
        uint16_t fgvalue;
        Color::Mode bgmode;
        uint16_t bgvalue;
    } window_data[8];
#ifdef ZTERP_GLK
    uint16_t new_x, new_y;
#endif

    try {
        version = io.read32();
    } catch (const IO::IOError &) {
        throw RestoreError("short read");
    }

    if (version != 0) {
        show_message("Unsupported Scrn version %lu; ignoring chunk", static_cast<unsigned long>(version));
        return;
    }

    if (size != data_size + 4) {
        throw RestoreError(fstring("invalid size: %lu", static_cast<unsigned long>(size)));
    }

    try {
        current_window = io.read8();
        new_upper_window_height = io.read16();
#ifdef ZTERP_GLK
        new_x = io.read16();
        new_y = io.read16();
#else
        io.read32();
#endif

        for (int i = 0; i < (zversion == 6 ? 8 : 2); i++) {
            window_data[i].style = io.read8();
            window_data[i].font = static_cast<Window::Font>(io.read8());
            window_data[i].fgmode = static_cast<Color::Mode>(io.read8());
            window_data[i].fgvalue = io.read16();
            window_data[i].bgmode = static_cast<Color::Mode>(io.read8());
            window_data[i].bgvalue = io.read16();
        }
    } catch (const IO::IOError &) {
        throw RestoreError("short read");
    }

    if (current_window > (zversion == 6 ? 7 : 1)) {
        throw RestoreError(fstring("invalid window: %d", current_window));
    }

    set_current_window(&windows[current_window]);
#ifdef ZTERP_GLK
    delayed_window_shrink = -1;
    saw_input = false;
#endif
    resize_upper_window(new_upper_window_height, false);

#ifdef ZTERP_GLK
    if (new_x > upper_window_width) {
        new_x = upper_window_width;
    }
    if (new_y > upper_window_height) {
        new_y = upper_window_height;
    }
    if (new_y != 0 && new_x != 0) {
        set_cursor(new_y, new_x);
    }
#endif

    for (int i = 0; i < (zversion == 6 ? 8 : 2); i++) {
        if (window_data[i].style < 16) {
            windows[i].style = window_data[i].style;
        }

        if (is_valid_font(window_data[i].font)) {
            windows[i].font = window_data[i].font;
        }

        try_load_color(window_data[i].fgmode, window_data[i].fgvalue, windows[i].fg_color);
        try_load_color(window_data[i].bgmode, window_data[i].bgvalue, windows[i].bg_color);
    }

    set_current_style();

#if defined(ZTERP_GLK_GRAPHICS) && defined(GLK_MODULE_GARGLKTEXT)
    update_graphics_bg();
#endif
}

IFF::TypeID screen_write_bfhs(IO &io)
{
    io.write32(0); // version
    io.write32(history.size());

    for (const auto &entry : history.entries()) {
        switch (entry.type) {
        case History::Entry::Type::Style:
            io.write8(static_cast<uint8_t>(entry.type));
            io.write8(entry.contents.style);
            break;
        case History::Entry::Type::FGColor: case History::Entry::Type::BGColor:
            io.write8(static_cast<uint8_t>(entry.type));
            io.write8(static_cast<uint8_t>(entry.contents.color.mode));
            io.write16(entry.contents.color.value);
            break;
        case History::Entry::Type::InputStart: case History::Entry::Type::InputEnd:
            io.write8(static_cast<uint8_t>(entry.type));
            break;
        case History::Entry::Type::Char:
            io.write8(static_cast<uint8_t>(entry.type));
            io.putc(entry.contents.c);
            break;
        }
    };

    return IFF::TypeID("Bfhs");
}

template <class F>
class ScopeGuard {
public:
    explicit ScopeGuard(F fn) : m_fn(std::move(fn)) {
    }

    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;

    ~ScopeGuard() {
        m_fn();
    }

private:
    F m_fn;
};

void screen_read_bfhs(IO &io, SaveType savetype)
{
    uint32_t version;
    Window saved = *mainwin;
    auto original_style = mainwin->style;
    uint32_t size;
#ifdef ZTERP_GLK
    // Write directly to the main window’s stream instead of going
    // through screen_print() or similar: history playback should be
    // “transparent”, so to speak. It will have already been added to a
    // transcript during the previous round of play (if the user so
    // desired), and this function itself recreates all history directly
    // from the Bfhs chunk.
    strid_t stream = glk_window_get_stream(mainwin->id);
#endif

    try {
        version = io.read32();
    } catch (const IO::IOError &) {
        show_message("Unable to read history version");
        return;
    }

    if (version != 0) {
        show_message("Unsupported history version: %lu", static_cast<unsigned long>(version));
        return;
    }

    mainwin->fg_color = Color();
    mainwin->bg_color = Color();
    mainwin->style.reset();
    set_window_style(mainwin);

    // Glk autosaves maintain window state, so history playback isn’t
    // desired. The history does exist, though, and we want to maintain
    // it for _normal_ saves the user might do after this. So we have a
    // slightly convoluted setup, where Glk autosaves don’t play back
    // history, everything else does, but Bocfel-native autosaves
    // display start/stop history playback banners.
    const bool display = savetype != SaveType::AutosaveLib;
    const bool banners = display && savetype != SaveType::Autosave;

#ifdef ZTERP_GLK
    auto write = [&stream](std::string msg) {
        glk_put_string_stream(stream, msg.data());
    };
#else
    auto write = [](const std::string &msg) {
        std::cout << msg;
    };
#endif

    ScopeGuard guard([&banners, &saved, &write] {
        if (banners) {
            write("[End of history playback]\n\n");
        }
        *mainwin = saved;
        set_window_style(mainwin);
    });

    if (banners) {
        write("[Starting history playback]\n");
    }

    try {
        size = io.read32();
    } catch (const IO::IOError &) {
        return;
    }

    if (size == 0 && savetype == SaveType::Autosave) {
        warning("empty history record");
        screen_print(">");
        return;
    }

    for (uint32_t i = 0; i < size; i++) {
        uint8_t type;
        std::optional<uint32_t> c;

        try {
            type = io.read8();
        } catch (const IO::IOError &) {
            return;
        }

        // Each history entry is added to the history buffer. This is so
        // that new saves, after a restore, continue to include the
        // older save file’s history.
        switch (static_cast<History::Entry::Type>(type)) {
        case History::Entry::Type::Style: {
            uint8_t style;

            try {
                style = io.read8();
            } catch (const IO::IOError &) {
                return;
            }

            mainwin->style = style;
            set_window_style(mainwin);

            history.add_style();

            break;
        }
        case History::Entry::Type::FGColor: case History::Entry::Type::BGColor: {
            uint8_t mode;
            uint16_t value;

            try {
                mode = io.read8();
                value = io.read16();
            } catch (const IO::IOError &) {
                return;
            }

            Color &color = static_cast<History::Entry::Type>(type) == History::Entry::Type::FGColor ? mainwin->fg_color : mainwin->bg_color;

            try_load_color(static_cast<Color::Mode>(mode), value, color);
            set_window_style(mainwin);

            if (static_cast<History::Entry::Type>(type) == History::Entry::Type::FGColor) {
                history.add_fg_color(color);
            } else {
                history.add_bg_color(color);
            }

            break;
        }
        case History::Entry::Type::InputStart:
            original_style = mainwin->style;
            mainwin->style.reset();
            set_window_style(mainwin);
#ifdef ZTERP_GLK
            glk_set_style_stream(stream, style_Input);
#endif
            history.add_input_start();
            break;
        case History::Entry::Type::InputEnd:
            mainwin->style = original_style;
            set_window_style(mainwin);
            history.add_input_end();
            break;
        case History::Entry::Type::Char:
            c = io.getc(false);
            if (!c.has_value()) {
                return;
            }
            for (const auto &clean : cleanse_control(*c)) {
                history.add_char(clean);
                if (display) {
#ifdef ZTERP_GLK
                    xglk_put_char_stream(stream, clean);
#else
                    IO::standard_out().putc(clean);
#endif
                }
            }
            break;
        default:
            return;
        }
    }
}

IFF::TypeID screen_write_bfts(IO &io)
{
    if (!options.persistent_transcript || !perstransio.has_value()) {
        return IFF::TypeID();
    }

    io.write32(0); // Version

    const auto &buf = perstransio->get_memory();
    io.write_exact(buf.data(), buf.size());

    return IFF::TypeID("Bfts");
}

void screen_read_bfts(IO &io, uint32_t size)
{
    uint32_t version;
    std::vector<uint8_t> buf;

    if (!options.persistent_transcript) {
        return;
    }

    if (size < 4) {
        show_message("Corrupted Bfts entry (too small)");
        return;
    }

    try {
        version = io.read32();
    } catch (const IO::IOError &) {
        show_message("Unable to read persistent transcript size from save file");
        return;
    }

    if (version != 0) {
        show_message("Unsupported Bfts version %lu", static_cast<unsigned long>(version));
        return;
    }

    // The size of the transcript is the size of the whole chunk minus
    // the 32-bit version.
    size -= 4;

    if (size > 0) {
        try {
            buf.resize(size);
        } catch (const std::bad_alloc &) {
            show_message("Unable to allocate memory for persistent transcript");
            return;
        }

        try {
            io.read_exact(buf.data(), size);
        } catch (const IO::IOError &) {
            show_message("Unable to read persistent transcript from save file");
            return;
        }
    }

    perstransio.emplace(buf, IO::Mode::Append);
}

class PersistentTranscriptStasher : public Stasher {
public:
    void backup() override {
        m_transcript.reset();

        if (options.persistent_transcript && perstransio.has_value()) {
            const auto &buf = perstransio->get_memory();
            try {
                m_transcript = buf;
            } catch (const std::bad_alloc &) {
            }
        }
    }

    // Never fail: this is an optional chunk so isn’t fatal on error.
    bool restore() override {
        if (!m_transcript.has_value()) {
            return true;
        }

        perstransio.emplace(*m_transcript, IO::Mode::Append);
        m_transcript.reset();

        return true;
    }

    void free() override {
        m_transcript.reset();
    }

private:
    std::optional<std::vector<uint8_t>> m_transcript;
};

static bool check_transcript()
{
    if (!options.persistent_transcript) {
        screen_puts("[Persistent transcripting is turned off]");
        return false;
    }

    if (!perstransio.has_value()) {
        screen_puts("[Persistent transcripting failed to start]");
        return false;
    }

    return true;
}

void screen_save_persistent_transcript()
{
    if (!check_transcript()) {
        return;
    }

    const auto &buf = perstransio->get_memory();

    try {
        IO io(std::nullopt, IO::Mode::WriteOnly, IO::Purpose::Transcript);

        try {
            io.write_exact(buf.data(), buf.size());
            screen_puts("[Saved]");
        } catch (const IO::IOError &) {
            screen_puts("[Error writing transcript to file]");
        }
    } catch (const IO::OpenError &) {
        screen_puts("[Failed to open file]");
        return;
    }
}

void screen_show_persistent_transcript()
{
    if (!check_transcript()) {
        return;
    }

    const auto &buf = perstransio->get_memory();

    std::vector<char> transcript(buf.begin(), buf.end());

    screen_puts("[Opening persistent transcript]");
    screen_flush();

    try {
        zterp_os_show_transcript(transcript);
    } catch (const std::runtime_error &e) {
        screen_printf("[Error showing persistent transcript: %s]\n", e.what());
    }
}

// The DEFINE command in Zork Zero uses cursor positioning in the main
// window, which doesn’t work with Glk. This is a replacement function
// which approximates the original.
//
// Note that while the arrow keys can be defined, most Glk
// implementations use them for cursor/history control during line
// input, so they won’t actually be usable in the game.
void zzork0_define()
{
    struct Entry {
        // The ZSCII key corresponding to this entry.
        const uint8_t key;
        // The label for this key (i.e. its name).
        const std::string label;
        // The existing command for this key.
        std::vector<std::uint8_t> existing;
    };

    // These labels are available in game memory as well, but since
    // they’re fixed, it’s more convenient to hard-code them directly in
    // a data structure here.
    std::vector<Entry> entries = {
        {ZSCII_UP,    " UP"},
        {ZSCII_DOWN,  " DN"},
        {ZSCII_LEFT,  " LF"},
        {ZSCII_RIGHT, " RT"},
        {ZSCII_F1,    " F1"},
        {ZSCII_F2,    " F2"},
        {ZSCII_F3,    " F3"},
        {ZSCII_F4,    " F4"},
        {ZSCII_F5,    " F5"},
        {ZSCII_F6,    " F6"},
        {ZSCII_F7,    " F7"},
        {ZSCII_F8,    " F8"},
        {ZSCII_F9,    " F9"},
        {ZSCII_F10,   "F10"},
    };

    struct Addrs {
        // FKEYS is a PLTABLE which includes “pairs” of values (two
        // consecutive words): the first is the ZSCII value of the key
        // (129 for up, etc). The second is the address of the
        // definition (FDEF); this consists of a two-byte header: the
        // first is the max length of the command (used as the width in
        // @print_table in the original code); the second is the actual
        // length of the command. Following this is the command itself,
        // a sequence of ZSCII characters.
        //
        // FKEYS contains both the keys themselves as well as the
        // entries “Save Definitions”, etc. If this were truly dynamic,
        // we’d have to read this table, but since it’s always the same,
        // this can be hard-coded in the same way as the labels above.
        uint16_t fkeys;

        // The location and size of the key table, for saving and
        // restoring. The table is directly saved/restored instead of
        // going through SOFT-SAVE-DEFS and SOFT-RESTORE-DEFS since
        // those routines clear and change windows and would need to be
        // worked around.
        uint32_t save_offset;
        uint32_t save_size;

        // The (unpacked) address of the SOFT-RESET-DEFAULTS routine.
        uint32_t reset_defaults;
    };

    static const std::unordered_map<std::string, Addrs> addrs_map = {
        {"296-881019", {0x7553, 0x72f3, 0x1c0, 0x14338}},
        {"366-890323", {0xce3c, 0x6e31, 0x1c0, 0x13004}},
        {"383-890602", {0xcdeb, 0x6e3f, 0x1c0, 0x130e0}},
        {"393-890714", {0xce2a, 0x6e41, 0x1c0, 0x1321c}},
    };
    auto addrs_it = addrs_map.find(get_story_id());
    if (addrs_it == addrs_map.end()) {
        screen_puts("[internal error: unable to find addresses]");
        return;
    }
    auto addrs = addrs_it->second;

    while (true) {
        uint16_t fkey = addrs.fkeys + 2;

        screen_puts("Software Function Key definition. Select the key to define. Hit the RETURN/ENTER key to exit.\n");

#ifdef ZTERP_GLK
        glk_set_style(style_Preformatted);
#endif

        // Simultaneously display the menu and load existing definitions.
        for (auto &entry : entries) {
            uint16_t fdef = user_word(fkey + 2);
            uint8_t len = user_byte(fdef + 1);

            entry.existing.resize(len);
            for (int i = 0; i < len; i++) {
                if (user_byte(fdef + 2 + i) == ZSCII_NEWLINE) {
                    entry.existing[i] = LATIN1_PIPE;
                } else {
                    uint8_t c = user_byte(fdef + 2 + i);
                    entry.existing[i] = (c < 32 || c > 126) ? LATIN1_QUESTIONMARK : c;
                }
            }

            screen_printf("%s: %.*s\n", entry.label.c_str(), static_cast<int>(len), reinterpret_cast<char *>(entry.existing.data()));
            fkey += 4;
        }

        screen_puts("");
        screen_puts("  S: Save Definitions");
        screen_puts("  R: Restore Definitions");
        screen_puts("  D: Restore Defaults");

#ifdef ZTERP_GLK
        set_current_style();
#endif

        fkey = addrs.fkeys + 2;

        while (true) {
            Input input;
            input.type = Input::Type::Char;
            get_input(0, 0, input);

            if (input.key == ZSCII_NEWLINE || input.key == ZSCII_ESCAPE) {
                return;
            } else if (input.key == 's' || input.key == 'S') {
                ZASSERT(addrs.save_offset + addrs.save_size < memory_size, "corrupted story: too small");
                try {
                    IO savefile(std::nullopt, IO::Mode::WriteOnly, IO::Purpose::Data);
                    savefile.write_exact(&memory[addrs.save_offset], addrs.save_size);
                } catch (const IO::Error &) {
                    screen_puts("\nFailed.");
                }

                break;
            } else if (input.key == 'r' || input.key == 'R') {
                ZASSERT(addrs.save_offset + addrs.save_size < memory_size, "corrupted story: too small");
                try {
                    IO savefile(std::nullopt, IO::Mode::ReadOnly, IO::Purpose::Data);
                    savefile.read_exact(&memory[addrs.save_offset], addrs.save_size);
                } catch (const IO::Error &) {
                    screen_puts("\nFailed.");
                }

                break;
            } else if (input.key == 'd' || input.key == 'D') {
                internal_call((addrs.reset_defaults - header.routines_offset) / 4);
                break;
            }

            auto entry = std::find_if(entries.begin(), entries.end(), [&input](const Entry &e) {
                return e.key == input.key;
            });

            if (entry == entries.end()) {
                continue;
            }

            auto index = entry - entries.begin();
            uint16_t fdef = user_word(fkey + 2 + (4 * index));

            screen_printf("\n%s: ", entry->label.c_str());

            input.type = Input::Type::Line;
            input.maxlen = user_byte(fdef);
            input.preloaded = entry->existing.size();
            if (input.preloaded > input.maxlen) {
                input.preloaded = input.maxlen;
            }
            std::copy(entry->existing.begin(), entry->existing.end(), input.line.begin());

            get_input(0, 0, input);

            if (input.term != ZSCII_NEWLINE) {
                break;
            }

            user_store_byte(fdef + 1, input.len);
            for (int i = 0; i < input.len; i++) {
                if (input.line[i] == ZSCII_PIPE || input.line[i] == ZSCII_EXCLAMATION) {
                    // A pipe (or exclamation point) in input means
                    // ENTER, which Zork Zero translates and stores
                    // directly as a newline in memory.
                    user_store_byte(fdef + 2 + i, ZSCII_NEWLINE);

                    // Zork Zero doesn’t allow text after typing a pipe;
                    // but to simplify, just bail when a pipe is hit
                    // (storing the truncated length), regardless of
                    // what else was typed.
                    user_store_byte(fdef + 1, i + 1);
                    break;
                } else {
                    // If a non-ASCII character is provided, store it as
                    // a question mark. Zork Zero itself doesn’t allow
                    // such input and although it’s actually possible to
                    // store and use ZSCII extra characters (155-251)
                    // here, there’s no utility in doing so.
                    if (input.line[i] < 32 || input.line[i] > 126) {
                        user_store_byte(fdef + 2 + i, ZSCII_QUESTIONMARK);
                    } else {
                        user_store_byte(fdef + 2 + i, input.line[i]);
                    }
                }
            }

            break;
        }

        screen_puts("");
    }
}

void create_graphicswin()
{
#ifdef ZTERP_GLK_GRAPHICS
    graphics_window.destroy();
    if (glk_gestalt(gestalt_DrawImage, wintype_Graphics)) {
        if ((is_game(Game::Arthur) ||
             is_game(Game::ZorkZero) ||
             is_game(Game::Shogun) ||
             is_game(Game::MysteriousAdventures)) &&
             graphics_window.create())
        {
            if (is_game(Game::Arthur)) {
                hack = Hack::Arthur;
                arthurwin->id = glk_window_open(mainwin->id, winmethod_Fixed | winmethod_Above | winmethod_NoBorder, 0, wintype_TextGrid, static_cast<glui32>(WindowRock::ArthurWin));
            } else if (is_game(Game::ZorkZero)) {
                hack = Hack::ZorkZero;
            } else if (is_game(Game::Shogun)) {
                hack = Hack::Shogun;
#ifdef ZTERP_GLK_OVERLAY
                // MENU-WINDOW. It lives outside the layout entirely:
                // shogun_place_menu_overlay() parks it over the main
                // window when the game selects it.
                shogunmenuwin->id = open_floating_window(wintype_TextGrid, WindowRock::ShogunMenuWin);
#endif
            } else if (is_game(Game::MysteriousAdventures)) {
                hack = Hack::MysteriousAdventures;

                auto *map = giblorb_get_resource_map();
                if (map == nullptr || giblorb_count_resources(map, giblorb_ID_Pict, &mysterious_max_image, nullptr, nullptr) != giblorb_err_None) {
                    graphics_window.destroy();
                    hack = Hack::None;
                }
            }
        } else if (is_game(Game::Journey)) {
            hack = Hack::Journey;
        }
    }

    // Redirecting windows is unnecessary when a “supported” game is running.
    if (hack != Hack::None) {
        options.redirect_v6_windows = false;
    }
#endif
}

void init_screen(bool first_run)
{
#ifdef ZTERP_GLK_OVERLAY
    // Return floating windows to the layout before clearing or closing
    // them.
    reset_overlays();
#endif

    for (auto &window : windows) {
        window.style.reset();
        window.fg_color = window.bg_color = Color();
        window.font = Window::Font::Normal;
        window.attributes = Attribute::Buffer;

#ifdef ZTERP_GLK
        clear_window(&window);
#endif
    }

    windows[0].attributes |= (Attribute::Scroll | Attribute::Wrap | Attribute::Script);

    close_upper_window();

#ifdef ZTERP_GLK
    // For now, unless the user disables it, point windows 2–7 (from
    // version 6) to the main window, allowing all output (text and
    // graphics) to be seen. Things could get pretty jumbled but it’s
    // not inherently worse than a chunk of output missing.
    if (options.redirect_v6_windows) {
        for (int i = 2; i < 8; i++) {
            windows[i].id = windows[0].id;
        }
    }

    if (statuswin.id != nullptr) {
        glk_window_clear(statuswin.id);
    }

    if (errorwin != nullptr) {
        glk_window_close(errorwin, nullptr);
        errorwin = nullptr;
    }

    stop_timer();

#ifdef ZTERP_GLK_GRAPHICS
    current_palette.reset();

    if (first_run) {
        build_palette_map();
    }

#ifdef GLK_MODULE_GARGLKTEXT
    update_graphics_bg();
#endif
#endif

#else
    have_unicode = true;
#endif

    if (first_run && options.persistent_transcript) {
        try {
            perstransio.emplace(std::vector<uint8_t>(), IO::Mode::WriteOnly);
        } catch (const IO::OpenError &) {
            warning("Failed to start persistent transcripting");
        }

        stash_register(std::make_unique<PersistentTranscriptStasher>());
    }

    // On restart, deselect stream 3 and select stream 1. This allows
    // the command script and transcript to persist across restarts,
    // while resetting memory output and ensuring screen output.
    streams.reset(OSTREAM_MEMORY);
    streams.set(OSTREAM_SCREEN);
    stream_tables.clear();

    set_current_window(mainwin);
}
