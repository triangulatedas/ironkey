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
  - `Unlock…` → the login dialog (see "Login / unlock dialog UX" below). On a
    single-password drive it just says *Login*; on an admin+user drive it
    defaults to the user account with an admin toggle. Shows the failed-login
    warning inline.
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

## Login / unlock dialog UX

This is the dialog the agent pops on insert and the tray's `Unlock…` opens. The
backend already decides the account and the wording — `choose_login()` defaults
to the **user** account on an admin+user drive and the sole account otherwise,
and `Login::password_name()` returns plain **"password"** for a single-password
drive but **"admin password" / "user password"** when both exist — so the UI
only renders that state.

**Account selection (rendered from `DeviceSummary.setup`, read non-disruptively
before the dialog):**

- **Single-password drive** → no account concept. Title *"Unlock IronKey"*, one
  field prompting for *"the password"*, and **no toggle** — never show the words
  "user" or "admin".
- **Admin + user drive** → **default to the user account** (matching the vendor
  and `choose_login`). Offer a switch to the admin account:
  - *zenity:* `--forms --add-combo="Log in as" --combo-values="User|Admin"
    --add-password="Password"` (one dialog, user preselected). The weak-RSA host
    warning goes in `--text`.
  - *kdialog / no combo support:* keep the plain password field and put the
    account choice on the tray submenu instead — `Unlock (user)…` as the default
    action plus `Unlock as admin…`. The agent's auto-popup, which has no menu,
    uses a tiny first step: a `--radiolist`/`--menu` with User preselected, then
    the password entry (skipped entirely on single-password drives, so the common
    case stays one dialog).
  - Reflect the choice in the prompt wording via `password_name()`, and show the
    account-specific lockout note from `Login::lockout_consequence()` (user login
    locks vs admin failures erase the drive).

**Manage / set-up affordance (the "cog"):** the login dialog offers a way out to
device management without unlocking — for a drive you want to initialize, format
or inspect rather than log into.

- *zenity:* `--extra-button="⚙ Set up / Manage…"`. Clicking it returns a distinct
  exit code; the agent then opens the management surface (the tray submenu for
  that drive, or directly the wizard / format / owner-info flows).
- *tray:* the cog is unnecessary as a dialog button — the drive's submenu already
  carries `Reset / Set up…`, `Owner info…`, `Show password hint`. The dialog's
  cog is mainly for the **agent's auto-popup**, which appears without the tray
  menu in reach.
- The cog never bypasses the erase confirmation: it leads to the wizard, which
  still requires the explicit ERASE step (see the wizard section).

**Failed-login feedback:** when `DeviceSummary.admin_failed_attempts > 0`, show
`Login::failed_warning()` inline (e.g. "2 failed logins; 8 left before the drive
erases itself"). This is per the admin counter; the user counter is not exposed.

Requirement recap: *user-default with an admin toggle on two-account drives, a
plain "Login" with no account wording on one-account drives, and a cog in the
popup to reach set-up/format* — all driven by `DeviceSummary.setup` +
`choose_login()` + `password_name()`, which already exist; only the dialog
rendering is new.

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

Entry point: the `Reset / Set up…` menu item (always available on a locked
drive). There is **no** reliable "this drive is blank" auto-prompt — a blank
drive can't be told apart from a single mistyped password (see Gaps) — so setup
is always user-initiated, never auto-raised.

Steps:
1. **Confirm erase**: big warning + type ERASE / click-through. (We can't assume
   the drive is blank, so the erase confirmation always applies.)
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
| Admin or user login | Login dialog defaults to the user account with an admin toggle on admin+user drives, and shows a plain "Login" (no account wording) on single-password drives — see "Login / unlock dialog UX". Driven by `choose_login()` + `password_name()`, which already exist. |
| Set-up/format from the login popup | A "⚙ Set up / Manage…" affordance (zenity `--extra-button`, or the drive's tray submenu) that opens the wizard / format / owner-info without unlocking; never bypasses the erase confirmation. |
| Show failed tries | `read_drive_status().admin_failed_attempts` (read over SCSI when possible) shown as a disabled menu line + tooltip. |
| Hint on click only | `Show password hint` action → `read_profile` → dialog. Never shown by default. |

## Event loop

The agent already `poll()`s the udev monitor fd + a signalfd. Add the sd-bus
connection fd(s) via `sd_bus_get_fd()` / `sd_bus_get_events()` to the same
`poll()` set (or migrate the loop to `sd-event`, which integrates sd-bus
cleanly). Menu actions spawn dialogs the way `password_dialog` already does and
run operations on the existing per-port worker model.

## Gaps / risks

- **Admin+user provisioning — DONE (implemented and hardware-verified).** The
  device infers the account count from the `FF A2` payload length:
  `(len-4)/16`. A single account is a 20-byte payload (`size[4]` + one 16-byte
  token); **admin+user is a 36-byte payload = `size[4]` + two 16-byte tokens**,
  each `enc(password_block)` (ECB, per account), admin first (index 0) then user
  (index 1); the CDB is identical. Implemented as `configure_payload()` +
  `reset_device(ch, admin, user)` in session.cpp, exposed on the CLI as
  `reset --admin-user`. Verified on hardware: after provisioning, the drive
  reports `account_flags = 0x07` (admin+user) and both accounts unlock
  independently (see PROTOCOL.md). The earlier caveat — that the two 16-byte
  halves in the passphrase captures differ even though both passphrases share
  their first 16 chars — was explained by `password_block()`: for a ≥17-char
  passphrase it is `MD5(whole passphrase)`, so two different passphrases give
  different tokens even when their first 16 chars match. No hidden per-account
  element. The UI wizard just needs to collect the two passwords and call
  `reset_device_full(..., user_password)`.
- **Factory/blank detection — investigated, not reliably possible; do not
  auto-classify.** A real blank drive reads `GetAreaParams[0x0C] = 1` and
  `account_flags = 0x03`; a freshly provisioned drive reads `[0x0C] = 0`. But
  `0x0C` is the admin failed-login counter, so a blank drive is indistinguishable
  from a single-password drive at exactly one failed login (a common mistype).
  The vendor app's richer reads (`FF A0` per account, `FF A0 CDB[3]=0xFF`
  summary, `FF 00` device-info/retry blocks) are byte-identical across blank /
  single / admin+user — they carry no account state — so the vendor has no clean
  signal either (see PROTOCOL.md). **Decision:** the tray offers **both**
  "Unlock…" and "Reset / Set up…" for every locked drive and never guesses
  "uninitialized." `account_flags` still cleanly flags admin+user (`0x07`).
- **SNI host availability.** Bare GNOME needs the AppIndicator extension; the
  agent should degrade gracefully (keep working headless with notifications) if
  no StatusNotifierWatcher is present.
- **Desktop portability.** Icon theme name, file-manager "Open" command, and
  zenity-vs-kdialog differences need the same care the current dialog code takes.

## Phases

1. **Refactor — done.** `find_scsi_helper`, `scsi_usable`, `reset_device_auto`
   and `reenumerate` now live in `device_ops`; `cmd_reset` is a thin caller that
   keeps only the UI (prompts + next-steps text). The agent/tray reuse the same
   reset flow via `reset_device_auto(dev, admin, user, profile, prefer, log)`.
2. **Tray shell — done.** `Tray` (`src/agent/tray.{h,cpp}`) exports the SNI +
   dbusmenu over sd-bus and registers with the watcher; the menu lists each drive
   with an Unlock/Lock action plus Quit, Unlock going through
   `Agent::request_unlock`. Shares the agent's `poll()` loop; headless fallback if
   no bus/watcher. Verified on GNOME (Wayland): the icon shows in the top bar and
   the menu opens and acts; also checked via busctl (registration, properties,
   layout, and a click dispatching an action).
3. **Status & hint — done.** Each drive is now a submenu (titled by owner name,
   else model/serial) with a disabled status line ("Admin + user · N of 10 failed
   logins"), Unlock…/Lock & eject, and Show password hint. Status comes from
   `read_device_summary()`; the hint from the new `read_profile_auto()` — both a
   single non-disruptive pass (SCSI when locked in storage mode, HID only when
   already HID; never switches a locked drive to HID). Verified live on GNOME via
   busctl: submenu, status line, and a Show-hint click that left the drive in
   locked storage mode. Also surfaced as `ironkey-unlock status`.
4. **Wizard — done.** A `Reset / Set up…` item per locked drive opens a GTK
   wizard (`ironkey-gui`, see below): confirm-erase → single vs admin+user →
   password(s) with live strength + hint → owner info → finish (drive name, fs
   under a Custom expander, default exFAT). The agent collects the answers and
   runs `reset_device_auto` + optional unlock-and-format.
5. **Admin/user — done.** The unlock dialog (`ironkey-gui unlock`) has an
   in-dialog user/admin toggle on two-account drives (default user), plain
   "password" otherwise; the worker builds the Login from the dialog's choice.
   User-account *provisioning* via `reset --admin-user` was already done.

## GUI front-end (`ironkey-gui`)

The dialogs are a separate optional GTK3 binary, `ironkey-gui`, not zenity (which
was too limited — no single-window wizard, no live strength meter). The core
(CLI + agent + tray) still links no GUI toolkit; only `ironkey-gui` links GTK3,
built only when `gtk+-3.0` is present. The agent spawns it (`unlock` / `setup`),
reads `key=value` on stdout, and performs all device I/O itself, so GTK never
touches the device code. When `ironkey-gui` is absent the agent falls back to the
zenity/kdialog sequence. Located via `$IRONKEY_GUI`, next to the agent, or PATH.
