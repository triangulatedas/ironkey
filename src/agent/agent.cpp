// SPDX-License-Identifier: GPL-2.0-only
#include "agent/agent.h"

#include <cstdio>

#include "agent/notify.h"
#include "linux/device_ops.h"
#include "linux/usb_topology.h"
#include "proto/session.h"

namespace ironkey {

namespace {

void log(const std::string& port, const std::string& msg)
{
    std::fprintf(stderr, "[%s] %s\n", port.c_str(), msg.c_str());
}

bool looks_like_ironkey(uint16_t vid, const std::string& product)
{
    UsbDevice d;
    d.vid = vid;
    d.product = product;
    return d.is_ironkey();
}

// Sleeps up to `ms`, returning early (false) if cancelled.
bool sleep_unless_cancelled(const std::atomic<bool>& cancelled, int ms)
{
    for (int waited = 0; waited < ms; waited += 100) {
        if (cancelled)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !cancelled;
}

std::string disk_list(const UsbDevice& dev)
{
    std::string s;
    for (const auto& d : dev.disks)
        if (d.size_bytes > 0)
            s += (s.empty() ? "" : ", ") + d.node;
    return s;
}

}  // namespace

Agent::~Agent()
{
    shutdown();
}

void Agent::scan()
{
    for (const auto& dev : find_kingston_devices())
        if (dev.is_ironkey() && !dev.is_unlocked())
            start(dev.port);
}

void Agent::on_event(const UsbEvent& ev)
{
    if (ev.action == UsbEvent::Action::Add) {
        reap();
        if (ev.vid == kKingstonVid && looks_like_ironkey(ev.vid, ev.product) &&
            !workers_.contains(ev.port))
            start(ev.port);
        return;
    }

    if (ev.action != UsbEvent::Action::Remove)
        return;
    auto it = workers_.find(ev.port);
    if (it == workers_.end())
        return;

    State s = it->second->state;
    // The device drops off the bus by design while switching to HID mode
    // and possibly while unlocking; the worker handles a real unplug itself.
    if (s == State::Switching || s == State::Unlocking)
        return;
    log(ev.port, s == State::Prompting ? "IronKey removed; closing password dialog"
                                       : "IronKey removed");
    stop(ev.port);
}

void Agent::reap()
{
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->second->state == State::Gone) {
            it->second->thread.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

void Agent::shutdown()
{
    while (!workers_.empty())
        stop(workers_.begin()->first);
}

void Agent::start(const std::string& port)
{
    log(port, "IronKey connected");
    auto w = std::make_unique<Worker>();
    Worker& ref = *w;
    w->thread = std::thread(
        [&ref, port, dry_run = dry_run_, account = account_] { run(ref, port, dry_run, account); });
    workers_.emplace(port, std::move(w));
}

void Agent::stop(const std::string& port)
{
    auto it = workers_.find(port);
    if (it == workers_.end())
        return;
    it->second->cancelled = true;
    it->second->dialog.cancel();
    it->second->thread.join();
    workers_.erase(it);
}

void Agent::run(Worker& w, const std::string& port, DryRun dry_run, Account account)
{
    auto worker_log = [&port](const std::string& msg) { log(port, msg); };
    auto finish = [&w](State s) { w.state = s; };

    // Give udisks and the desktop a moment to mount the device's CD-ROM,
    // so we release it after rather than racing with the automount.
    if (!sleep_unless_cancelled(w.cancelled, 2000))
        return finish(State::Gone);

    UsbDevice dev;
    dev.port = port;
    if (!refresh(dev))
        return finish(State::Gone);
    if (!dev.is_ironkey())
        return finish(State::Done);
    if (dev.is_unlocked()) {
        log(port, "Already unlocked");
        return finish(State::Done);
    }

    try {
        check_access(dev);
        if (PasswordDialog::program().empty())
            throw IronKeyError("No password dialog available. Install zenity or kdialog.");

        auto prompt = [&dev, account](const std::string& note) {
            return (note.empty() ? "" : note + "\n\n") + "Enter the " + to_string(account) +
                   " password for " + describe(dev) + ", or cancel to leave it locked.";
        };

        // Switch to HID mode before asking, so the drive's own failed-login
        // count can be shown. A locked drive in HID mode is harmless.
        std::string note;
        if (dry_run == DryRun::Off) {
            w.state = State::Switching;
            ensure_hid_mode(dev, worker_log);
            if (auto failed = read_failed_attempts(dev, account); failed && *failed > 0) {
                log(port, failed_attempts_warning(*failed, account));
                note = "Warning: " + failed_attempts_warning(*failed, account) + ".";
            }
        }
        std::string message = prompt(note);

        for (;;) {
            w.state = State::Prompting;
            auto password = w.dialog.ask("Unlock IronKey", message);
            if (w.cancelled)
                return finish(State::Gone);
            if (!password) {
                log(port, "Password prompt cancelled; leaving the drive locked");
                return finish(State::Done);
            }

            if (password->empty() || password->size() > kMaxPasswordBytes) {
                secure_wipe(password->data(), password->capacity());
                message = prompt("The password must be 1 to 16 characters (passphrase mode is "
                                 "not supported yet). Nothing was sent to the drive.");
                continue;
            }

            try {
                if (dry_run != DryRun::Off) {
                    log(port, "DRY RUN: password entered (" + std::to_string(password->size()) +
                                  " bytes); nothing sent to the drive");
                    // Goes through the same handler as a real failed unlock.
                    if (dry_run == DryRun::Fail)
                        throw NotUnlockedError("DRY RUN: simulated wrong password", true);
                    secure_wipe(password->data(), password->capacity());
                    notify("IronKey dry run", "Password entered for " + describe(dev) +
                                                  ". Nothing was sent to the drive.");
                    return finish(State::Done);
                }

                w.state = State::Switching;
                ensure_hid_mode(dev, worker_log);
                w.state = State::Unlocking;
                unlock_device(dev, *password, account, worker_log);
                secure_wipe(password->data(), password->capacity());
            } catch (const NotUnlockedError& e) {
                secure_wipe(password->data(), password->capacity());
                log(port, e.what());
                message = prompt(std::string(e.what()) + ".");
                continue;
            } catch (...) {
                secure_wipe(password->data(), password->capacity());
                throw;
            }

            log(port, "Unlocked: " + disk_list(dev));
            notify("IronKey unlocked",
                   describe(dev) + " is available as " + disk_list(dev) +
                       ".\nTo lock it, eject it in Files or run: ironkey-unlock lock");
            return finish(State::Done);
        }
    } catch (const DeviceGoneError&) {
        log(port, "Device removed");
        return finish(State::Gone);
    } catch (const std::exception& e) {
        if (!refresh(dev)) {
            log(port, "Device removed");
            return finish(State::Gone);
        }
        log(port, std::string("Error: ") + e.what());
        notify("IronKey unlock failed", e.what(), true);
        return finish(State::Done);
    }
}

}  // namespace ironkey
