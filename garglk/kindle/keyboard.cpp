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

#include <cstdlib>

#if __has_include(<openlipc.h>)
#include <openlipc.h>
#else
#include <lipc.h>
#endif

#include "kindle.h"

static constexpr const char *APPLICATION_ID = "net.fabiszewski.gargoyle";

static LIPC *lipc()
{
    static LIPC *instance = [] {
        LIPC *lipc = LipcOpen(APPLICATION_ID);
        if (lipc != nullptr) {
            std::atexit([] { LipcClose(::lipc()); });
        }
        return lipc;
    }();

    return instance;
}

void kindle::show_keyboard()
{
    int shown = 0;

    if (lipc() == nullptr) {
        return;
    }

    LipcGetIntProperty(lipc(), "com.lab126.keyboard", "show", &shown);
    if (shown == 0) {
        LipcSetStringProperty(lipc(), "com.lab126.keyboard", "open", "net.fabiszewski.gargoyle:abc:0");
    }
}

void kindle::hide_keyboard()
{
    if (lipc() != nullptr) {
        LipcSetStringProperty(lipc(), "com.lab126.keyboard", "close", APPLICATION_ID);
    }
}

// The keyboard covers the bottom 11/32 of the screen in portrait.
int kindle::keyboard_height()
{
    return gdk_screen_get_height(gdk_screen_get_default()) * 11 / 32;
}

std::optional<std::string> kindle::keyboard_language()
{
    char *value = nullptr;

    if (lipc() == nullptr || LipcGetStringProperty(lipc(), "com.lab126.keyboard", "language", &value) != LIPC_OK || value == nullptr) {
        return std::nullopt;
    }

    std::string language = value;
    LipcFreeString(value);
    return language;
}
