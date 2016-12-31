/*
 * Copyright (C) 2013 Daniel Micay
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <vector>
#include <set>
#include <string>

#include <gtk/gtk.h>
#include <vte/vte.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "url_regex.hh"
#include "util/clamp.hh"
#include "util/maybe.hh"
#include "util/memory.hh"

using namespace std::placeholders;

/* Allow scales a bit smaller and a bit larger than the usual pango ranges */
#define TERMINAL_SCALE_XXX_SMALL   (PANGO_SCALE_XX_SMALL/1.2)
#define TERMINAL_SCALE_XXXX_SMALL  (TERMINAL_SCALE_XXX_SMALL/1.2)
#define TERMINAL_SCALE_XXXXX_SMALL (TERMINAL_SCALE_XXXX_SMALL/1.2)
#define TERMINAL_SCALE_XXX_LARGE   (PANGO_SCALE_XX_LARGE*1.2)
#define TERMINAL_SCALE_XXXX_LARGE  (TERMINAL_SCALE_XXX_LARGE*1.2)
#define TERMINAL_SCALE_XXXXX_LARGE (TERMINAL_SCALE_XXXX_LARGE*1.2)
#define TERMINAL_SCALE_MINIMUM     (TERMINAL_SCALE_XXXXX_SMALL/1.2)
#define TERMINAL_SCALE_MAXIMUM     (TERMINAL_SCALE_XXXXX_LARGE*1.2)

static const std::vector<double> zoom_factors = {
    TERMINAL_SCALE_MINIMUM,
    TERMINAL_SCALE_XXXXX_SMALL,
    TERMINAL_SCALE_XXXX_SMALL,
    TERMINAL_SCALE_XXX_SMALL,
    PANGO_SCALE_XX_SMALL,
    PANGO_SCALE_X_SMALL,
    PANGO_SCALE_SMALL,
    PANGO_SCALE_MEDIUM,
    PANGO_SCALE_LARGE,
    PANGO_SCALE_X_LARGE,
    PANGO_SCALE_XX_LARGE,
    TERMINAL_SCALE_XXX_LARGE,
    TERMINAL_SCALE_XXXX_LARGE,
    TERMINAL_SCALE_XXXXX_LARGE,
    TERMINAL_SCALE_MAXIMUM
};

struct config_info {
    char *browser;
    gboolean urgent_on_bell, clickable_url;
    int tag;
    char *config_file;
};

static void launch_browser(char *browser, char *url);
static gboolean key_press_cb(VteTerminal *vte, GdkEventKey *event);
static gboolean button_press_cb(VteTerminal *vte, GdkEventButton *event, const config_info *info);
static void bell_cb(GtkWidget *vte, gboolean *urgent_on_bell);
static gboolean focus_cb(GtkWindow *window);

static void window_toggle_fullscreen(GtkWindow *window);
static char *check_match(VteTerminal *vte, GdkEventButton *event);
static void load_config(GtkWindow *window, VteTerminal *vte, config_info *info);
static void set_config(GtkWindow *window, VteTerminal *vte, config_info *info, GKeyFile *config);

static std::function<void ()> reload_config;

static void override_background_color(GtkWidget *widget, GdkRGBA *rgba) {
    GtkCssProvider *provider = gtk_css_provider_new();

    gchar *colorstr = gdk_rgba_to_string(rgba);
    char *css = g_strdup_printf("* { background-color: %s; }", colorstr);
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    g_free(colorstr);
    g_free(css);

    gtk_style_context_add_provider(gtk_widget_get_style_context(widget),
                                   GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static gboolean is_fullscreen = FALSE;

void launch_browser(char *browser, char *url) {
    char *browser_cmd[3] = {browser, url, nullptr};
    GError *error = nullptr;

    if (!browser) {
        g_printerr("browser not set, can't open url\n");
        return;
    }

    GPid child_pid;
    if (!g_spawn_async(nullptr, browser_cmd, nullptr, G_SPAWN_SEARCH_PATH,
                       nullptr, nullptr, &child_pid, &error)) {
        g_printerr("error launching '%s': %s\n", browser, error->message);
        g_error_free(error);
    }
    g_spawn_close_pid(child_pid);
}

/* {{{ CALLBACKS */
static void reset_font_scale(VteTerminal *vte) {
    vte_terminal_set_font_scale(vte, 1.0);
}

static void increase_font_scale(VteTerminal *vte) {
    gdouble scale = vte_terminal_get_font_scale(vte);

    for (auto it = zoom_factors.begin(); it != zoom_factors.end(); ++it) {
        if ((*it - scale) > 1e-6) {
            vte_terminal_set_font_scale(vte, *it);
            return;
        }
    }
}

static void decrease_font_scale(VteTerminal *vte) {
    gdouble scale = vte_terminal_get_font_scale(vte);

    for (auto it = zoom_factors.rbegin(); it != zoom_factors.rend(); ++it) {
        if ((scale - *it) > 1e-6) {
            vte_terminal_set_font_scale(vte, *it);
            return;
        }
    }
}

gboolean key_press_cb(VteTerminal *vte, GdkEventKey *event) {
    const guint modifiers = event->state & gtk_accelerator_get_default_mod_mask();
    if (modifiers == (GDK_CONTROL_MASK|GDK_SHIFT_MASK)) {
        switch (gdk_keyval_to_lower(event->keyval)) {
            case GDK_KEY_plus:
                increase_font_scale(vte);
                return TRUE;
            case GDK_KEY_c:
                vte_terminal_copy_clipboard(vte);
                return TRUE;
            case GDK_KEY_v:
                vte_terminal_paste_clipboard(vte);
                return TRUE;
            case GDK_KEY_r:
                reload_config();
                return TRUE;
            case GDK_KEY_underscore:
                decrease_font_scale(vte);
                return TRUE;
            case GDK_KEY_parenright:
                reset_font_scale(vte);
                return TRUE;
        }
    } else if (event->keyval == GDK_KEY_F11) {
      window_toggle_fullscreen(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(vte))));
      return TRUE;
    }
    return FALSE;
}

gboolean button_press_cb(VteTerminal *vte, GdkEventButton *event, const config_info *info) {
    if (info->clickable_url && event->type == GDK_BUTTON_PRESS) {
        char *match = check_match(vte, event);
        if (match && event->button == 1) {
            launch_browser(info->browser, match);
            g_free(match);
            return TRUE;
        }
    }
    return FALSE;
}

static void bell_cb(GtkWidget *vte, gboolean *urgent_on_bell) {
    if (*urgent_on_bell) {
        gtk_window_set_urgency_hint(GTK_WINDOW(gtk_widget_get_toplevel(vte)), TRUE);
    }
}

gboolean focus_cb(GtkWindow *window) {
    gtk_window_set_urgency_hint(window, FALSE);
    return FALSE;
}
/* }}} */

void window_toggle_fullscreen(GtkWindow *window) {
  if(is_fullscreen) {
    gtk_window_unfullscreen(window);
  } else {
    gtk_window_fullscreen(window);
  }
  is_fullscreen = !is_fullscreen;
}

char *check_match(VteTerminal *vte, GdkEventButton *event) {
    int tag;

    return vte_terminal_match_check_event(vte, (GdkEvent*) event, &tag);
}

/* {{{ CONFIG LOADING */
template<typename T>
maybe<T> get_config(T (*get)(GKeyFile *, const char *, const char *, GError **),
                    GKeyFile *config, const char *group, const char *key) {
    GError *error = nullptr;
    maybe<T> value = get(config, group, key, &error);
    if (error) {
        g_error_free(error);
        return {};
    }
    return value;
}

auto get_config_integer(std::bind(get_config<int>, g_key_file_get_integer,
                                  _1, _2, _3));
auto get_config_string(std::bind(get_config<char *>, g_key_file_get_string,
                                 _1, _2, _3));
auto get_config_double(std::bind(get_config<double>, g_key_file_get_double,
                                 _1, _2, _3));

static maybe<GdkRGBA> get_config_color(GKeyFile *config, const char *section, const char *key) {
    if (auto s = get_config_string(config, section, key)) {
        GdkRGBA color;
        if (gdk_rgba_parse(&color, *s)) {
            g_free(*s);
            return color;
        }
        g_printerr("invalid color string: %s\n", *s);
        g_free(*s);
    }
    return {};
}

static void load_theme(GtkWindow *window, VteTerminal *vte, GKeyFile *config) {
    std::array<GdkRGBA, 256> palette;
    char color_key[] = "color000";

    for (unsigned i = 0; i < palette.size(); i++) {
        snprintf(color_key, sizeof(color_key), "color%u", i);
        if (auto color = get_config_color(config, "colors", color_key)) {
            palette[i] = *color;
        } else if (i < 16) {
            palette[i].blue = (((i & 4) ? 0xc000 : 0) + (i > 7 ? 0x3fff: 0)) / 65535.0;
            palette[i].green = (((i & 2) ? 0xc000 : 0) + (i > 7 ? 0x3fff : 0)) / 65535.0;
            palette[i].red = (((i & 1) ? 0xc000 : 0) + (i > 7 ? 0x3fff : 0)) / 65535.0;
            palette[i].alpha = 0;
        } else if (i < 232) {
            const unsigned j = i - 16;
            const unsigned r = j / 36, g = (j / 6) % 6, b = j % 6;
            const unsigned red =   (r == 0) ? 0 : r * 40 + 55;
            const unsigned green = (g == 0) ? 0 : g * 40 + 55;
            const unsigned blue =  (b == 0) ? 0 : b * 40 + 55;
            palette[i].red   = (red | red << 8) / 65535.0;
            palette[i].green = (green | green << 8) / 65535.0;
            palette[i].blue  = (blue | blue << 8) / 65535.0;
            palette[i].alpha = 0;
        } else if (i < 256) {
            const unsigned shade = 8 + (i - 232) * 10;
            palette[i].red = palette[i].green = palette[i].blue = (shade | shade << 8) / 65535.0;
            palette[i].alpha = 0;
        }
    }

    vte_terminal_set_colors(vte, nullptr, nullptr, palette.data(), palette.size());
    if (auto color = get_config_color(config, "colors", "foreground")) {
        vte_terminal_set_color_foreground(vte, &*color);
        vte_terminal_set_color_bold(vte, &*color);
    }
    if (auto color = get_config_color(config, "colors", "foreground_bold")) {
        vte_terminal_set_color_bold(vte, &*color);
    }
    if (auto color = get_config_color(config, "colors", "background")) {
        vte_terminal_set_color_background(vte, &*color);
        override_background_color(GTK_WIDGET(window), &*color);
    }
    if (auto color = get_config_color(config, "colors", "cursor")) {
        vte_terminal_set_color_cursor(vte, &*color);
    }
    if (auto color = get_config_color(config, "colors", "cursor_foreground")) {
        vte_terminal_set_color_cursor_foreground(vte, &*color);
    }
    if (auto color = get_config_color(config, "colors", "highlight")) {
        vte_terminal_set_color_highlight(vte, &*color);
    }
}

static void load_config(GtkWindow *window, VteTerminal *vte, config_info *info) {
    const std::string default_path = "/termite/config";
    GKeyFile *config = g_key_file_new();

    gboolean loaded = FALSE;

    if (info->config_file) {
        loaded = g_key_file_load_from_file(config,
                                           info->config_file,
                                           G_KEY_FILE_NONE, nullptr);
    }

    if (!loaded) {
        loaded = g_key_file_load_from_file(config,
                                           (g_get_user_config_dir() + default_path).c_str(),
                                           G_KEY_FILE_NONE, nullptr);
    }

    for (const char *const *dir = g_get_system_config_dirs();
         !loaded && *dir; dir++) {
        loaded = g_key_file_load_from_file(config, (*dir + default_path).c_str(),
                                           G_KEY_FILE_NONE, nullptr);
    }

    if (loaded) {
        set_config(window, vte, info, config);
    }
    g_key_file_free(config);
}

static void set_config(GtkWindow *window, VteTerminal *vte, config_info *info,
                       GKeyFile *config) {
    auto cfg_bool = [config](const char *key, gboolean value) {
        return get_config<gboolean>(g_key_file_get_boolean,
                                    config, "options", key).get_value_or(value);
    };

    vte_terminal_set_scroll_on_output(vte, cfg_bool("scroll_on_output", FALSE));
    vte_terminal_set_scroll_on_keystroke(vte, cfg_bool("scroll_on_keystroke", TRUE));
    vte_terminal_set_audible_bell(vte, cfg_bool("audible_bell", FALSE));
    vte_terminal_set_mouse_autohide(vte, cfg_bool("mouse_autohide", FALSE));
    vte_terminal_set_allow_bold(vte, cfg_bool("allow_bold", TRUE));
    info->urgent_on_bell = cfg_bool("urgent_on_bell", TRUE);
    info->clickable_url = cfg_bool("clickable_url", TRUE);

    g_free(info->browser);
    info->browser = nullptr;

    if (auto s = get_config_string(config, "options", "browser")) {
        info->browser = *s;
    } else {
        info->browser = g_strdup(g_getenv("BROWSER"));
    }

    if (!info->browser) {
        info->browser = g_strdup("xdg-open");
    }

    if (info->clickable_url) {
        info->tag = vte_terminal_match_add_regex(vte,
                vte_regex_new_for_match(url_regex,
                                        (gssize) strlen(url_regex),
                                        PCRE2_MULTILINE | PCRE2_NOTEMPTY,
                                        nullptr),
                0);
        vte_terminal_match_set_cursor_type(vte, info->tag, GDK_HAND2);
    } else if (info->tag != -1) {
        vte_terminal_match_remove(vte, info->tag);
        info->tag = -1;
    }

    if (auto s = get_config_string(config, "options", "font")) {
        PangoFontDescription *font = pango_font_description_from_string(*s);
        vte_terminal_set_font(vte, font);
        pango_font_description_free(font);
        g_free(*s);
    }

    if (auto i = get_config_integer(config, "options", "scrollback_lines")) {
        vte_terminal_set_scrollback_lines(vte, *i);
    }

    if (auto s = get_config_string(config, "options", "cursor_blink")) {
        if (!g_ascii_strcasecmp(*s, "system")) {
            vte_terminal_set_cursor_blink_mode(vte, VTE_CURSOR_BLINK_SYSTEM);
        } else if (!g_ascii_strcasecmp(*s, "on")) {
            vte_terminal_set_cursor_blink_mode(vte, VTE_CURSOR_BLINK_ON);
        } else if (!g_ascii_strcasecmp(*s, "off")) {
            vte_terminal_set_cursor_blink_mode(vte, VTE_CURSOR_BLINK_OFF);
        }
        g_free(*s);
    }

    if (auto s = get_config_string(config, "options", "cursor_shape")) {
        if (!g_ascii_strcasecmp(*s, "block")) {
            vte_terminal_set_cursor_shape(vte, VTE_CURSOR_SHAPE_BLOCK);
        } else if (!g_ascii_strcasecmp(*s, "ibeam")) {
            vte_terminal_set_cursor_shape(vte, VTE_CURSOR_SHAPE_IBEAM);
        } else if (!g_ascii_strcasecmp(*s, "underline")) {
            vte_terminal_set_cursor_shape(vte, VTE_CURSOR_SHAPE_UNDERLINE);
        }
        g_free(*s);
    }

    load_theme(window, vte, config);
}/*}}}*/

static void exit_with_status(VteTerminal *, int status) {
    gtk_main_quit();
    exit(WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE);
}

static void exit_with_success(VteTerminal *) {
    gtk_main_quit();
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
    GError *error = nullptr;
    const char *const term = "xterm-256color";
    char *directory = nullptr;
    gboolean version = FALSE, hold = FALSE;

    GOptionContext *context = g_option_context_new(nullptr);
    char *role = nullptr, *execute = nullptr, *config_file = nullptr;
    const GOptionEntry entries[] = {
        {"version", 'v', 0, G_OPTION_ARG_NONE, &version, "Version info", nullptr},
        {"exec", 'e', 0, G_OPTION_ARG_STRING, &execute, "Command to execute", "COMMAND"},
        {"role", 'r', 0, G_OPTION_ARG_STRING, &role, "The role to use", "ROLE"},
        {"directory", 'd', 0, G_OPTION_ARG_STRING, &directory, "Change to directory", "DIRECTORY"},
        {"hold", 0, 0, G_OPTION_ARG_NONE, &hold, "Remain open after child process exits", nullptr},
        {"config", 'c', 0, G_OPTION_ARG_STRING, &config_file, "Path of config file", "CONFIG"},
        {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr}
    };
    g_option_context_add_main_entries(context, entries, nullptr);
    g_option_context_add_group(context, gtk_get_option_group(TRUE));

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("option parsing failed: %s\n", error->message);
        g_clear_error (&error);
        return EXIT_FAILURE;
    }

    g_option_context_free(context);

    if (version) {
        g_print("termite %s\n", TERMITE_VERSION);
        return EXIT_SUCCESS;
    }

    if (directory) {
        if (chdir(directory) == -1) {
            perror("chdir");
            return EXIT_FAILURE;
        }
        g_free(directory);
    }

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    GtkWidget *vte_widget = vte_terminal_new();
    VteTerminal *vte = VTE_TERMINAL(vte_widget);

    if (role) {
        gtk_window_set_role(GTK_WINDOW(window), role);
        g_free(role);
    }

    char **command_argv;
    char *default_argv[2] = {nullptr, nullptr};

    if (execute) {
        int argcp;
        char **argvp;
        g_shell_parse_argv(execute, &argcp, &argvp, &error);
        if (error) {
            g_printerr("failed to parse command: %s\n", error->message);
            return EXIT_FAILURE;
        }
        command_argv = argvp;
    } else {
        default_argv[0] = vte_get_user_shell();
        command_argv = default_argv;
    }

    config_info info {
         nullptr, FALSE, FALSE, -1, config_file
    };

    load_config(GTK_WINDOW(window), vte, &info);

    reload_config = [&]{
        load_config(GTK_WINDOW(window), vte, &info);
    };
    signal(SIGUSR1, [](int){ reload_config(); });

    gtk_container_add(GTK_CONTAINER(window), vte_widget);

    if (!hold) {
        g_signal_connect(vte, "child-exited", G_CALLBACK(exit_with_status), nullptr);
    }
    g_signal_connect(window, "destroy", G_CALLBACK(exit_with_success), nullptr);
    g_signal_connect(vte, "key-press-event", G_CALLBACK(key_press_cb), &info);
    g_signal_connect(vte, "button-press-event", G_CALLBACK(button_press_cb), &info);
    g_signal_connect(vte, "bell", G_CALLBACK(bell_cb), &info.urgent_on_bell);

    g_signal_connect(window, "focus-in-event",  G_CALLBACK(focus_cb), nullptr);
    g_signal_connect(window, "focus-out-event", G_CALLBACK(focus_cb), nullptr);

    gtk_widget_grab_focus(vte_widget);
    gtk_widget_show_all(window);

    char **env = g_get_environ();

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_SCREEN(gtk_widget_get_screen(window))) {
        GdkWindow *gdk_window = gtk_widget_get_window(window);
        if (!gdk_window) {
            g_printerr("no window\n");
            return EXIT_FAILURE;
        }
        char xid_s[std::numeric_limits<long unsigned>::digits10 + 1];
        snprintf(xid_s, sizeof(xid_s), "%lu", GDK_WINDOW_XID(gdk_window));
        env = g_environ_setenv(env, "WINDOWID", xid_s, TRUE);
    }
#endif

    env = g_environ_setenv(env, "TERM", term, TRUE);

    GPid child_pid;
    if (vte_terminal_spawn_sync(vte, VTE_PTY_DEFAULT, nullptr, command_argv, env,
                                G_SPAWN_SEARCH_PATH, nullptr, nullptr, &child_pid, nullptr,
                                &error)) {
        vte_terminal_watch_child(vte, child_pid);
    } else {
        g_printerr("the command failed to run: %s\n", error->message);
        return EXIT_FAILURE;
    }

    int width, height;
    const long char_width = vte_terminal_get_char_width(vte);
    const long char_height = vte_terminal_get_char_height(vte);

    gtk_window_get_size(GTK_WINDOW(window), &width, &height);
    vte_terminal_set_size(vte, width / char_width, height / char_height);

    g_strfreev(env);

    gtk_main();
    return EXIT_FAILURE; // child process did not cause termination
}

// vim: et:sts=4:sw=4:cino=(0:cc=100
