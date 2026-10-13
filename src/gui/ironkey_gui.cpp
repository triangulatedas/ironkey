// SPDX-License-Identifier: GPL-2.0-only
//
// ironkey-gui: the optional GTK3 front-end for ironkey-agent. It renders the
// unlock dialog and the reset/initialize wizard, collects input, and prints a
// structured result (key=value lines) to stdout for the agent to act on. It
// does NO device I/O itself — all of that stays in the agent/device_ops, so GTK
// is never linked into the core tools. Exit 0 = completed (result on stdout),
// non-zero = cancelled.
//
// Usage:
//   ironkey-gui unlock --label TEXT [--accounts single|adminuser] [--note TEXT]
//   ironkey-gui setup  --label TEXT
#include <gtk/gtk.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// ---- password strength ------------------------------------------------------

// Returns 1..4 (Weak/Fair/Good/Strong); sets *name.
int strength_score(const char* p, const char** name)
{
    bool lo = false, up = false, di = false, ot = false;
    size_t n = 0;
    for (const char* c = p; *c; ++c, ++n) {
        unsigned char u = static_cast<unsigned char>(*c);
        if (g_ascii_islower(u))
            lo = true;
        else if (g_ascii_isupper(u))
            up = true;
        else if (g_ascii_isdigit(u))
            di = true;
        else
            ot = true;
    }
    int classes = lo + up + di + ot;
    int score;
    if (n >= 12 && classes >= 3)
        score = 4;
    else if (n >= 10 && classes >= 2)
        score = 3;
    else if (n >= 8)
        score = 2;
    else
        score = 1;
    static const char* names[] = {"", "Weak", "Fair", "Good", "Strong"};
    *name = names[score];
    return score;
}

void emit(const char* key, const char* value)
{
    std::printf("%s=%s\n", key, value ? value : "");
}

const char* arg_value(int argc, char** argv, const char* name)
{
    for (int i = 2; i < argc - 1; ++i)
        if (std::strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return nullptr;
}

// A little theming so it does not look like a bare dialog.
void load_css()
{
    static const char* css =
        ".ik-title{font-size:1.5em;font-weight:bold;}"
        ".ik-sub{color:alpha(currentColor,0.7);}"
        "list.ik-steps{background:transparent;}"
        "list.ik-steps row{padding:9px 20px;border-radius:7px;margin:1px 6px;}"
        "list.ik-steps row:selected{background:@theme_selected_bg_color;color:@theme_selected_fg_color;}"
        ".ik-sidebar{background:alpha(currentColor,0.04);border-right:1px solid alpha(currentColor,0.1);}"
        ".ik-page{padding:22px 26px;}"
        ".ik-warn{color:#c0392b;}";
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(p),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

void add_class(GtkWidget* w, const char* c) { gtk_style_context_add_class(gtk_widget_get_style_context(w), c); }

// Builds a page with a bold heading + optional subheading, returns the vbox and
// hands back the inner content box (where the caller adds widgets) and the
// heading label (so it can be relabelled later).
GtkWidget* make_page(const char* heading, const char* sub, GtkWidget** out_head, GtkWidget** out_body)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    add_class(box, "ik-page");
    GtkWidget* h = gtk_label_new(heading);
    add_class(h, "ik-title");
    gtk_label_set_xalign(GTK_LABEL(h), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), h, FALSE, FALSE, 0);
    if (sub) {
        GtkWidget* s = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(s), sub);
        add_class(s, "ik-sub");
        gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(s), TRUE);
        gtk_box_pack_start(GTK_BOX(box), s, FALSE, FALSE, 0);
    }
    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(box), body, TRUE, TRUE, 6);
    if (out_head)
        *out_head = h;
    if (out_body)
        *out_body = body;
    return box;
}

// ============================ unlock dialog =================================

int run_unlock(const char* label, const char* accounts, const char* note)
{
    load_css();
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Unlock IronKey", nullptr, GTK_DIALOG_MODAL,
                                                    "_Cancel", GTK_RESPONSE_CANCEL, "_Unlock",
                                                    GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, -1);
    GtkWidget* ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    add_class(ok, "suggested-action");

    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_box_set_spacing(GTK_BOX(box), 12);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(row),
                       gtk_image_new_from_icon_name("drive-removable-media", GTK_ICON_SIZE_DIALOG),
                       FALSE, FALSE, 0);
    GtkWidget* heading = gtk_label_new(nullptr);
    char* markup = g_markup_printf_escaped("Enter the password for\n<b>%s</b>", label ? label : "");
    gtk_label_set_markup(GTK_LABEL(heading), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(heading), TRUE);
    gtk_box_pack_start(GTK_BOX(row), heading, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget* account_combo = nullptr;
    if (accounts && std::strcmp(accounts, "adminuser") == 0) {
        account_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(account_combo), "user", "User password");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(account_combo), "admin", "Admin password");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(account_combo), "user");
        gtk_box_pack_start(GTK_BOX(box), account_combo, FALSE, FALSE, 0);
    }

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Password");
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY,
                                      "view-reveal-symbolic");
    g_signal_connect(entry, "icon-press",
                     G_CALLBACK(+[](GtkEntry* e, GtkEntryIconPosition, GdkEvent*, gpointer) {
                         gboolean vis = gtk_entry_get_visibility(e);
                         gtk_entry_set_visibility(e, !vis);
                         gtk_entry_set_icon_from_icon_name(
                             e, GTK_ENTRY_ICON_SECONDARY,
                             vis ? "view-reveal-symbolic" : "view-conceal-symbolic");
                     }),
                     nullptr);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    if (note && *note) {
        GtkWidget* nl = gtk_label_new(nullptr);
        char* nm = g_markup_printf_escaped("<small>%s</small>", note);
        gtk_label_set_markup(GTK_LABEL(nl), nm);
        g_free(nm);
        gtk_label_set_xalign(GTK_LABEL(nl), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(nl), TRUE);
        gtk_box_pack_start(GTK_BOX(box), nl, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(dialog);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    int rc = 1;
    if (response == GTK_RESPONSE_OK) {
        if (account_combo)
            emit("account", gtk_combo_box_get_active_id(GTK_COMBO_BOX(account_combo)));
        emit("password", gtk_entry_get_text(GTK_ENTRY(entry)));
        rc = 0;
    }
    gtk_widget_destroy(dialog);
    return rc;
}

// ============================ setup wizard ==================================

enum { S_INTRO, S_ACCOUNTS, S_ADMIN, S_USER, S_OWNER, S_FORMAT, S_COUNT };

struct Pwd {
    GtkWidget* heading;
    GtkWidget* pw;
    GtkWidget* confirm;
    GtkWidget* bar;
    GtkWidget* strength;
    GtkWidget* hint;
};

struct Wizard {
    GtkWidget* window;
    GtkWidget* stack;
    GtkWidget* listbox;
    GtkWidget* back_btn;
    GtkWidget* next_btn;
    GtkWidget* pages[S_COUNT];
    GtkWidget* single_radio;
    GtkWidget* step_label[S_COUNT];  // sidebar row labels
    Pwd admin;
    Pwd user;
    GtkWidget* name;
    GtkWidget* company;
    GtkWidget* description;
    GtkWidget* fs_combo;
    GtkWidget* fs_label;
    int current;
    int max_reached;
    int result;
};

bool single_mode(Wizard* w)
{
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->single_radio));
}

bool pwd_ok(Pwd* p)
{
    const char* pw = gtk_entry_get_text(GTK_ENTRY(p->pw));
    return std::strlen(pw) >= 6 && std::strcmp(pw, gtk_entry_get_text(GTK_ENTRY(p->confirm))) == 0;
}

bool step_valid(Wizard* w, int step)
{
    if (step == S_ADMIN)
        return pwd_ok(&w->admin);
    if (step == S_USER)
        return single_mode(w) || pwd_ok(&w->user);
    return true;
}

int step_after(Wizard* w, int step)
{
    int n = step + 1;
    if (n == S_USER && single_mode(w))
        ++n;
    return n;
}

int step_before(Wizard* w, int step)
{
    int n = step - 1;
    if (n == S_USER && single_mode(w))
        --n;
    return n;
}

void update_ui(Wizard* w)
{
    gtk_stack_set_visible_child(GTK_STACK(w->stack), w->pages[w->current]);
    gtk_list_box_select_row(GTK_LIST_BOX(w->listbox),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(w->listbox), w->current));
    gtk_widget_set_sensitive(w->back_btn, w->current > S_INTRO);

    bool last = (w->current == S_FORMAT);
    gtk_button_set_label(GTK_BUTTON(w->next_btn), last ? "Erase & set up" : "Next");
    GtkStyleContext* sc = gtk_widget_get_style_context(w->next_btn);
    gtk_style_context_remove_class(sc, "suggested-action");
    gtk_style_context_remove_class(sc, "destructive-action");
    gtk_style_context_add_class(sc, last ? "destructive-action" : "suggested-action");
    gtk_widget_set_sensitive(w->next_btn, step_valid(w, w->current));
}

void go_to(Wizard* w, int step)
{
    w->current = step;
    update_ui(w);
}

void refresh_password_meter(Wizard* w, Pwd* p, int step)
{
    const char* pw = gtk_entry_get_text(GTK_ENTRY(p->pw));
    const char* name = "";
    int score = strength_score(pw, &name);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(p->bar), pw[0] ? score : 0);
    size_t n = std::strlen(pw);
    char buf[160];
    if (!pw[0])
        buf[0] = '\0';
    else if (n < 6)
        std::snprintf(buf, sizeof buf, "<small>At least 6 characters (%zu so far)</small>", n);
    else
        std::snprintf(buf, sizeof buf, "<small>Strength: <b>%s</b>%s</small>", name,
                      n < 10 ? " — a longer passphrase is stronger" : "");
    gtk_label_set_markup(GTK_LABEL(p->strength), buf);
    if (w->current == step)
        gtk_widget_set_sensitive(w->next_btn, step_valid(w, step));
}

// Fills `body` with the password widgets for one account.
void build_password_body(Wizard* w, GtkWidget* body, Pwd* p, int step)
{
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    auto label = [](const char* t) {
        GtkWidget* l = gtk_label_new(t);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
        return l;
    };
    p->pw = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(p->pw), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(p->pw), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_widget_set_hexpand(p->pw, TRUE);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(p->pw), GTK_ENTRY_ICON_SECONDARY,
                                      "view-reveal-symbolic");
    p->confirm = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(p->confirm), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(p->confirm), GTK_INPUT_PURPOSE_PASSWORD);
    p->bar = gtk_level_bar_new_for_interval(0, 4);
    gtk_level_bar_set_mode(GTK_LEVEL_BAR(p->bar), GTK_LEVEL_BAR_MODE_DISCRETE);
    p->strength = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(p->strength), 0.0f);
    p->hint = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->hint),
                                   "shown before unlock — never the password itself");

    gtk_grid_attach(GTK_GRID(grid), label("Password"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->pw, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label("Confirm"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->confirm, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->bar, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->strength, 1, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label("Hint (optional)"), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->hint, 1, 4, 1, 1);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);

    struct CbData {
        Wizard* w;
        Pwd* p;
        int step;
    };
    auto* cb = new CbData{w, p, step};
    GCallback changed = G_CALLBACK(+[](GtkEditable*, gpointer d) {
        auto* c = static_cast<CbData*>(d);
        refresh_password_meter(c->w, c->p, c->step);
    });
    g_signal_connect(p->pw, "changed", changed, cb);
    g_signal_connect(p->confirm, "changed", changed, cb);
    g_signal_connect(p->pw, "icon-press",
                     G_CALLBACK(+[](GtkEntry* e, GtkEntryIconPosition, GdkEvent*, gpointer d) {
                         Pwd* pg = static_cast<Pwd*>(d);
                         gboolean vis = gtk_entry_get_visibility(GTK_ENTRY(pg->pw));
                         gtk_entry_set_visibility(GTK_ENTRY(pg->pw), !vis);
                         gtk_entry_set_visibility(GTK_ENTRY(pg->confirm), !vis);
                         gtk_entry_set_icon_from_icon_name(
                             e, GTK_ENTRY_ICON_SECONDARY,
                             vis ? "view-reveal-symbolic" : "view-conceal-symbolic");
                     }),
                     p);
}

// Relabels the admin step/heading depending on the account choice.
void sync_account_labels(Wizard* w)
{
    bool single = single_mode(w);
    gtk_label_set_text(GTK_LABEL(w->step_label[S_ADMIN]), single ? "Password" : "Admin password");
    gtk_label_set_text(GTK_LABEL(w->admin.heading),
                       single ? "Set the password" : "Set the admin password");
    // The user step is only reachable in admin+user mode.
    GtkWidget* urow = GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(w->listbox), S_USER));
    gtk_widget_set_sensitive(urow, !single);
}

void gather_and_emit(Wizard* w)
{
    bool single = single_mode(w);
    emit("mode", single ? "single" : "adminuser");
    emit("admin_password", gtk_entry_get_text(GTK_ENTRY(w->admin.pw)));
    emit("admin_hint", gtk_entry_get_text(GTK_ENTRY(w->admin.hint)));
    if (!single) {
        emit("user_password", gtk_entry_get_text(GTK_ENTRY(w->user.pw)));
        emit("user_hint", gtk_entry_get_text(GTK_ENTRY(w->user.hint)));
    }
    emit("name", gtk_entry_get_text(GTK_ENTRY(w->name)));
    emit("company", gtk_entry_get_text(GTK_ENTRY(w->company)));
    emit("description", gtk_entry_get_text(GTK_ENTRY(w->description)));
    const char* fs = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w->fs_combo));
    emit("format", fs ? fs : "none");
    emit("label", gtk_entry_get_text(GTK_ENTRY(w->fs_label)));
}

int run_setup(const char* label)
{
    load_css();
    static Wizard w{};
    w.result = 1;

    w.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(w.window), 620, 440);
    GtkWidget* header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Set up IronKey");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_pack_start(
        GTK_HEADER_BAR(header),
        gtk_image_new_from_icon_name("drive-harddisk", GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_window_set_titlebar(GTK_WINDOW(w.window), header);
    g_signal_connect(w.window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { gtk_main_quit(); }),
                     nullptr);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(w.window), root);
    GtkWidget* middle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(root), middle, TRUE, TRUE, 0);

    // Sidebar (clickable steps).
    w.listbox = gtk_list_box_new();
    add_class(w.listbox, "ik-steps");
    GtkWidget* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(sidebar, "ik-sidebar");
    gtk_box_pack_start(GTK_BOX(sidebar), w.listbox, TRUE, TRUE, 10);
    gtk_widget_set_size_request(sidebar, 170, -1);
    gtk_box_pack_start(GTK_BOX(middle), sidebar, FALSE, FALSE, 0);

    w.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(w.stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_widget_set_hexpand(w.stack, TRUE);
    gtk_box_pack_start(GTK_BOX(middle), w.stack, TRUE, TRUE, 0);

    const char* titles[S_COUNT] = {"Reset",     "Accounts",   "Password",
                                   "User password", "Owner info", "Finish"};
    for (int i = 0; i < S_COUNT; ++i) {
        GtkWidget* r = gtk_label_new(titles[i]);
        gtk_label_set_xalign(GTK_LABEL(r), 0.0f);
        w.step_label[i] = r;
        gtk_list_box_insert(GTK_LIST_BOX(w.listbox), r, -1);
    }

    // --- page 0: reset warning
    {
        GtkWidget* head;
        GtkWidget* page = make_page("Reset and set up", nullptr, &head, nullptr);
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_box_pack_start(GTK_BOX(row),
                           gtk_image_new_from_icon_name("dialog-warning", GTK_ICON_SIZE_DIALOG),
                           FALSE, FALSE, 0);
        GtkWidget* l = gtk_label_new(nullptr);
        char* m = g_markup_printf_escaped(
            "This <b>erases everything</b> on\n<b>%s</b>\nand sets a new password.\n\n"
            "It cannot be undone. No current password is needed.", label ? label : "this drive");
        gtk_label_set_markup(GTK_LABEL(l), m);
        g_free(m);
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
        gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
        // put the warning row into the page body
        GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), body, TRUE, TRUE, 6);
        w.pages[S_INTRO] = page;
        gtk_stack_add_named(GTK_STACK(w.stack), page, "intro");
    }

    // --- page 1: accounts
    {
        GtkWidget* body;
        GtkWidget* page = make_page(
            "Accounts",
            "A separate <b>user</b> account is optional. It has its own password; the <b>admin</b> "
            "can reset it, the user cannot change the admin's. Most people need just one.",
            nullptr, &body);
        w.single_radio = gtk_radio_button_new_with_label(nullptr, "Just one password");
        GtkWidget* both = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(w.single_radio), "Admin  +  a separate user account");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w.single_radio), TRUE);
        gtk_box_pack_start(GTK_BOX(body), w.single_radio, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(body), both, FALSE, FALSE, 0);
        g_signal_connect(w.single_radio, "toggled",
                         G_CALLBACK(+[](GtkToggleButton*, gpointer d) {
                             sync_account_labels(static_cast<Wizard*>(d));
                         }),
                         &w);
        w.pages[S_ACCOUNTS] = page;
        gtk_stack_add_named(GTK_STACK(w.stack), page, "accounts");
    }

    // --- page 2: admin password
    {
        GtkWidget* body;
        GtkWidget* page = make_page("Set the password",
                                    "At least 6 characters. A longer passphrase is far harder to "
                                    "guess.",
                                    &w.admin.heading, &body);
        build_password_body(&w, body, &w.admin, S_ADMIN);
        w.pages[S_ADMIN] = page;
        gtk_stack_add_named(GTK_STACK(w.stack), page, "admin");
    }

    // --- page 3: user password
    {
        GtkWidget* body;
        GtkWidget* page = make_page("Set the user password",
                                    "The user logs in with this. The admin can reset it later.",
                                    &w.user.heading, &body);
        build_password_body(&w, body, &w.user, S_USER);
        w.pages[S_USER] = page;
        gtk_stack_add_named(GTK_STACK(w.stack), page, "user");
    }

    // --- page 4: owner info
    {
        GtkWidget* body;
        GtkWidget* page = make_page(
            "Owner info",
            "<small>Optional. Stored on the drive and readable before unlock, on any computer.</small>",
            nullptr, &body);
        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        const char* names[] = {"Name", "Company", "Description"};
        GtkWidget** fields[] = {&w.name, &w.company, &w.description};
        for (int i = 0; i < 3; ++i) {
            GtkWidget* lab = gtk_label_new(names[i]);
            gtk_label_set_xalign(GTK_LABEL(lab), 0.0f);
            *fields[i] = gtk_entry_new();
            gtk_widget_set_hexpand(*fields[i], TRUE);
            gtk_grid_attach(GTK_GRID(grid), lab, 0, i, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), *fields[i], 1, i, 1, 1);
        }
        gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
        w.pages[S_OWNER] = page;
        gtk_stack_add_named(GTK_STACK(w.stack), page, "owner");
    }

    // --- page 5: finish (drive name up front; filesystem hidden under Custom)
    {
        GtkWidget* body;
        GtkWidget* page = make_page("Ready to set up",
                                    "Give the drive a name. It will be erased, protected with "
                                    "your password and formatted so it's ready to use.",
                                    nullptr, &body);
        GtkWidget* namerow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget* namelab = gtk_label_new("Drive name");
        gtk_label_set_xalign(GTK_LABEL(namelab), 0.0f);
        w.fs_label = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(w.fs_label), "IRONKEY");
        gtk_widget_set_hexpand(w.fs_label, TRUE);
        gtk_box_pack_start(GTK_BOX(namerow), namelab, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(namerow), w.fs_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(body), namerow, FALSE, FALSE, 0);

        // Filesystem choice is an advanced option: hidden until expanded, with a
        // sensible default (exFAT — works on Windows/macOS/Linux with no 4 GB
        // file limit, unlike FAT32).
        GtkWidget* exp = gtk_expander_new("Custom format options");
        GtkWidget* ebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(ebox), 8);
        GtkWidget* fslab = gtk_label_new("Filesystem");
        gtk_label_set_xalign(GTK_LABEL(fslab), 0.0f);
        w.fs_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.fs_combo), "exfat",
                                  "exFAT — Windows, macOS and Linux (recommended)");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.fs_combo), "vfat",
                                  "FAT32 — most compatible, but no files over 4 GB");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.fs_combo), "ext4", "ext4 — Linux only");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.fs_combo), "none",
                                  "Don't format now — I'll do it myself");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(w.fs_combo), "exfat");
        gtk_box_pack_start(GTK_BOX(ebox), fslab, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ebox), w.fs_combo, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(exp), ebox);
        gtk_box_pack_start(GTK_BOX(body), exp, FALSE, FALSE, 8);

        w.pages[S_FORMAT] = page;
        gtk_stack_add_named(GTK_STACK(w.stack), page, "format");
    }

    // Clickable steps: only to a step already reached.
    g_signal_connect(w.listbox, "row-activated",
                     G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer d) {
                         Wizard* ww = static_cast<Wizard*>(d);
                         int idx = gtk_list_box_row_get_index(row);
                         if (idx <= ww->max_reached && gtk_widget_get_sensitive(GTK_WIDGET(row)))
                             go_to(ww, idx);
                     }),
                     &w);

    // Action bar.
    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(actions), 12);
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    w.back_btn = gtk_button_new_with_label("Back");
    w.next_btn = gtk_button_new_with_label("Next");
    gtk_box_pack_start(GTK_BOX(actions), cancel, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), w.next_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), w.back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);

    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { gtk_main_quit(); }),
                     nullptr);
    g_signal_connect(w.back_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
                         Wizard* ww = static_cast<Wizard*>(d);
                         go_to(ww, step_before(ww, ww->current));
                     }),
                     &w);
    g_signal_connect(w.next_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
                         Wizard* ww = static_cast<Wizard*>(d);
                         if (!step_valid(ww, ww->current))
                             return;
                         if (ww->current == S_FORMAT) {
                             gather_and_emit(ww);
                             ww->result = 0;
                             gtk_widget_destroy(ww->window);
                             return;
                         }
                         int n = step_after(ww, ww->current);
                         if (n > ww->max_reached)
                             ww->max_reached = n;
                         go_to(ww, n);
                     }),
                     &w);

    sync_account_labels(&w);
    gtk_widget_show_all(w.window);
    go_to(&w, S_INTRO);
    gtk_main();
    return w.result;
}

}  // namespace

int main(int argc, char** argv)
{
    gtk_init(&argc, &argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: ironkey-gui unlock|setup --label TEXT ...\n");
        return 2;
    }
    const char* label = arg_value(argc, argv, "--label");
    if (std::strcmp(argv[1], "unlock") == 0)
        return run_unlock(label, arg_value(argc, argv, "--accounts"),
                          arg_value(argc, argv, "--note"));
    if (std::strcmp(argv[1], "setup") == 0)
        return run_setup(label);
    std::fprintf(stderr, "ironkey-gui: unknown command '%s'\n", argv[1]);
    return 2;
}
