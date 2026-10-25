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
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>

#include "garglk.h"
#include "kindle.h"

using Catalog = std::unordered_map<std::string, std::string>;

// The quoted string of a .po line, with its escapes resolved.
static std::string po_string(const std::string &line)
{
    std::string out;
    auto start = line.find('"');

    if (start == std::string::npos) {
        return out;
    }

    for (auto i = start + 1; i < line.size() && line[i] != '"'; i++) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            char c = line[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            out += line[i];
        }
    }

    return out;
}

// Only the subset of the .po syntax produced by msgmerge is needed:
// msgid/msgstr pairs with continuation lines; comments, contexts and
// plural forms are skipped.
static bool load(const std::string &path, Catalog &catalog)
{
    std::ifstream file(path);
    std::string line, msgid, msgstr;
    std::string *field = nullptr;

    if (!file.is_open()) {
        return false;
    }

    auto add = [&] {
        if (!msgid.empty() && !msgstr.empty()) {
            catalog.emplace(msgid, msgstr);
        }
        msgid.clear();
        msgstr.clear();
    };

    while (std::getline(file, line)) {
        if (line.rfind("msgid ", 0) == 0) {
            add();
            field = &msgid;
        } else if (line.rfind("msgstr ", 0) == 0) {
            field = &msgstr;
        } else if (line.empty() || line[0] != '"') {
            field = nullptr;
        }

        if (field != nullptr) {
            *field += po_string(line);
        }
    }
    add();

    return true;
}

// The language of the interface, as a POSIX locale name such as es_MX:
// from the environment, else from the language of the Kindle keyboard.
static std::optional<std::string> language()
{
    for (const char *variable : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        std::string value = std::getenv(variable) != nullptr ? std::getenv(variable) : "";
        if (!value.empty() && value != "C" && value != "POSIX") {
            return value.substr(0, value.find_first_of(":.@"));
        }
    }

    auto keyboard = kindle::keyboard_language();
    if (!keyboard.has_value()) {
        return std::nullopt;
    }

    auto value = keyboard->substr(0, keyboard->find_first_of(".@"));
    std::replace(value.begin(), value.end(), '-', '_');
    return value;
}

static Catalog load_catalog()
{
    Catalog catalog;
    auto lang = language();
    auto dir = garglk::winappdir();

    if (lang.has_value() && dir.has_value()) {
        auto base = *dir + "/l10n/";
        if (!load(base + *lang + ".po", catalog)) {
            load(base + lang->substr(0, lang->find('_')) + ".po", catalog);
        }
    }

    return catalog;
}

const char *kindle::tr(const char *msgid)
{
    static const Catalog catalog = load_catalog();
    auto translation = catalog.find(msgid);

    return translation != catalog.end() ? translation->second.c_str() : msgid;
}
