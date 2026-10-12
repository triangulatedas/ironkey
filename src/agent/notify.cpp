// SPDX-License-Identifier: GPL-2.0-only
#include "agent/notify.h"

#include <systemd/sd-bus.h>

#include <cstdio>
#include <cstring>

namespace ironkey {

void notify(const std::string& summary, const std::string& body, bool critical)
{
    sd_bus* bus = nullptr;
    int rc = sd_bus_open_user(&bus);
    if (rc < 0) {
        std::fprintf(stderr, "notify: cannot connect to session bus: %s\n", std::strerror(-rc));
        return;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    // Notify(app_name, replaces_id, icon, summary, body, actions, hints, timeout)
    rc = sd_bus_call_method(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                            "org.freedesktop.Notifications", "Notify", &error, &reply,
                            "susssasa{sv}i", "IronKey", 0u, "drive-removable-media",
                            summary.c_str(), body.c_str(),
                            0,                                         // no actions
                            1, "urgency", "y", critical ? 2 : 1,       // hints
                            -1);                                       // default timeout
    if (rc < 0)
        std::fprintf(stderr, "notify: %s\n", error.message ? error.message : std::strerror(-rc));

    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_flush_close_unref(bus);
}

}  // namespace ironkey
