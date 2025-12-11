// colossus_pkgcenter.cpp
// COLOSSUS System Installer — Arch/AUR GUI for yay.
//
// Features:
// - GTK3 UI that follows system theme
// - On startup asks for sudo password (cached in RAM)
// - Search via `yay -Ss` as normal user
// - Install/remove via yay as normal user (after sudo pre-auth)
// - "Clean Orphans" button runs `yay -Yc --noconfirm`
// - UI stays responsive during long operations.
//
// Build (Arch):
//   sudo pacman -S gtk3 base-devel
//   g++ colossus_pkgcenter.cpp -o colossus-pkgcenter `pkg-config --cflags --libs gtk+-3.0`
//
// Run:
//   ./colossus-pkgcenter

#include <gtk/gtk.h>
#include <string>
#include <vector>
#include <sstream>
#include <cstdio>

// ───────────────────────────────────────────────
//  Simple command runners
// ───────────────────────────────────────────────

// Capture stdout, but keep GTK responsive while reading
std::string run_command(const std::string &cmd) {
    std::string result;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;

        // Keep GTK responsive
        while (gtk_events_pending()) {
            gtk_main_iteration_do(FALSE);
        }
    }
    pclose(pipe);
    return result;
}

// Just run a command, return true if exit status == 0, keep GTK responsive.
bool run_command_status(const std::string &cmd) {
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        // If you ever want to log, process `buffer` here.

        // Keep GTK responsive while the command runs
        while (gtk_events_pending()) {
            gtk_main_iteration_do(FALSE);
        }
    }

    int rc = pclose(pipe);
    return (rc == 0);
}

// Strip ANSI color (CSI) and OSC hyperlink sequences from yay output.
std::string strip_ansi_and_osc(const std::string &input) {
    std::string out;
    out.reserve(input.size());

    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);

        if (c == 0x1B) { // ESC
            if (i + 1 >= input.size()) {
                // Lone ESC at end, skip it.
                i++;
                continue;
            }

            unsigned char next = static_cast<unsigned char>(input[i + 1]);

            // CSI sequences: ESC [
            if (next == '[') {
                size_t j = i + 2;
                while (j < input.size()) {
                    unsigned char d = static_cast<unsigned char>(input[j]);
                    // Final byte in CSI sequence is between '@' and '~'
                    if (d >= '@' && d <= '~') {
                        j++;
                        break;
                    }
                    j++;
                }
                i = j;
                continue;
            }

            // OSC sequences: ESC ]
            if (next == ']') {
                size_t j = i + 2;
                while (j < input.size()) {
                    unsigned char d = static_cast<unsigned char>(input[j]);
                    if (d == 0x07) { // BEL terminator
                        j++;
                        break;
                    }
                    // String terminator: ESC followed by backslash
                    if (d == 0x1B && j + 1 < input.size() &&
                        static_cast<unsigned char>(input[j + 1]) == '\\') {
                        j += 2;
                        break;
                    }
                    j++;
                }
                i = j;
                continue;
            }

            // Any other ESC sequence: just drop the ESC and move on.
            i++;
            continue;
        }

        // Normal character, keep it.
        out.push_back(input[i]);
        i++;
    }

    return out;
}

// Run a sudo command with password sent on stdin (no output captured).
// We use this to pre-auth sudo (not to run yay itself).
bool run_sudo_with_password(const std::string &password,
                            const std::string &cmd) {
    if (password.empty()) return false;

    FILE *pipe = popen(cmd.c_str(), "w");
    if (!pipe) return false;

    std::string pwline = password + "\n";
    fwrite(pwline.c_str(), 1, pwline.size(), pipe);
    fflush(pipe);

    int rc = pclose(pipe);
    return (rc == 0);
}

// Check if a package is installed using pacman.
bool is_package_installed(const std::string &name) {
    std::string cmd = "pacman -Qi " + name + " >/dev/null 2>&1";
    return run_command_status(cmd);
}

// ───────────────────────────────────────────────
//  Data model
// ───────────────────────────────────────────────

struct PackageInfo {
    std::string repo;
    std::string name;
    std::string version;
    std::string description;
    bool installed = false;
};

// Parse `yay -Ss` output (after stripping ANSI/OSC)
std::vector<PackageInfo> parse_yay_search(const std::string &output) {
    std::vector<PackageInfo> pkgs;

    std::istringstream iss(output);
    std::string line;
    PackageInfo current;
    bool expecting_desc = false;

    while (std::getline(iss, line)) {
        if (line.empty()) {
            expecting_desc = false;
            continue;
        }

        // Description lines: leading space or tab
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            if (expecting_desc) {
                size_t pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) {
                    current.description = line.substr(pos);
                } else {
                    current.description = line;
                }
                pkgs.push_back(current);
                expecting_desc = false;
            }
            continue;
        }

        // Header line: "repo/name version [installed]"
        std::istringstream header(line);
        std::string repo_name, version;
        if (!(header >> repo_name >> version)) {
            continue;
        }

        size_t slash_pos = repo_name.find('/');
        if (slash_pos == std::string::npos) {
            continue;
        }

        current.repo = repo_name.substr(0, slash_pos);
        current.name = repo_name.substr(slash_pos + 1);
        current.version = version;
        current.description.clear();
        current.installed = false;

        std::string rest;
        std::getline(header, rest);
        if (rest.find("[installed]") != std::string::npos ||
            rest.find("(installed)") != std::string::npos) {
            current.installed = true;
        }

        expecting_desc = true;
    }

    return pkgs;
}

// ───────────────────────────────────────────────
//  Globals
// ───────────────────────────────────────────────

static GtkWidget *g_main_window   = nullptr;
static GtkWidget *g_search_entry  = nullptr;
static GtkWidget *g_results_list  = nullptr;
static GtkWidget *g_status_label  = nullptr;
static std::string g_sudo_password;

// Forward declarations
static void perform_search(const std::string &query);
extern "C" void on_install_clicked(GtkWidget *button, gpointer user_data);
extern "C" void on_remove_clicked(GtkWidget *button, gpointer user_data);
extern "C" void on_clean_orphans_clicked(GtkWidget *button, gpointer user_data);

// ───────────────────────────────────────────────
//  Install / Remove / Clean handlers
// ───────────────────────────────────────────────

extern "C" void on_install_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    char *pkg_name_c = static_cast<char *>(user_data);
    if (!pkg_name_c) return;

    std::string pkg_name(pkg_name_c);
    g_free(pkg_name_c);

    if (g_sudo_password.empty()) {
        GtkWidget *warn = gtk_message_dialog_new(
            GTK_WINDOW(g_main_window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "No sudo password cached.\nPlease restart the app."
        );
        gtk_dialog_run(GTK_DIALOG(warn));
        gtk_widget_destroy(warn);
        return;
    }

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_OK_CANCEL,
        "Install package \"%s\" using yay?",
        pkg_name.c_str()
    );
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response != GTK_RESPONSE_OK) {
        return;
    }

    if (g_status_label) {
        std::string msg = "Installing " + pkg_name + "...";
        gtk_label_set_text(GTK_LABEL(g_status_label), msg.c_str());
    }

    GtkWidget *info = gtk_message_dialog_new(
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_NONE,
        "Installing %s...\n\nThis may take a moment.",
        pkg_name.c_str()
    );
    gtk_widget_show_all(info);

    // 1) Pre-authenticate sudo (cache credentials for the user).
    bool auth_ok = run_sudo_with_password(
        g_sudo_password,
        "sudo -S -v"
    );

    bool ok = false;
    if (auth_ok) {
        // 2) Run yay as the NORMAL USER (no sudo here!)
        //    yay will call sudo pacman internally, which will reuse the cached credentials.
        std::string cmd =
            "yay -S --noconfirm "
            "--answerclean None "
            "--answerdiff None "
            "--answeredit None "
            + pkg_name;

        ok = run_command_status(cmd);
    }

    gtk_widget_destroy(info);

    GtkWidget *done = gtk_message_dialog_new(
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK,
        ok ?
          "Installation finished.\nRe-run the search to see the updated status." :
          "Installation may have failed.\nCheck terminal logs or run yay manually."
    );
    gtk_dialog_run(GTK_DIALOG(done));
    gtk_widget_destroy(done);

    const char *current_query = gtk_entry_get_text(GTK_ENTRY(g_search_entry));
    if (current_query && *current_query) {
        perform_search(current_query);
    } else if (g_status_label) {
        gtk_label_set_text(GTK_LABEL(g_status_label),
                           "Ready. Enter a search term.");
    }
}

extern "C" void on_remove_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    char *pkg_name_c = static_cast<char *>(user_data);
    if (!pkg_name_c) return;

    std::string pkg_name(pkg_name_c);
    g_free(pkg_name_c);

    if (g_sudo_password.empty()) {
        GtkWidget *warn = gtk_message_dialog_new(
            GTK_WINDOW(g_main_window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "No sudo password cached.\nPlease restart the app."
        );
        gtk_dialog_run(GTK_DIALOG(warn));
        gtk_widget_destroy(warn);
        return;
    }

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_OK_CANCEL,
        "Remove package \"%s\"?\n\nThis will also remove unused dependencies.",
        pkg_name.c_str()
    );
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response != GTK_RESPONSE_OK) {
        return;
    }

    if (g_status_label) {
        std::string msg = "Removing " + pkg_name + "...";
        gtk_label_set_text(GTK_LABEL(g_status_label), msg.c_str());
    }

    GtkWidget *info = gtk_message_dialog_new(
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_NONE,
        "Removing %s...\n\nThis may take a moment.",
        pkg_name.c_str()
    );
    gtk_widget_show_all(info);

    // Pre-auth sudo
    bool auth_ok = run_sudo_with_password(
        g_sudo_password,
        "sudo -S -v"
    );

    bool ok = false;
    if (auth_ok) {
        // Remove package and unused dependencies.
        std::string cmd =
            "yay -Rns --noconfirm " + pkg_name;

        ok = run_command_status(cmd);
    }

    gtk_widget_destroy(info);

    GtkWidget *done = gtk_message_dialog_new(
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK,
        ok ?
          "Removal finished.\nRe-run the search to see the updated status." :
          "Removal may have failed.\nCheck terminal logs or run yay manually."
    );
    gtk_dialog_run(GTK_DIALOG(done));
    gtk_widget_destroy(done);

    const char *current_query = gtk_entry_get_text(GTK_ENTRY(g_search_entry));
    if (current_query && *current_query) {
        perform_search(current_query);
    } else if (g_status_label) {
        gtk_label_set_text(GTK_LABEL(g_status_label),
                           "Ready. Enter a search term.");
    }
}

extern "C" void on_clean_orphans_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (g_sudo_password.empty()) {
        GtkWidget *warn = gtk_message_dialog_new(
            GTK_WINDOW(g_main_window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "No sudo password cached.\nPlease restart the app."
        );
        gtk_dialog_run(GTK_DIALOG(warn));
        gtk_widget_destroy(warn);
        return;
    }

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_OK_CANCEL,
        "Clean up orphaned packages?\n\nThis runs \"yay -Yc --noconfirm\" to remove unused dependencies."
    );
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response != GTK_RESPONSE_OK) {
        return;
    }

    if (g_status_label) {
        gtk_label_set_text(GTK_LABEL(g_status_label),
                           "Cleaning orphaned packages...");
    }

    GtkWidget *info = gtk_message_dialog_new(
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_NONE,
        "Cleaning orphaned packages...\n\nThis may take a moment.",
        nullptr
    );
    gtk_widget_show_all(info);

    bool auth_ok = run_sudo_with_password(
        g_sudo_password,
        "sudo -S -v"
    );

    bool ok = false;
    if (auth_ok) {
        ok = run_command_status("yay -Yc --noconfirm");
    }

    gtk_widget_destroy(info);

    GtkWidget *done = gtk_message_dialog_new(
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK,
        ok ?
          "Orphan cleanup finished." :
          "Cleanup may have failed.\nCheck terminal logs or run yay -Yc manually."
    );
    gtk_dialog_run(GTK_DIALOG(done));
    gtk_widget_destroy(done);

    if (g_status_label) {
        gtk_label_set_text(GTK_LABEL(g_status_label),
                           "Orphan cleanup complete. You can search again.");
    }
}

// ───────────────────────────────────────────────
//  UI helpers
// ───────────────────────────────────────────────

void clear_results() {
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_results_list));
    for (GList *iter = children; iter != nullptr; iter = iter->next) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);
}

GtkWidget* create_package_row(const PackageInfo &pkg) {
    // Outer vbox = content + separator
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(outer, 2);
    gtk_widget_set_margin_bottom(outer, 2);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 8);

    // Row content: HBox (icon + text + button)
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    // Icon
    GtkWidget *icon = nullptr;
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    if (gtk_icon_theme_has_icon(theme, pkg.name.c_str())) {
        icon = gtk_image_new_from_icon_name(pkg.name.c_str(), GTK_ICON_SIZE_DIALOG);
    } else {
        // Generic software icon fallback
        icon = gtk_image_new_from_icon_name("system-software-install", GTK_ICON_SIZE_DIALOG);
    }
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

    // Text vbox
    GtkWidget *text_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    // First line: "<name> -- <version>" plus repo tag
    std::string name_line = pkg.name + " -- " + pkg.version;
    GtkWidget *name_label = gtk_label_new(name_line.c_str());
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);

    std::string repo_tag = "[" + pkg.repo + "]";
    GtkWidget *repo_label = gtk_label_new(repo_tag.c_str());
    gtk_label_set_xalign(GTK_LABEL(repo_label), 0.0);

    GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(name_row), name_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(name_row), repo_label, FALSE, FALSE, 0);

    // Description below
    GtkWidget *desc_label = gtk_label_new(pkg.description.c_str());
    gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(desc_label), TRUE);

    gtk_box_pack_start(GTK_BOX(text_vbox), name_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text_vbox), desc_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(row), text_vbox, TRUE, TRUE, 0);

    // Right side: Install or Remove button
    if (pkg.installed) {
        GtkWidget *btn = gtk_button_new_with_label("Remove");
        char *pkg_name_copy = g_strdup(pkg.name.c_str());
        g_signal_connect(btn, "clicked", G_CALLBACK(on_remove_clicked), pkg_name_copy);
        gtk_box_pack_end(GTK_BOX(row), btn, FALSE, FALSE, 0);
    } else {
        GtkWidget *btn = gtk_button_new_with_label("Install");
        char *pkg_name_copy = g_strdup(pkg.name.c_str());
        g_signal_connect(btn, "clicked", G_CALLBACK(on_install_clicked), pkg_name_copy);
        gtk_box_pack_end(GTK_BOX(row), btn, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    // Separator bar
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(outer), sep, FALSE, FALSE, 4);

    return outer;
}

void populate_results(const std::vector<PackageInfo> &pkgs) {
    clear_results();

    int total = 0;
    int installed = 0;

    for (const auto &pkg : pkgs) {
        total++;
        if (pkg.installed) installed++;

        GtkWidget *outer = create_package_row(pkg);
        gtk_list_box_insert(GTK_LIST_BOX(g_results_list), outer, -1);
    }

    gtk_widget_show_all(g_results_list);

    if (g_status_label) {
        std::string status = "Results: " + std::to_string(total) +
                             "  | Installed: " + std::to_string(installed) +
                             " (already on system)";
        gtk_label_set_text(GTK_LABEL(g_status_label), status.c_str());
    }
}

// ───────────────────────────────────────────────
//  Search logic
// ───────────────────────────────────────────────

static void perform_search(const std::string &query) {
    if (query.empty()) {
        clear_results();
        if (g_status_label)
            gtk_label_set_text(GTK_LABEL(g_status_label), "Ready. Enter a search term.");
        return;
    }

    std::string cmd = "yay -Ss " + query;
    std::string raw_output = run_command(cmd);
    std::string output = strip_ansi_and_osc(raw_output);

    auto pkgs = parse_yay_search(output);

    // Double-check installed status with pacman so the Remove button is accurate
    for (auto &pkg : pkgs) {
        if (is_package_installed(pkg.name)) {
            pkg.installed = true;
        } else {
            pkg.installed = false;
        }
    }

    populate_results(pkgs);

    if (pkgs.empty() && g_status_label) {
        gtk_label_set_text(GTK_LABEL(g_status_label), "No results found.");
    }
}

extern "C" void on_search_activated(GtkWidget *entry, gpointer) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!text) return;
    perform_search(text);
}

// ───────────────────────────────────────────────
//  Password dialog
// ───────────────────────────────────────────────

void prompt_for_sudo_password() {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Authentication Required",
        GTK_WINDOW(g_main_window),
        GTK_DIALOG_MODAL,
        "_OK",
        GTK_RESPONSE_OK,
        "_Quit",
        GTK_RESPONSE_CANCEL,
        nullptr
    );

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(content), vbox);

    GtkWidget *label = gtk_label_new(
        "Please enter your sudo password.\n"
        "This will be used for installs, removals, and cleanup during this session."
    );
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_invisible_char(GTK_ENTRY(entry), 0x2022);
    gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK) {
        const char *pw = gtk_entry_get_text(GTK_ENTRY(entry));
        if (pw) {
            g_sudo_password = pw;
        }
    }

    gtk_widget_destroy(dialog);

    if (g_sudo_password.empty()) {
        gtk_window_close(GTK_WINDOW(g_main_window));
    }
}

// ───────────────────────────────────────────────
//  App setup
// ───────────────────────────────────────────────

static void activate(GtkApplication *app, gpointer) {
    g_main_window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(g_main_window), 900, 600);

    // Header bar so it feels like a proper system tool
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "COLOSSUS System Installer");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Arch + AUR (yay backend)");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(g_main_window), header);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g_main_window), vbox);

    // Top search bar
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(search_box, 6);
    gtk_widget_set_margin_bottom(search_box, 6);
    gtk_widget_set_margin_start(search_box, 6);
    gtk_widget_set_margin_end(search_box, 6);

    g_search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_search_entry),
                                   "Search packages (via yay -Ss)...");
    g_signal_connect(g_search_entry, "activate", G_CALLBACK(on_search_activated), nullptr);

    GtkWidget *search_button = gtk_button_new_with_label("Search");
    g_signal_connect_swapped(search_button, "clicked",
                             G_CALLBACK(on_search_activated),
                             g_search_entry);

    gtk_box_pack_start(GTK_BOX(search_box), g_search_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(search_box), search_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), search_box, FALSE, FALSE, 0);

    // Results list in scrolled window
    g_results_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_results_list), GTK_SELECTION_NONE);

    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_results_list);

    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    // Status bar + Clean Orphans button at bottom
    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(status_box, 4);
    gtk_widget_set_margin_bottom(status_box, 4);
    gtk_widget_set_margin_start(status_box, 8);
    gtk_widget_set_margin_end(status_box, 8);

    g_status_label = gtk_label_new("Ready. Enter a search term.");
    gtk_label_set_xalign(GTK_LABEL(g_status_label), 0.0);
    gtk_box_pack_start(GTK_BOX(status_box), g_status_label, TRUE, TRUE, 0);

    GtkWidget *clean_button = gtk_button_new_with_label("Clean Orphans");
    g_signal_connect(clean_button, "clicked",
                     G_CALLBACK(on_clean_orphans_clicked), nullptr);
    gtk_box_pack_end(GTK_BOX(status_box), clean_button, FALSE, FALSE, 0);

    gtk_box_pack_end(GTK_BOX(vbox), status_box, FALSE, FALSE, 0);

    gtk_widget_show_all(g_main_window);

    // Prompt for sudo password once
    prompt_for_sudo_password();
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(
        "tech.will.colossus.pkgcenter",
        G_APPLICATION_DEFAULT_FLAGS
    );
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

