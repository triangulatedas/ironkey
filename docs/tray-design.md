# Tray icon + setup wizard — design

Status: proposal (not yet implemented). Tracks the last roadmap item.

## Goals (from the request)

- A **system-tray icon** with a menu of useful commands.
- A **setup / initialize wizard** in the agent.
- **Choose admin or user** when logging in.
- Show the **failed-login count**.
- Show the **password hint only on explicit click** (never automatically).

## Decisions

| Question | Decision |
|---|---|
| Who owns the tray? | `ironkey-agent` (it is already the session daemon). No second process. |
| Separate UI binary? | No. Menu over D-Bus; dialogs stay external (`zenity`/`kdialog`). |
| New shared library? | No. Reuse `ironkey_linux` + `ironkey_proto`. Extract leftover CLI orchestration into `device_ops`. |
| Tray technology | **StatusNotifierItem + com.canonical.dbusmenu over sd-bus** (no new deps). Alternative below. |
| Wizard UI | Sequenced `zenity`/`kdialog` dialogs (`--forms`, `--list`, `--password`). |

### Tray technology trade-off

- **Recommended — raw SNI over sd-bus.** The agent already links `libsystemd`
  and has a `poll()` loop. We expose `org.kde.StatusNotifierItem` (icon,
  status, tooltip, a menu object path) and `com.canonical.dbusmenu` (layout +
  events), and register with the desktop's StatusNotifierWatcher. Zero new
  dependencies; works on KDE, and on GNOME with the common AppIndicator
  extension. Cost: a few hundred lines of sd-bus vtable code.
- **Alternative — libayatana-appindicator3.** Much less code (it implements SNI
  + DBusMenu for you), but adds `libayatana-appindicator3` **and GTK3** as
  runtime deps, against the project's lean-deps ethos. Reasonable if we decide
  the SNI boilerplate isn't worth it.

Either way the **dialogs/wizard are external** (`zenity`/`kdialog`), so no GUI
toolkit is linked into the agent in the recommended path.

## Refactor first (extract from the CLI)

Everything below the UI already exists in the libs (`unlock_device`,
`reset_device_full`, `format_data_disk`, `read_profile_device`,
`read_drive_status`, `choose_login`, `lock_device`, `change_password_device`).
Move the remaining CLI-only glue into `ironkey_linux` so the tray calls the same
code as `ironkey-unlock`:

- `find_scsi_helper()` and `scsi_usable(dev)` → `device_ops`.
- New `reset_device_auto(dev, password, profile, prefer, log)` encapsulating the
  transport choice (SCSI-when-usable else HID), the SCSI reset + `FF 01`
  re-enumerate, and the wait-for-reattach. `cmd_reset` becomes a thin caller.
- New `reenumerate(dev, log)` (the `FF 01` soft power-cycle) for reuse.
- Optional: a small `Operations` facade that takes callbacks
  (`ask_password`, `confirm`, `progress`) so the CLI (terminal prompts) and the
  agent (zenity/notifications) drive identical flows.

No behaviour change for the CLI; the agent gains the same operations.

## Tray menu (state-aware, rebuilt on open)

Top level: an IronKey icon; tooltip shows how many drives and their state.
Per connected drive a submenu titled from `describe(dev)`:

- **Locked drive**
  - `Unlock…` → password dialog (with admin/user choice when the drive has
    both accounts). Shows the failed-login warning inline.
  - `Show password hint` → reads the profile and shows the hint (on click only).
  - `Owner info…` → name / company / description (read-only).
  - `Reset / Set up…` → the wizard.
  - Status line (disabled item): `N failed logins — M left` (admin account).
- **Unlocked drive**
  - `Open` → opens the mounted volume in the file manager.
  - `Lock & eject`.
  - `Change password…`.
  - `Format…` → fs type (vfat/exfat/ext4) + label.
- Global: `Settings` (dry-run toggle, force HID/SCSI), `About`, `Quit`.

Items are enabled/disabled by state; the menu is cheap to rebuild from the
device list the agent already tracks.

## Reading status & hints without disruption

`read_drive_status` and `read_profile` need either HID mode (disruptive — it
releases volumes) or the SCSI path. For a *locked drive in storage mode* the
**SCSI transport reads `FF A0` (failed count, setup) and the profile (hint)
without switching to HID** — ideal for a passive tray. So:

- If the SCSI helper is available → read status/hint over SG_IO (non-disruptive,
  can even refresh periodically for the failed count).
- Else → read only when the user initiates an action that already uses HID, and
  otherwise show "unknown". Never switch to HID just to populate the menu.

## Setup / initialize wizard (zenity sequence)

Entry points: the `Reset / Set up…` menu item, and an auto-prompt the agent
raises when it sees a **factory/blank** drive (optional, like the current
unlock prompt).

Steps:
1. **Confirm erase** (skip if the drive is already blank): big warning + type
   ERASE / click-through.
2. **Accounts**: single password, or admin + user. *(see Gaps — user account
   provisioning isn't implemented yet.)*
3. **Password(s)**: complex (≤16) or passphrase (≤64); confirm field; show the
   untrusted-host warning.
4. **Owner info** (optional): name, company, description, admin/user hint —
   one `zenity --forms`.
5. **Run**: `reset_device_auto(...)` with the profile. Progress via
   notifications; auto-SCSI means no replug, else tell the user to replug.
6. **Format** (optional): offer vfat/exfat/ext4 + label → `format_data_disk`.
7. **Done**: summary notification; offer to open the volume.

## Requirement → implementation map

| Requirement | How |
|---|---|
| Menu of commands | SNI + DBusMenu in the agent; actions call `device_ops`. |
| Setup/initialize wizard | Sequenced zenity dialogs → `reset_device_auto` + `format_data_disk`. |
| Admin or user login | Account radio in the unlock dialog; `choose_login` already supports it. |
| Show failed tries | `read_drive_status().admin_failed_attempts` (read over SCSI when possible) shown as a disabled menu line + tooltip. |
| Hint on click only | `Show password hint` action → `read_profile` → dialog. Never shown by default. |

## Event loop

The agent already `poll()`s the udev monitor fd + a signalfd. Add the sd-bus
connection fd(s) via `sd_bus_get_fd()` / `sd_bus_get_events()` to the same
`poll()` set (or migrate the loop to `sd-event`, which integrates sd-bus
cleanly). Menu actions spawn dialogs the way `password_dialog` already does and
run operations on the existing per-port worker model.

## Gaps / risks

- **Admin+user provisioning — mechanism known, needs a hardware check.** The
  device infers the account count from the `FF A2` payload length:
  `(len-4)/16`. A single account is a 20-byte payload (`size[4]` + one 16-byte
  token); **admin+user is a 36-byte payload = `size[4]` + two 16-byte tokens**,
  each `enc(password[0:16])` (ECB, per account). `reset` currently sets only the
  admin token. To provision both, send two tokens and the user index. One
  caveat: in the passphrase/fields captures the two 16-byte halves differ even
  though both passphrases share their first 16 chars, so either those captures
  are single-passphrase resets on a drive that was *already* admin+user, or
  there is a per-account element not visible without decrypting the tokens.
  Resolve by implementing the straightforward two-token A2 and verifying on
  hardware that **both** accounts unlock (I have the drive); a factory
  admin+user setup capture would remove all doubt. Admin/user *login* already
  works; only *creation* is pending.
- **Factory/blank detection.** Need to confirm how a never-initialized drive
  reports (GetAreaParams / GetConfig) so the wizard can auto-offer setup.
- **SNI host availability.** Bare GNOME needs the AppIndicator extension; the
  agent should degrade gracefully (keep working headless with notifications) if
  no StatusNotifierWatcher is present.
- **Desktop portability.** Icon theme name, file-manager "Open" command, and
  zenity-vs-kdialog differences need the same care the current dialog code takes.

## Phases

1. **Refactor** — extract `find_scsi_helper` / `scsi_usable` / `reset_device_auto`
   / `reenumerate` into `device_ops`; CLI unchanged. (Small, low-risk.)
2. **Tray shell** — SNI + DBusMenu over sd-bus; a static menu that lists drives
   and does Unlock/Lock via the existing worker. Validate on the target desktop.
3. **Status & hint** — failed-count line and on-click hint, read over SCSI when
   available.
4. **Wizard** — single-password setup + owner info + optional format.
5. **Admin/user** — login account choice now; user-account *provisioning* after
   the capture + RE.
