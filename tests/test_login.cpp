// SPDX-License-Identifier: GPL-2.0-only
// Account choice and wording for the different password setups.
#include "check.h"
#include "linux/device_ops.h"

using namespace ironkey;

namespace {

DriveStatus status_of(PasswordSetup setup, int admin_failed = 0)
{
    DriveStatus s;
    s.setup = setup;
    s.admin_failed_attempts = admin_failed;
    return s;
}

}  // namespace

TEST(admin_and_user_drive_defaults_to_user_login)
{
    Login login = choose_login(status_of(PasswordSetup::AdminAndUser), std::nullopt);
    CHECK(login.account == Account::User);
    CHECK_EQ(login.password_name(), std::string("user password"));
    // The user counter's location is unknown, so no count is claimed.
    CHECK(!login.failed_attempts(status_of(PasswordSetup::AdminAndUser, 3)));
}

TEST(admin_can_be_requested_on_admin_and_user_drive)
{
    Login login = choose_login(status_of(PasswordSetup::AdminAndUser, 2), Account::Admin);
    CHECK(login.account == Account::Admin);
    CHECK_EQ(login.password_name(), std::string("admin password"));
    CHECK_EQ(*login.failed_attempts(status_of(PasswordSetup::AdminAndUser, 2)), 2);
    CHECK(login.failed_warning(2).find("8 left before the drive erases itself") !=
          std::string::npos);
}

TEST(single_password_drive_uses_index_0_and_plain_wording)
{
    Login login = choose_login(status_of(PasswordSetup::Single, 1), std::nullopt);
    CHECK(login.account == Account::Admin);
    CHECK_EQ(login.password_name(), std::string("password"));
    CHECK(login.lockout_consequence().find("erases itself") != std::string::npos);
    CHECK_EQ(login.failed_warning(1).substr(0, 15), std::string("1 failed login "));
}

TEST(user_login_refused_on_single_password_drive)
{
    CHECK_THROWS_AS(choose_login(status_of(PasswordSetup::Single), Account::User), IronKeyError);
}

TEST(unknown_setup_falls_back_to_index_0)
{
    Login login = choose_login(DriveStatus{}, std::nullopt);
    CHECK(login.account == Account::Admin);
}
