// SPDX-License-Identifier: GPL-2.0-only
#include "agent/tray.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace ironkey {

namespace {

constexpr char kSniPath[] = "/StatusNotifierItem";
constexpr char kSniIface[] = "org.kde.StatusNotifierItem";
constexpr char kMenuPath[] = "/MenuBar";
constexpr char kMenuIface[] = "com.canonical.dbusmenu";

// Append one "{sv}" dict entry with a string value.
void prop_s(sd_bus_message* m, const char* key, const char* val)
{
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_open_container(m, 'v', "s");
    sd_bus_message_append(m, "s", val);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}

// Append one "{sv}" dict entry with a boolean value.
void prop_b(sd_bus_message* m, const char* key, int val)
{
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_open_container(m, 'v', "b");
    sd_bus_message_append(m, "b", val);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}

}  // namespace

// --- dbusmenu property dicts, shared by GetLayout and GetGroupProperties -----

void Tray::append_props_dict(sd_bus_message* m, const Tray::Item& it)
{
    sd_bus_message_open_container(m, 'a', "{sv}");
    if (it.kind == Tray::Kind::Separator) {
        prop_s(m, "type", "separator");
    } else {
        if (it.kind == Tray::Kind::Submenu)
            prop_s(m, "children-display", "submenu");
        if (!it.label.empty())
            prop_s(m, "label", it.label.c_str());
        prop_b(m, "enabled", it.enabled ? 1 : 0);
        prop_b(m, "visible", 1);
    }
    sd_bus_message_close_container(m);
}

// Append a full "(ia{sv}av)" layout struct, recursing into children.
void Tray::append_item_struct(sd_bus_message* m, const Tray::Item& it)
{
    sd_bus_message_open_container(m, 'r', "ia{sv}av");
    sd_bus_message_append(m, "i", it.id);
    append_props_dict(m, it);
    sd_bus_message_open_container(m, 'a', "v");
    for (const auto& c : it.children) {
        sd_bus_message_open_container(m, 'v', "(ia{sv}av)");
        append_item_struct(m, c);
        sd_bus_message_close_container(m);
    }
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}

Tray::Item Tray::build_root() const
{
    Item root;
    root.id = 0;
    root.kind = Kind::Submenu;
    root.children = items_;
    return root;
}

const Tray::Item* Tray::find_in(const Item& node, int id)
{
    if (node.id == id)
        return &node;
    for (const auto& c : node.children)
        if (const Item* f = find_in(c, id))
            return f;
    return nullptr;
}

// --- construction ------------------------------------------------------------

Tray::Tray(Callbacks cb) : cb_(std::move(cb)) {}

Tray::~Tray()
{
    if (sni_slot_)
        sd_bus_slot_unref(sni_slot_);
    if (menu_slot_)
        sd_bus_slot_unref(menu_slot_);
    if (bus_)
        sd_bus_flush_close_unref(bus_);
}

bool Tray::start()
{
    int r = sd_bus_open_user(&bus_);
    if (r < 0) {
        std::fprintf(stderr, "tray: no session bus (%s); running headless\n", std::strerror(-r));
        bus_ = nullptr;
        return false;
    }
    bus_name_ = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    r = sd_bus_request_name(bus_, bus_name_.c_str(), 0);
    if (r < 0) {
        std::fprintf(stderr, "tray: cannot take bus name (%s); running headless\n",
                     std::strerror(-r));
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
        return false;
    }
    r = sd_bus_add_object_vtable(bus_, &sni_slot_, kSniPath, kSniIface, sni_vtable_, this);
    if (r >= 0)
        r = sd_bus_add_object_vtable(bus_, &menu_slot_, kMenuPath, kMenuIface, menu_vtable_, this);
    if (r < 0) {
        std::fprintf(stderr, "tray: cannot export objects (%s); running headless\n",
                     std::strerror(-r));
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
        return false;
    }
    rebuild_items();

    // Register with the desktop's StatusNotifierWatcher. Best-effort: without a
    // watcher the icon just will not show, but the agent keeps working.
    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(bus_, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                           "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem", &err,
                           nullptr, "s", bus_name_.c_str());
    if (r < 0)
        std::fprintf(stderr, "tray: no StatusNotifierWatcher (%s); the icon may not appear "
                             "(on GNOME, enable the AppIndicator extension)\n",
                     err.message ? err.message : std::strerror(-r));
    else
        std::fprintf(stderr, "tray: registered with the StatusNotifierWatcher\n");
    sd_bus_error_free(&err);
    return true;
}

// --- poll-loop integration ---------------------------------------------------

int Tray::fd() const { return bus_ ? sd_bus_get_fd(bus_) : -1; }

int Tray::events() const { return bus_ ? sd_bus_get_events(bus_) : 0; }

uint64_t Tray::timeout_us() const
{
    uint64_t t = UINT64_MAX;
    if (bus_)
        sd_bus_get_timeout(bus_, &t);
    return t;
}

void Tray::process()
{
    if (!bus_)
        return;
    while (sd_bus_process(bus_, nullptr) > 0) {
    }
}

void Tray::notify_changed()
{
    if (!bus_)
        return;
    rebuild_items();
    ++menu_revision_;
    sd_bus_emit_signal(bus_, kMenuPath, kMenuIface, "LayoutUpdated", "ui", menu_revision_, 0);
    sd_bus_emit_signal(bus_, kSniPath, kSniIface, "NewToolTip", "");
    sd_bus_emit_signal(bus_, kSniPath, kSniIface, "NewStatus", "s", "Active");
}

// --- menu model --------------------------------------------------------------

void Tray::rebuild_items()
{
    items_.clear();
    std::vector<Drive> drives = cb_.list ? cb_.list() : std::vector<Drive>{};
    int id = 100;
    for (const auto& d : drives) {
        Item sub;
        sub.id = id++;
        sub.kind = Kind::Submenu;
        sub.label = d.label;

        if (!d.status.empty()) {
            Item status;
            status.id = id++;
            status.kind = Kind::Action;
            status.label = d.status;
            status.action = Act::None;
            status.enabled = false;  // informational line
            sub.children.push_back(std::move(status));
            sub.children.push_back({id++, Kind::Separator, "", "", Act::None, true, {}});
        }

        Item primary;
        primary.id = id++;
        primary.kind = Kind::Action;
        primary.port = d.port;
        if (d.unlocked) {
            primary.label = "Lock && eject";  // "&&" renders as one "&"
            primary.action = Act::Lock;
        } else {
            primary.label = "Unlock…";
            primary.action = Act::Unlock;
        }
        sub.children.push_back(std::move(primary));

        if (!d.unlocked) {
            Item hint;
            hint.id = id++;
            hint.kind = Kind::Action;
            hint.port = d.port;
            hint.label = "Show password hint";
            hint.action = Act::ShowHint;
            sub.children.push_back(std::move(hint));

            sub.children.push_back({id++, Kind::Separator, "", "", Act::None, true, {}});
            Item setup;
            setup.id = id++;
            setup.kind = Kind::Action;
            setup.port = d.port;
            setup.label = "Reset / Set up…";
            setup.action = Act::Setup;
            sub.children.push_back(std::move(setup));
        }

        items_.push_back(std::move(sub));
    }
    if (drives.empty()) {
        Item none{id++, Kind::Action, "No IronKey drives connected", "", Act::None, false, {}};
        items_.push_back(std::move(none));
    }
    items_.push_back({90, Kind::Separator, "", "", Act::None, true, {}});
    items_.push_back({1, Kind::Action, "Quit", "", Act::Quit, true, {}});
}

const Tray::Item* Tray::find_item(int id) const
{
    for (const auto& it : items_)
        if (const Item* f = find_in(it, id))
            return f;
    return nullptr;
}

// --- StatusNotifierItem interface -------------------------------------------

int Tray::sni_get_property(sd_bus*, const char*, const char*, const char* prop,
                           sd_bus_message* reply, void* userdata, sd_bus_error*)
{
    auto* self = static_cast<Tray*>(userdata);
    if (std::strcmp(prop, "Category") == 0)
        return sd_bus_message_append(reply, "s", "Hardware");
    if (std::strcmp(prop, "Id") == 0)
        return sd_bus_message_append(reply, "s", "ironkey");
    if (std::strcmp(prop, "Title") == 0)
        return sd_bus_message_append(reply, "s", "IronKey");
    if (std::strcmp(prop, "Status") == 0)
        return sd_bus_message_append(reply, "s", "Active");
    if (std::strcmp(prop, "IconName") == 0)
        return sd_bus_message_append(reply, "s", "drive-removable-media");
    if (std::strcmp(prop, "IconThemePath") == 0)
        return sd_bus_message_append(reply, "s", "");
    if (std::strcmp(prop, "ItemIsMenu") == 0)
        return sd_bus_message_append(reply, "b", 1);
    if (std::strcmp(prop, "Menu") == 0)
        return sd_bus_message_append(reply, "o", kMenuPath);
    if (std::strcmp(prop, "ToolTip") == 0) {
        // (icon-name, icon-pixmaps, title, text)
        std::vector<Drive> drives = self->cb_.list ? self->cb_.list() : std::vector<Drive>{};
        std::string text;
        if (drives.empty()) {
            text = "No IronKey drives connected";
        } else {
            int locked = 0, unlocked = 0;
            for (const auto& d : drives)
                (d.unlocked ? unlocked : locked)++;
            text = std::to_string(drives.size()) + (drives.size() == 1 ? " drive" : " drives");
            if (locked)
                text += ", " + std::to_string(locked) + " locked";
            if (unlocked)
                text += ", " + std::to_string(unlocked) + " unlocked";
        }
        sd_bus_message_open_container(reply, 'r', "sa(iiay)ss");
        sd_bus_message_append(reply, "s", "drive-removable-media");
        sd_bus_message_open_container(reply, 'a', "(iiay)");
        sd_bus_message_close_container(reply);
        sd_bus_message_append(reply, "ss", "IronKey", text.c_str());
        return sd_bus_message_close_container(reply);
    }
    return -ENOENT;
}

int Tray::sni_method_noop(sd_bus_message* m, void*, sd_bus_error*)
{
    return sd_bus_reply_method_return(m, "");
}

// --- com.canonical.dbusmenu interface ---------------------------------------

int Tray::menu_get_property(sd_bus*, const char*, const char*, const char* prop,
                            sd_bus_message* reply, void* userdata, sd_bus_error*)
{
    auto* self = static_cast<Tray*>(userdata);
    if (std::strcmp(prop, "Version") == 0)
        return sd_bus_message_append(reply, "u", 3u);
    if (std::strcmp(prop, "Status") == 0)
        return sd_bus_message_append(reply, "s", "normal");
    if (std::strcmp(prop, "TextDirection") == 0)
        return sd_bus_message_append(reply, "s", "ltr");
    if (std::strcmp(prop, "IconThemePath") == 0)
        return sd_bus_message_append_strv(reply, nullptr);
    (void)self;
    return -ENOENT;
}

int Tray::menu_get_layout(sd_bus_message* m, void* userdata, sd_bus_error*)
{
    auto* self = static_cast<Tray*>(userdata);
    int parent = 0, depth = 0;
    sd_bus_message_read(m, "ii", &parent, &depth);
    sd_bus_message_skip(m, "as");
    // items_ were refreshed by AboutToShow (hosts call it before showing) or at
    // start()/hotplug; avoid another device read here.

    sd_bus_message* reply = nullptr;
    sd_bus_message_new_method_return(m, &reply);
    sd_bus_message_append(reply, "u", self->menu_revision_);
    Item root = self->build_root();
    const Item* target = (parent == 0) ? &root : find_in(root, parent);
    if (target) {
        append_item_struct(reply, *target);
    } else {
        Item empty{parent, Kind::Action, "", "", Act::None, true, {}};
        append_item_struct(reply, empty);
    }
    sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return 1;
}

int Tray::menu_get_group_properties(sd_bus_message* m, void* userdata, sd_bus_error*)
{
    auto* self = static_cast<Tray*>(userdata);

    // Requested ids (empty => all).
    std::vector<int> ids;
    sd_bus_message_enter_container(m, 'a', "i");
    for (int v; sd_bus_message_read(m, "i", &v) > 0;)
        ids.push_back(v);
    sd_bus_message_exit_container(m);
    sd_bus_message_skip(m, "as");

    auto wanted = [&](int id) {
        if (ids.empty())
            return true;
        for (int v : ids)
            if (v == id)
                return true;
        return false;
    };

    // Flatten the tree (root + all descendants) so any requested id is found.
    std::vector<const Item*> all;
    Item root = self->build_root();
    std::function<void(const Item&)> collect = [&](const Item& n) {
        all.push_back(&n);
        for (const auto& c : n.children)
            collect(c);
    };
    collect(root);

    sd_bus_message* reply = nullptr;
    sd_bus_message_new_method_return(m, &reply);
    sd_bus_message_open_container(reply, 'a', "(ia{sv})");
    for (const Item* it : all) {
        if (!wanted(it->id))
            continue;
        sd_bus_message_open_container(reply, 'r', "ia{sv}");
        sd_bus_message_append(reply, "i", it->id);
        append_props_dict(reply, *it);
        sd_bus_message_close_container(reply);
    }
    sd_bus_message_close_container(reply);
    sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return 1;
}

int Tray::menu_get_property_method(sd_bus_message* m, void* userdata, sd_bus_error* err)
{
    auto* self = static_cast<Tray*>(userdata);
    int id = 0;
    const char* name = nullptr;
    sd_bus_message_read(m, "is", &id, &name);
    const Item* it = self->find_item(id);
    if (!it)
        return sd_bus_error_set(err, SD_BUS_ERROR_INVALID_ARGS, "no such menu item"), -EINVAL;
    sd_bus_message* reply = nullptr;
    sd_bus_message_new_method_return(m, &reply);
    if (name && std::strcmp(name, "label") == 0)
        sd_bus_message_append(reply, "v", "s", it->label.c_str());
    else if (name && std::strcmp(name, "enabled") == 0)
        sd_bus_message_append(reply, "v", "b", it->enabled ? 1 : 0);
    else if (name && std::strcmp(name, "type") == 0)
        sd_bus_message_append(reply, "v", "s", it->kind == Kind::Separator ? "separator" : "");
    else
        sd_bus_message_append(reply, "v", "s", "");
    sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return 1;
}

int Tray::menu_event(sd_bus_message* m, void* userdata, sd_bus_error*)
{
    auto* self = static_cast<Tray*>(userdata);
    int id = 0;
    const char* event = nullptr;
    sd_bus_message_read(m, "is", &id, &event);
    sd_bus_message_skip(m, "v");  // data
    // timestamp (u) left unread; reply does not depend on it.

    if (event && std::strcmp(event, "clicked") == 0) {
        if (const Item* it = self->find_item(id); it && it->enabled) {
            switch (it->action) {
            case Act::Unlock:
                if (self->cb_.unlock)
                    self->cb_.unlock(it->port);
                break;
            case Act::Lock:
                if (self->cb_.lock)
                    self->cb_.lock(it->port);
                break;
            case Act::ShowHint:
                if (self->cb_.show_hint)
                    self->cb_.show_hint(it->port);
                break;
            case Act::Setup:
                if (self->cb_.setup)
                    self->cb_.setup(it->port);
                break;
            case Act::Quit:
                if (self->cb_.quit)
                    self->cb_.quit();
                break;
            case Act::None:
                break;
            }
        }
    }
    return sd_bus_reply_method_return(m, "");
}

int Tray::menu_about_to_show(sd_bus_message* m, void* userdata, sd_bus_error*)
{
    auto* self = static_cast<Tray*>(userdata);
    self->rebuild_items();  // so the following GetLayout is fresh
    return sd_bus_reply_method_return(m, "b", 1);  // needUpdate => re-fetch layout
}

// --- vtables (out-of-line, so they may name the private trampolines) ---------

const sd_bus_vtable Tray::sni_vtable_[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Category", "s", sni_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Id", "s", sni_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Title", "s", sni_get_property, 0, 0),
    SD_BUS_PROPERTY("Status", "s", sni_get_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IconName", "s", sni_get_property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IconThemePath", "s", sni_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ItemIsMenu", "b", sni_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Menu", "o", sni_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", sni_get_property, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("Activate", "ii", "", sni_method_noop, 0),
    SD_BUS_METHOD("SecondaryActivate", "ii", "", sni_method_noop, 0),
    SD_BUS_METHOD("ContextMenu", "ii", "", sni_method_noop, 0),
    SD_BUS_METHOD("Scroll", "is", "", sni_method_noop, 0),
    SD_BUS_SIGNAL("NewIcon", "", 0),
    SD_BUS_SIGNAL("NewStatus", "s", 0),
    SD_BUS_SIGNAL("NewToolTip", "", 0),
    SD_BUS_SIGNAL("NewTitle", "", 0),
    SD_BUS_VTABLE_END,
};

const sd_bus_vtable Tray::menu_vtable_[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "u", menu_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("TextDirection", "s", menu_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", menu_get_property, 0, 0),
    SD_BUS_PROPERTY("IconThemePath", "as", menu_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", menu_get_layout, 0),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})", menu_get_group_properties, 0),
    SD_BUS_METHOD("GetProperty", "is", "v", menu_get_property_method, 0),
    SD_BUS_METHOD("Event", "isvu", "", menu_event, 0),
    SD_BUS_METHOD("AboutToShow", "i", "b", menu_about_to_show, 0),
    SD_BUS_SIGNAL("ItemsPropertiesUpdated", "a(ia{sv})a(ias)", 0),
    SD_BUS_SIGNAL("LayoutUpdated", "ui", 0),
    SD_BUS_SIGNAL("ItemActivationRequested", "iu", 0),
    SD_BUS_VTABLE_END,
};

}  // namespace ironkey
