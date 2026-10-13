// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <systemd/sd-bus.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ironkey {

// A system-tray icon for the agent, implemented directly as a
// org.kde.StatusNotifierItem plus a com.canonical.dbusmenu menu over sd-bus
// (no extra toolkit dependency). Works on KDE and on GNOME with the common
// AppIndicator extension. The agent drives it from the main thread; the tray
// shares the agent's poll() loop through fd()/events()/process().
//
// Phase 2 (the shell): the icon appears with a tooltip, and a menu lists each
// connected drive with an Unlock or Lock action, plus Quit. The menu is rebuilt
// from the callbacks each time it is opened.
class Tray {
public:
    // One connected drive as the menu needs to show it.
    struct Drive {
        std::string port;   // USB port path, the stable id
        std::string label;  // human description (owner name / model / serial)
        bool unlocked = false;
        std::string status; // one-line status for a disabled menu entry, or empty
    };

    struct Callbacks {
        std::function<std::vector<Drive>()> list;  // current drives, for the menu
        std::function<void(std::string port)> unlock;     // user picked Unlock
        std::function<void(std::string port)> lock;       // user picked Lock & eject
        std::function<void(std::string port)> show_hint;  // user picked Show hint
        std::function<void()> quit;                        // user picked Quit
    };

    explicit Tray(Callbacks cb);
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // Connects to the session bus, exports the SNI + menu objects, and registers
    // with the StatusNotifierWatcher. Returns false (and logs) if the bus or the
    // watcher is unavailable; the agent then runs headless as before.
    bool start();

    // Poll-loop integration. Valid only after a successful start().
    int fd() const;
    int events() const;      // POLLIN/POLLOUT mask sd-bus currently wants
    uint64_t timeout_us() const;  // sd-bus's own timeout, UINT64_MAX for none
    void process();          // drain pending bus messages (call after poll wakes)

    // Tell the host the drive list changed, so the menu and tooltip refresh.
    void notify_changed();

    bool active() const { return bus_ != nullptr; }

private:
    // dbusmenu item model (a shallow tree), rebuilt from cb_.list() on each
    // layout request. A Submenu item carries children; the root is a Submenu.
    enum class Kind { Action, Separator, Submenu };
    enum class Act { Unlock, Lock, ShowHint, Quit, None };
    struct Item {
        int id;
        Kind kind;
        std::string label;
        std::string port;      // for per-drive action items
        Act action = Act::None;
        bool enabled = true;
        std::vector<Item> children;  // for Submenu items
    };
    void rebuild_items();
    const Item* find_item(int id) const;
    static void append_props_dict(sd_bus_message* m, const Item& it);
    static void append_item_struct(sd_bus_message* m, const Item& it);
    Item build_root() const;  // id-0 Submenu whose children are items_
    static const Item* find_in(const Item& node, int id);

    // sd-bus method/property trampolines (static; userdata = this).
    static int sni_get_property(sd_bus* bus, const char* path, const char* iface,
                                const char* prop, sd_bus_message* reply, void* userdata,
                                sd_bus_error* err);
    static int sni_method_noop(sd_bus_message* m, void* userdata, sd_bus_error* err);

    static int menu_get_property(sd_bus* bus, const char* path, const char* iface,
                                 const char* prop, sd_bus_message* reply, void* userdata,
                                 sd_bus_error* err);
    static int menu_get_layout(sd_bus_message* m, void* userdata, sd_bus_error* err);
    static int menu_get_group_properties(sd_bus_message* m, void* userdata, sd_bus_error* err);
    static int menu_get_property_method(sd_bus_message* m, void* userdata, sd_bus_error* err);
    static int menu_event(sd_bus_message* m, void* userdata, sd_bus_error* err);
    static int menu_about_to_show(sd_bus_message* m, void* userdata, sd_bus_error* err);

    // vtables (defined out-of-line, so they may name the private trampolines).
    static const sd_bus_vtable sni_vtable_[];
    static const sd_bus_vtable menu_vtable_[];

    Callbacks cb_;
    sd_bus* bus_ = nullptr;
    sd_bus_slot* sni_slot_ = nullptr;
    sd_bus_slot* menu_slot_ = nullptr;
    std::string bus_name_;        // our unique SNI well-known name
    uint32_t menu_revision_ = 1;  // bumped on every layout change
    std::vector<Item> items_;
};

}  // namespace ironkey
