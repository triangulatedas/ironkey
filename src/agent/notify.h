// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>

namespace ironkey {

// Shows a desktop notification (org.freedesktop.Notifications on the
// session bus). Failures are logged and otherwise ignored.
void notify(const std::string& summary, const std::string& body, bool critical = false);

}  // namespace ironkey
