/*
 * PokeSim -- a Pokemon battle simulator with a GTK front end.
 *
 * The loader lives in pokesim_data.c. This file is the part you
 * look at: a browsable Pokedex with search, filtering, sortable columns, a
 * detail panel, and the full type chart.
 *
 *     pokesim              open the window
 *     pokesim --report     the console data summary
 *     pokesim --test       run the self-tests
 *
 * There are still no battles. This is the shell the tournament results will
 * eventually live in, built now because browsing the data by hand is the
 * fastest way to notice when something about it is wrong.
 *
 * The GUI is event-driven: instead of our code deciding what happens next,
 * GTK calls our callbacks when the user types, clicks or selects. That is the
 * main shift from a console program, and it is why so much state has to be
 * bundled into a struct that the callbacks can reach.
 *
 * Author: Ricky
 * Created: 2026-09-16
 */

#include "pokesim_data.h"
#include "pokesim_battle.h"

#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Columns in the list store behind the table. */
enum {
    COL_DEX, COL_NAME, COL_TYPE1, COL_TYPE2,
    COL_HP, COL_ATK, COL_DEF, COL_SPA, COL_SPD, COL_SPE, COL_TOTAL,
    COL_INDEX,          /* index into roster[] -- hidden from the user */
    COL_ICON,           /* 16x16 sprite; appended last so every column above
                           keeps the number it already had */
    N_COLUMNS
};

enum { MOVE_COL_LEVEL, MOVE_COL_NAME, MOVE_N_COLUMNS };

/*
 * The window opens filling the screen's work area -- the desktop minus the
 * taskbar -- as reported by the system, and can be resized freely. The table
 * takes whatever width is left after the detail panel, so a bigger screen
 * simply shows more of it.
 *
 * MIN_* is the smallest the layout still works at, not a target.
 */
#define MIN_WIDTH      1000
#define MIN_HEIGHT      640
#define PANEL_WIDTH     380
#define TABLE_WIDTH     520     /* minimum, not fixed: the table expands */

/* Sprites: 1025 32x32 PNGs named NNNN_slug.png, one per dex number. */
#define SPRITE_DIR    "sprites"
#define SPRITE_LARGE   96      /* detail panel, scaled 3x from 32x32 */
#define SPRITE_COLUMN  38      /* table column: 32px plus a little padding */

/* Window and taskbar icon. */
#define APP_ICON      "simball.png"

static const char *STAT_NAMES[6] = {
    "HP", "Attack", "Defense", "Sp. Atk", "Sp. Def", "Speed"
};

/*
 * Everything the callbacks need. GTK hands a callback exactly one user_data
 * pointer, so the normal pattern is to bundle the widgets and the data into
 * one struct and pass its address.
 */
typedef struct {
    Pokemon *roster;
    int      count;
    double (*chart)[TYPE_COUNT];

    GtkListStore *store;
    GtkTreeModel *filter;
    GtkWidget    *tree;
    GtkWidget    *search;
    GtkWidget    *type_combo;
    GtkWidget    *count_label;

    /* Detail panel */
    GtkWidget    *detail_stack;      /* swaps placeholder <-> details */
    GtkWidget    *detail_sprite;
    GtkWidget    *detail_name;
    GtkWidget    *detail_types;
    GtkWidget    *detail_total;
    GtkWidget    *stat_bar[6];
    GtkWidget    *stat_value[6];
    GtkWidget    *weak_label;
    GtkWidget    *resist_label;
    GtkWidget    *immune_label;
    GtkListStore *move_store;
    GtkWidget    *move_count_label;

    /* Current filter settings, kept lowercased for cheap comparison. */
    char search_text[NAME_LEN];
    int  type_filter;                /* TYPE_NONE means "all types" */

    /* --- Duel tab --- */
    GtkWidget    *duel_entry[2];
    GtkWidget    *duel_sprite[2];
    GtkWidget    *duel_name[2];
    GtkWidget    *duel_types[2];
    GtkWidget    *duel_moves[2];
    GtkWidget    *duel_stat[2][6];   /* one label per stat, per side */
    GtkWidget    *duel_stat_bar[2][6];
    GtkWidget    *duel_total[2];
    GtkWidget    *duel_runs;
    GtkWidget    *duel_result;
    GtkWidget    *duel_detail;
    GtkWidget    *duel_bar;

    /* --- Ranking tab --- */
    GtkWidget    *rank_button;
    GtkWidget    *rank_runs;
    GtkWidget    *rank_progress;
    GtkWidget    *rank_status;
    GtkWidget    *rank_search;
    GtkWidget    *rank_type_combo;
    GtkListStore *rank_store;
    GtkTreeModel *rank_filter;
    GtkWidget    *rank_tree;
    char          rank_search_text[NAME_LEN];
    int           rank_type_filter;

    /* Worker state. `running` is read by the GUI thread and written by it
     * too; the worker only ever touches `progress` and `cancel`. */
    GThread      *worker;
    volatile int  cancel;
    volatile int  worker_done;
    double        progress;
    int           worker_runs;
    double        worker_seconds;
    RankEntry    *results;
} AppState;

/* Columns of the ranking table. */
enum {
    RK_RANK, RK_ICON, RK_NAME, RK_TYPE1, RK_TYPE2,
    RK_WINS, RK_LOSSES, RK_DRAWS, RK_WINPCT, RK_TOTAL,
    RK_AVGTURNS, RK_DMGRATIO, RK_INDEX, RK_N
};

/*
 * The Duel and Ranking pages are built further down, after the widgets they
 * rely on, but activate() needs to know about them up here.
 */
static GtkWidget *build_duel_page(AppState *state);
static GtkWidget *build_rank_page(AppState *state);
static void       duel_refresh(AppState *state);
static void       on_duel_run(GtkButton *button, gpointer data);
static void       on_rank_run(GtkButton *button, gpointer data);

static void get_stats(const Pokemon *p, int out[6])
{
    out[0] = p->hp;     out[1] = p->attack; out[2] = p->defense;
    out[3] = p->sp_atk; out[4] = p->sp_def; out[5] = p->speed;
}

/* ------------------------------------------------------------------ *
 * Sprites
 * ------------------------------------------------------------------ */

/*
 * Sprite files are named NNNN_slug.png and we only know the dex number, not
 * the slug -- so the directory is scanned once and the leading digits of each
 * filename build a dex -> path table. A renamed or missing file then degrades
 * to "no icon" rather than to a wrong one.
 *
 * All 1025 images together are well under a megabyte, so they are cached for
 * the life of the program and scrolling never touches the disk again.
 */
static char       sprite_path[MAX_POKEMON + 1][256];
static GdkPixbuf *sprite_cache[MAX_POKEMON + 1];

static void sprites_init(void)
{
    /* Same fallbacks as the CSVs, so running from build/ still works. */
    static const char *candidates[] = {
        SPRITE_DIR, "../" SPRITE_DIR, "../../" SPRITE_DIR
    };

    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        GDir *dir = g_dir_open(candidates[i], 0, NULL);
        if (dir == NULL) {
            continue;
        }
        const char *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            int dex = atoi(name);       /* "0595_joltik.png" -> 595 */
            if (dex >= 1 && dex <= MAX_POKEMON) {
                g_snprintf(sprite_path[dex], sizeof sprite_path[dex],
                           "%s/%s", candidates[i], name);
            }
        }
        g_dir_close(dir);
        return;
    }

    g_printerr("No %s/ directory found -- the table will have no icons.\n",
               SPRITE_DIR);
}

/*
 * The window and taskbar icon. Setting it as the *default* icon rather than on
 * one window means every window the program opens picks it up, including
 * dialogs, without having to be told.
 */
static void load_app_icon(void)
{
    static const char *candidates[] = {
        APP_ICON, "../" APP_ICON, "../../" APP_ICON
    };

    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        GdkPixbuf *icon = gdk_pixbuf_new_from_file(candidates[i], NULL);
        if (icon != NULL) {
            gtk_window_set_default_icon(icon);
            g_object_unref(icon);
            return;
        }
    }
    g_printerr("No %s found -- the window will use the default icon.\n", APP_ICON);
}

/* The cached 16x16 sprite for a dex number, or NULL if there is not one. */
static GdkPixbuf *sprite_for(int dex)
{
    if (dex < 1 || dex > MAX_POKEMON || sprite_path[dex][0] == '\0') {
        return NULL;
    }
    if (sprite_cache[dex] == NULL) {
        sprite_cache[dex] = gdk_pixbuf_new_from_file(sprite_path[dex], NULL);
    }
    return sprite_cache[dex];
}

/* ------------------------------------------------------------------ *
 * Styling
 * ------------------------------------------------------------------ */

/*
 * GTK is styled with CSS, much like a web page. Loading it into the default
 * screen means every widget in the app can use these classes.
 */
static void load_css(void)
{
    /*
     * Every widget that draws its own background needs its foreground set to
     * match. A blanket "label { color: white }" looks fine until you notice
     * the buttons and notebook tabs, which keep their light backgrounds and
     * end up white-on-white -- so those get styled explicitly here rather
     * than inheriting.
     */
    static const char *css =
        "window, notebook, notebook > stack { background-color: #1e2228; }\n"
        "label { color: #e6e6e6; }\n"
        ".panel { background-color: #262b33; border-radius: 8px; }\n"
        ".title { font-size: 20px; font-weight: bold; }\n"
        ".subtle { color: #9aa4b2; font-size: 11px; }\n"
        ".heading { font-weight: bold; color: #c7d0dd; }\n"

        /* Tabs: dark strip, dim labels, bright label on the active tab. */
        "notebook header { background-color: #262b33; border-color: #39404a; }\n"
        "notebook tab { background-color: transparent; padding: 6px 16px; }\n"
        "notebook tab label { color: #9aa4b2; }\n"
        "notebook tab:checked { background-color: #2f3640; }\n"
        "notebook tab:checked label { color: #ffffff; font-weight: bold; }\n"

        /* Controls, which would otherwise stay light-themed. */
        "button { background-image: none; background-color: #39404a;\n"
        "         color: #e6e6e6; border: 1px solid #4a525e; padding: 4px 14px; }\n"
        "button:hover { background-color: #47505c; }\n"
        "button:disabled { color: #6b7480; }\n"
        "entry { background-image: none; background-color: #2b313a;\n"
        "        color: #e6e6e6; border: 1px solid #4a525e; }\n"
        "entry image, entry placeholder { color: #8b94a1; }\n"
        "combobox button { padding: 4px 8px; }\n"

        /* Stat bars, coloured by how good the stat is. */
        "progressbar trough { min-height: 10px; background-color: #39404a; }\n"
        "progressbar progress { min-height: 10px; }\n"
        "progressbar.s-low  progress { background-color: #e05a4f; }\n"
        "progressbar.s-mid  progress { background-color: #e8a33d; }\n"
        "progressbar.s-high progress { background-color: #9bd14f; }\n"
        "progressbar.s-max  progress { background-color: #3fb950; }\n"

        "treeview { background-color: #262b33; color: #e6e6e6; }\n"
        "treeview:selected { background-color: #3d6fb5; color: #ffffff; }\n"
        "treeview header button { background-color: #2f3640; color: #9aa4b2;\n"
        "                         border: 0; padding: 4px; }\n"
        "scrollbar { background-color: #262b33; }\n"
        "separator { background-color: #39404a; }\n"
        /* The percentage sits on top of the filled bar, so it needs a colour
         * that reads against both the filled and unfilled halves. */
        "progressbar text { color: #e6e6e6; font-size: 11px; }\n"
        "spinbutton { background-color: #2b313a; color: #e6e6e6; }\n";

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/*
 * A coloured "pill" for a type, built with Pango markup rather than CSS
 * classes. Markup is far less fiddly here: the colour changes with every
 * selection, and swapping style classes on a label each time would mean
 * removing the old one first, which is easy to get wrong.
 */
static void append_type_badge(GString *out, int type)
{
    if (type == TYPE_NONE) {
        return;
    }
    g_string_append_printf(
        out, "<span background=\"%s\" foreground=\"#12151a\" size=\"small\">"
             "  %s  </span>  ",
        TYPE_COLOURS[type], TYPE_NAMES[type]);
}

/* Colour a stat bar by how good the stat is, and fill it proportionally. */
static void set_stat_bar(GtkWidget *bar, int value)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context(bar);
    gtk_style_context_remove_class(ctx, "s-low");
    gtk_style_context_remove_class(ctx, "s-mid");
    gtk_style_context_remove_class(ctx, "s-high");
    gtk_style_context_remove_class(ctx, "s-max");

    const char *class_name = "s-max";
    if (value < 60) {
        class_name = "s-low";
    } else if (value < 90) {
        class_name = "s-mid";
    } else if (value < 120) {
        class_name = "s-high";
    }
    gtk_style_context_add_class(ctx, class_name);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar),
                                  (double)value / MAX_SINGLE_STAT);
}

/* ------------------------------------------------------------------ *
 * Filtering
 * ------------------------------------------------------------------ */

/*
 * Called by GTK for every row to decide whether it should be shown. Note it
 * receives the *underlying* store and an iter into it, which is why the
 * hidden COL_INDEX is so useful: one integer gets us straight back to the
 * real Pokemon without copying any strings.
 */
static gboolean row_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    AppState *state = data;

    int index = -1;
    gtk_tree_model_get(model, iter, COL_INDEX, &index, -1);
    if (index < 0 || index >= state->count) {
        return FALSE;
    }
    const Pokemon *p = &state->roster[index];

    if (state->type_filter != TYPE_NONE &&
        p->type1 != state->type_filter && p->type2 != state->type_filter) {
        return FALSE;
    }

    if (state->search_text[0] != '\0') {
        char lowered[NAME_LEN];
        snprintf(lowered, sizeof lowered, "%s", p->name);
        for (char *c = lowered; *c != '\0'; c++) {
            *c = (char)g_ascii_tolower(*c);
        }
        if (strstr(lowered, state->search_text) == NULL) {
            return FALSE;
        }
    }

    return TRUE;
}

static void update_count_label(AppState *state)
{
    int visible = gtk_tree_model_iter_n_children(state->filter, NULL);
    char text[128];
    if (visible == state->count) {
        snprintf(text, sizeof text, "%d species", state->count);
    } else {
        snprintf(text, sizeof text, "%d of %d species", visible, state->count);
    }
    gtk_label_set_text(GTK_LABEL(state->count_label), text);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data)
{
    AppState   *state = data;
    const char *text  = gtk_entry_get_text(GTK_ENTRY(entry));

    snprintf(state->search_text, sizeof state->search_text, "%s", text);
    for (char *c = state->search_text; *c != '\0'; c++) {
        *c = (char)g_ascii_tolower(*c);
    }

    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(state->filter));
    update_count_label(state);
}

static void on_type_changed(GtkComboBox *combo, gpointer data)
{
    AppState *state = data;
    int       active = gtk_combo_box_get_active(combo);

    /* Row 0 is "All types", so every real type sits one row further down. */
    state->type_filter = (active <= 0) ? TYPE_NONE : active - 1;

    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(state->filter));
    update_count_label(state);
}

static void on_clear_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    AppState *state = data;
    gtk_entry_set_text(GTK_ENTRY(state->search), "");
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->type_combo), 0);
}

/* ------------------------------------------------------------------ *
 * Detail panel
 * ------------------------------------------------------------------ */

static void show_pokemon(AppState *state, const Pokemon *p)
{
    gtk_label_set_text(GTK_LABEL(state->detail_name), p->name);

    /*
     * Scaled with NEAREST, not a smooth filter: these are 16x16 pixel art, and
     * interpolating up to 64x64 turns crisp pixels into mush.
     */
    GdkPixbuf *small_sprite = sprite_for(p->dex);
    if (small_sprite != NULL) {
        GdkPixbuf *big = gdk_pixbuf_scale_simple(small_sprite, SPRITE_LARGE,
                                                 SPRITE_LARGE, GDK_INTERP_NEAREST);
        gtk_image_set_from_pixbuf(GTK_IMAGE(state->detail_sprite), big);
        g_object_unref(big);
    } else {
        gtk_image_clear(GTK_IMAGE(state->detail_sprite));
    }

    GString *types = g_string_new(NULL);
    append_type_badge(types, p->type1);
    append_type_badge(types, p->type2);
    gtk_label_set_markup(GTK_LABEL(state->detail_types), types->str);
    g_string_free(types, TRUE);

    char total[64];
    snprintf(total, sizeof total, "Base stat total  %d", p->total);
    gtk_label_set_text(GTK_LABEL(state->detail_total), total);

    int stats[6];
    get_stats(p, stats);
    for (int i = 0; i < 6; i++) {
        char value[16];
        snprintf(value, sizeof value, "%d", stats[i]);
        gtk_label_set_text(GTK_LABEL(state->stat_value[i]), value);
        set_stat_bar(state->stat_bar[i], stats[i]);
    }

    /*
     * What this Pokemon is weak to, resists, or ignores entirely -- computed
     * live from the type chart rather than stored anywhere. This is the first
     * thing in the project that actually *uses* the chart for something a
     * person would want to know.
     */
    GString *weak   = g_string_new(NULL);
    GString *resist = g_string_new(NULL);
    GString *immune = g_string_new(NULL);

    for (int t = 0; t < TYPE_COUNT; t++) {
        double multiplier = effectiveness(state->chart, t, p);
        if (multiplier == 0.0) {
            append_type_badge(immune, t);
        } else if (multiplier > 1.0) {
            append_type_badge(weak, t);
            if (multiplier == 4.0) {
                g_string_append(weak, "<span foreground=\"#e05a4f\">4x</span>  ");
            }
        } else if (multiplier < 1.0) {
            append_type_badge(resist, t);
            if (multiplier == 0.25) {
                g_string_append(resist, "<span foreground=\"#9bd14f\">1/4</span>  ");
            }
        }
    }

    gtk_label_set_markup(GTK_LABEL(state->weak_label),
                         weak->len   ? weak->str   : "<span foreground=\"#9aa4b2\">nothing</span>");
    gtk_label_set_markup(GTK_LABEL(state->resist_label),
                         resist->len ? resist->str : "<span foreground=\"#9aa4b2\">nothing</span>");
    gtk_label_set_markup(GTK_LABEL(state->immune_label),
                         immune->len ? immune->str : "<span foreground=\"#9aa4b2\">nothing</span>");

    g_string_free(weak, TRUE);
    g_string_free(resist, TRUE);
    g_string_free(immune, TRUE);

    gtk_list_store_clear(state->move_store);
    for (int m = 0; m < p->move_count; m++) {
        GtkTreeIter iter;
        gtk_list_store_append(state->move_store, &iter);
        gtk_list_store_set(state->move_store, &iter,
                           MOVE_COL_LEVEL, p->moves[m].level,
                           MOVE_COL_NAME,  p->moves[m].name,
                           -1);
    }

    char moves[96];
    if (p->move_count == 1) {
        /* Worth calling out: these are the species that can never win. */
        snprintf(moves, sizeof moves, "1 level-up move -- this one cannot win a fight");
    } else {
        snprintf(moves, sizeof moves, "%d level-up moves", p->move_count);
    }
    gtk_label_set_text(GTK_LABEL(state->move_count_label), moves);

    gtk_stack_set_visible_child_name(GTK_STACK(state->detail_stack), "details");
}

static void on_selection_changed(GtkTreeSelection *selection, gpointer data)
{
    AppState     *state = data;
    GtkTreeModel *model = NULL;
    GtkTreeIter   iter;

    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gtk_stack_set_visible_child_name(GTK_STACK(state->detail_stack), "empty");
        return;
    }

    int index = -1;
    gtk_tree_model_get(model, &iter, COL_INDEX, &index, -1);
    if (index >= 0 && index < state->count) {
        show_pokemon(state, &state->roster[index]);
    }
}

/* ------------------------------------------------------------------ *
 * Building the window
 * ------------------------------------------------------------------ */

static GtkWidget *make_stat_row(AppState *state, GtkWidget *grid, int row)
{
    GtkWidget *name = gtk_label_new(STAT_NAMES[row]);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_widget_set_size_request(name, 70, -1);
    gtk_grid_attach(GTK_GRID(grid), name, 0, row, 1, 1);

    GtkWidget *bar = gtk_progress_bar_new();
    gtk_widget_set_hexpand(bar, TRUE);
    gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), bar, 1, row, 1, 1);
    state->stat_bar[row] = bar;

    GtkWidget *value = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(value), 1.0);
    gtk_widget_set_size_request(value, 40, -1);
    gtk_grid_attach(GTK_GRID(grid), value, 2, row, 1, 1);
    state->stat_value[row] = value;

    return bar;
}

/* One labelled row of coloured type pills in the matchup section. */
static GtkWidget *make_matchup_row(GtkWidget *box, const char *heading)
{
    GtkWidget *title = gtk_label_new(heading);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "heading");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    GtkWidget *value = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(value), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(value), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(value), 30);
    gtk_box_pack_start(GTK_BOX(box), value, FALSE, FALSE, 0);

    return value;
}

/*
 * Columns are given explicit fixed widths rather than being left to size
 * themselves. Auto-sized columns take as much width as their widest cell
 * needs, which makes the whole table's minimum width larger than the window
 * and quietly shoves the detail panel off the right-hand edge.
 */
static void add_column(GtkWidget *tree, const char *title, int column,
                       int width, gboolean right_align)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    if (right_align) {
        g_object_set(renderer, "xalign", 1.0, NULL);
    }
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        title, renderer, "text", column, NULL);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col, width);
    gtk_tree_view_column_set_sort_column_id(col, column);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
}

static void add_number_column(GtkWidget *tree, const char *title, int column)
{
    add_column(tree, title, column, 62, TRUE);
}

/*
 * A double stored in the model renders as its full precision -- 95.676491 --
 * which is unreadable in a table. Binding a cell data function instead lets
 * the column keep the real number for sorting while showing a rounded one.
 * The suffix ("%" or nothing) rides along in the user data.
 */
static void format_decimal(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                           GtkTreeModel *model, GtkTreeIter *iter,
                           gpointer data)
{
    (void)col;
    int column = GPOINTER_TO_INT(data);
    int percent = column < 0;
    if (percent) {
        column = -column - 1;
    }

    double value = 0.0;
    gtk_tree_model_get(model, iter, column, &value, -1);

    char text[32];
    snprintf(text, sizeof text, percent ? "%.1f%%" : "%.2f", value);
    g_object_set(cell, "text", text, NULL);
}

static void add_formatted_column(GtkWidget *tree, const char *title,
                                 int column, int percent)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xalign", 1.0, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        title, renderer, NULL);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col, 70);
    gtk_tree_view_column_set_sort_column_id(col, column);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_cell_data_func(
        col, renderer, format_decimal,
        GINT_TO_POINTER(percent ? -(column + 1) : column), NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
}

static void add_decimal_column(GtkWidget *tree, const char *title, int column)
{
    add_formatted_column(tree, title, column, 0);
}

static void add_percent_column(GtkWidget *tree, const char *title, int column)
{
    add_formatted_column(tree, title, column, 1);
}

/* The sprite column: no title, no sorting, just the icon. */
static void add_icon_column(GtkWidget *tree, int column)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "", renderer, "pixbuf", column, NULL);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col, SPRITE_COLUMN);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
}

static void add_text_column(GtkWidget *tree, const char *title, int column)
{
    add_column(tree, title, column, 110, FALSE);
}

/* The Pokedex page: filters on top, table on the left, details on the right. */
static GtkWidget *build_dex_page(AppState *state)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    /* --- filter bar --- */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(page), bar, FALSE, FALSE, 0);

    state->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->search), "Search by name...");
    gtk_widget_set_hexpand(state->search, TRUE);
    gtk_box_pack_start(GTK_BOX(bar), state->search, TRUE, TRUE, 0);

    state->type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->type_combo), "All types");
    for (int t = 0; t < TYPE_COUNT; t++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->type_combo),
                                       TYPE_NAMES[t]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->type_combo), 0);
    gtk_box_pack_start(GTK_BOX(bar), state->type_combo, FALSE, FALSE, 0);

    GtkWidget *clear = gtk_button_new_with_label("Clear");
    gtk_box_pack_start(GTK_BOX(bar), clear, FALSE, FALSE, 0);

    state->count_label = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(state->count_label),
                                "subtle");
    gtk_box_pack_start(GTK_BOX(bar), state->count_label, FALSE, FALSE, 6);

    /* --- the table --- */
    state->store = gtk_list_store_new(N_COLUMNS,
                                      G_TYPE_INT,    G_TYPE_STRING,
                                      G_TYPE_STRING, G_TYPE_STRING,
                                      G_TYPE_INT, G_TYPE_INT, G_TYPE_INT,
                                      G_TYPE_INT, G_TYPE_INT, G_TYPE_INT,
                                      G_TYPE_INT, G_TYPE_INT,
                                      GDK_TYPE_PIXBUF);

    for (int i = 0; i < state->count; i++) {
        const Pokemon *p = &state->roster[i];
        GtkTreeIter iter;
        gtk_list_store_append(state->store, &iter);
        gtk_list_store_set(state->store, &iter,
                           COL_DEX,   p->dex,
                           COL_NAME,  p->name,
                           COL_TYPE1, type_name(p->type1),
                           COL_TYPE2, (p->type2 == TYPE_NONE) ? "" : type_name(p->type2),
                           COL_HP,    p->hp,
                           COL_ATK,   p->attack,
                           COL_DEF,   p->defense,
                           COL_SPA,   p->sp_atk,
                           COL_SPD,   p->sp_def,
                           COL_SPE,   p->speed,
                           COL_TOTAL, p->total,
                           COL_INDEX, i,
                           COL_ICON,  sprite_for(p->dex),
                           -1);
    }

    /*
     * Three models stacked on top of each other, which is the standard GTK
     * arrangement: the store holds every row, the filter hides the ones that
     * do not match, and the sort reorders what is left. The view only ever
     * talks to the top of the stack.
     */
    state->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(state->store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(state->filter),
                                           row_visible, state, NULL);

    GtkTreeModel *sorted = gtk_tree_model_sort_new_with_model(state->filter);
    state->tree = gtk_tree_view_new_with_model(sorted);

    /*
     * Do NOT turn on gtk_tree_view_set_fixed_height_mode() here. It is only
     * legal when every column has been set to GTK_TREE_VIEW_COLUMN_FIXED
     * sizing; with the default sizing the rows end up zero pixels tall and
     * the table renders as an empty black rectangle, with no warning at all.
     * At 1025 rows the default mode is plenty fast, so there is nothing to
     * gain by risking it.
     */

    /* 38 + 46 + 108 + 74 + 74 + 7*62 = 774, just inside TABLE_WIDTH. */
    add_icon_column(state->tree, COL_ICON);
    add_column(state->tree, "#",    COL_DEX,   46,  TRUE);
    add_column(state->tree, "Name", COL_NAME,  108, FALSE);
    add_column(state->tree, "Type", COL_TYPE1, 74,  FALSE);
    add_column(state->tree, "",     COL_TYPE2, 74,  FALSE);
    add_number_column(state->tree, "HP",    COL_HP);
    add_number_column(state->tree, "Atk",   COL_ATK);
    add_number_column(state->tree, "Def",   COL_DEF);
    add_number_column(state->tree, "SpA",   COL_SPA);
    add_number_column(state->tree, "SpD",   COL_SPD);
    add_number_column(state->tree, "Spe",   COL_SPE);
    add_number_column(state->tree, "Total", COL_TOTAL);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), state->tree);
    gtk_widget_set_size_request(scroll, TABLE_WIDTH, -1);

    /* --- detail panel --- */
    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(details), 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(details), "panel");

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    state->detail_sprite = gtk_image_new();
    gtk_widget_set_size_request(state->detail_sprite, SPRITE_LARGE, SPRITE_LARGE);
    gtk_box_pack_start(GTK_BOX(header), state->detail_sprite, FALSE, FALSE, 0);

    state->detail_name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->detail_name), 0.0);
    gtk_widget_set_valign(state->detail_name, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->detail_name),
                                "title");
    gtk_box_pack_start(GTK_BOX(header), state->detail_name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(details), header, FALSE, FALSE, 0);

    state->detail_types = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->detail_types), 0.0);
    gtk_box_pack_start(GTK_BOX(details), state->detail_types, FALSE, FALSE, 0);

    state->detail_total = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->detail_total), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->detail_total),
                                "heading");
    gtk_box_pack_start(GTK_BOX(details), state->detail_total, FALSE, FALSE, 0);

    GtkWidget *stat_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(stat_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(stat_grid), 8);
    for (int i = 0; i < 6; i++) {
        make_stat_row(state, stat_grid, i);
    }
    gtk_box_pack_start(GTK_BOX(details), stat_grid, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(details),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    state->weak_label   = make_matchup_row(details, "Weak to");
    state->resist_label = make_matchup_row(details, "Resists");
    state->immune_label = make_matchup_row(details, "Immune to");

    gtk_box_pack_start(GTK_BOX(details),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    state->move_count_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->move_count_label), 0.0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(state->move_count_label), "heading");
    gtk_box_pack_start(GTK_BOX(details), state->move_count_label, FALSE, FALSE, 0);

    state->move_store = gtk_list_store_new(MOVE_N_COLUMNS,
                                           G_TYPE_INT, G_TYPE_STRING);
    GtkWidget *move_tree = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(state->move_store));
    add_number_column(move_tree, "Lv",   MOVE_COL_LEVEL);
    add_text_column(move_tree,   "Move", MOVE_COL_NAME);

    GtkWidget *move_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(move_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(move_scroll), move_tree);
    gtk_widget_set_vexpand(move_scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(details), move_scroll, TRUE, TRUE, 0);

    /*
     * A GtkStack shows one child at a time. Here it swaps between the "pick
     * something" message and the real details, which is tidier than building
     * and destroying the panel on every selection.
     */
    GtkWidget *placeholder = gtk_label_new("Select a Pokemon to see its details.");
    gtk_style_context_add_class(gtk_widget_get_style_context(placeholder), "subtle");

    state->detail_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(state->detail_stack), placeholder, "empty");
    gtk_stack_add_named(GTK_STACK(state->detail_stack), details, "details");
    gtk_stack_set_visible_child_name(GTK_STACK(state->detail_stack), "empty");
    gtk_widget_set_size_request(state->detail_stack, PANEL_WIDTH, -1);

    /*
     * A plain box rather than a GtkPaned. A paned negotiates its divider from
     * the two children's natural sizes, and the table's natural width is the
     * sum of all its columns -- which is wider than the window, so the panel
     * got squeezed to nothing. pack_end gives the panel its width first and
     * lets the table have whatever is left, which is predictable at any
     * window size. The cost is losing the draggable divider.
     */
    GtkWidget *split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(split), scroll, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(split), state->detail_stack, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), split, TRUE, TRUE, 0);

    /* --- wiring --- */
    g_signal_connect(state->search, "search-changed",
                     G_CALLBACK(on_search_changed), state);
    g_signal_connect(state->type_combo, "changed",
                     G_CALLBACK(on_type_changed), state);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_clicked), state);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->tree)),
                     "changed", G_CALLBACK(on_selection_changed), state);

    update_count_label(state);
    return page;
}

/* The full 18x18 grid, built once. Colour makes the shape of it readable. */
static GtkWidget *build_chart_page(AppState *state)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);

    GtkWidget *corner = gtk_label_new("atk \\ def");
    gtk_style_context_add_class(gtk_widget_get_style_context(corner), "subtle");
    gtk_grid_attach(GTK_GRID(grid), corner, 0, 0, 1, 1);

    for (int t = 0; t < TYPE_COUNT; t++) {
        GString *markup = g_string_new(NULL);

        /* Column headings across the top. */
        g_string_printf(markup,
                        "<span foreground=\"%s\" size=\"small\">%.3s</span>",
                        TYPE_COLOURS[t], TYPE_NAMES[t]);
        GtkWidget *top = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(top), markup->str);
        gtk_widget_set_size_request(top, 34, -1);
        gtk_grid_attach(GTK_GRID(grid), top, t + 1, 0, 1, 1);

        /* Row headings down the side. */
        g_string_printf(markup, "<span foreground=\"%s\">%s</span>",
                        TYPE_COLOURS[t], TYPE_NAMES[t]);
        GtkWidget *side = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(side), markup->str);
        gtk_label_set_xalign(GTK_LABEL(side), 1.0);
        gtk_grid_attach(GTK_GRID(grid), side, 0, t + 1, 1, 1);

        g_string_free(markup, TRUE);
    }

    for (int atk = 0; atk < TYPE_COUNT; atk++) {
        for (int def = 0; def < TYPE_COUNT; def++) {
            double value = state->chart[atk][def];

            const char *background = "#2b313a";
            const char *text       = "#5d6875";
            const char *label      = "";
            if (value == 0.0) {
                background = "#3a2326"; text = "#e05a4f"; label = "0";
            } else if (value == 0.5) {
                background = "#33272a"; text = "#d98b83"; label = "\302\275";
            } else if (value == 2.0) {
                background = "#24372a"; text = "#7ddc8c"; label = "2";
            }

            GString *markup = g_string_new(NULL);
            g_string_printf(markup,
                            "<span background=\"%s\" foreground=\"%s\">"
                            "   %s   </span>", background, text, label);
            GtkWidget *cell = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(cell), markup->str);
            g_string_free(markup, TRUE);

            gtk_grid_attach(GTK_GRID(grid), cell, def + 1, atk + 1, 1, 1);
        }
    }

    /* The grid is much narrower than the window, so centre it. */
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(grid, GTK_ALIGN_START);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), grid);
    return scroll;
}

/*
 * Select the first visible row. Must run after the window is shown -- see the
 * note in activate().
 */
static void select_first_row(AppState *state)
{
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(state->tree));
    GtkTreeIter   first;
    if (gtk_tree_model_get_iter_first(model, &first)) {
        gtk_tree_selection_select_iter(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(state->tree)), &first);
    }
}

static void activate(GtkApplication *app, gpointer data)
{
    AppState *state = data;

    load_css();
    sprites_init();
    load_app_icon();

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "PokeSim");
    /*
     * Size to the monitor's work area rather than a hard-coded number, so the
     * window fits whatever display it opens on and never hides behind the
     * taskbar. Maximising on top of that lets the window manager have the
     * final say, which is what the system settings are for.
     */
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (monitor == NULL) {
        monitor = gdk_display_get_monitor(display, 0);
    }
    if (monitor != NULL) {
        GdkRectangle area;
        gdk_monitor_get_workarea(monitor, &area);
        gtk_window_set_default_size(GTK_WINDOW(window), area.width, area.height);
    } else {
        gtk_window_set_default_size(GTK_WINDOW(window), MIN_WIDTH, MIN_HEIGHT);
    }
    gtk_widget_set_size_request(window, MIN_WIDTH, MIN_HEIGHT);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_maximize(GTK_WINDOW(window));

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_dex_page(state),
                             gtk_label_new("Pokedex"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_duel_page(state),
                             gtk_label_new("Duel"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_rank_page(state),
                             gtk_label_new("Ranking"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_chart_page(state),
                             gtk_label_new("Type chart"));
    gtk_container_add(GTK_CONTAINER(window), notebook);

    gtk_widget_show_all(window);

    /*
     * The first selection has to come after show_all(). A GtkStack settles on
     * its visible child when its children are realised, so a selection made
     * while building gets overwritten by the placeholder page -- which is why
     * the panel used to say "select a Pokemon" with row 1 already highlighted.
     */
    select_first_row(state);
    duel_refresh(state);

    /*
     * A hook for checking the tabs without a mouse: POKESIM_AUTORUN=duel runs the
     * duel, POKESIM_AUTORUN=rank kicks off the tournament, and either opens the
     * matching tab. Harmless when the variable is unset.
     */
    const char *autorun = g_getenv("POKESIM_AUTORUN");
    if (autorun != NULL && strcmp(autorun, "duel") == 0) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
        on_duel_run(NULL, state);
    } else if (autorun != NULL && strcmp(autorun, "rank") == 0) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 2);
        on_rank_run(GTK_BUTTON(state->rank_button), state);
    }
}

/* ------------------------------------------------------------------ *
 * Duel tab
 * ------------------------------------------------------------------ */

/*
 * A GtkEntryCompletion turns a plain entry into a type-ahead box: it watches
 * what is typed and offers matching rows from a model. The model here is just
 * the 1025 names, and inline completion fills in the rest of the word as you
 * go.
 */
static void attach_name_completion(GtkWidget *entry, AppState *state)
{
    GtkListStore *names = gtk_list_store_new(2, G_TYPE_STRING, GDK_TYPE_PIXBUF);
    for (int i = 0; i < state->count; i++) {
        GtkTreeIter it;
        gtk_list_store_append(names, &it);
        gtk_list_store_set(names, &it,
                           0, state->roster[i].name,
                           1, sprite_for(state->roster[i].dex),
                           -1);
    }

    GtkEntryCompletion *completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(names));
    g_object_unref(names);

    /* Show the sprite beside each suggestion. */
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(completion), pix, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(completion), pix, "pixbuf", 1);

    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_completion_set_inline_completion(completion, TRUE);
    gtk_entry_completion_set_popup_completion(completion, TRUE);
    gtk_entry_completion_set_minimum_key_length(completion, 1);
    gtk_entry_set_completion(GTK_ENTRY(entry), completion);
    g_object_unref(completion);
}

static const Pokemon *duel_pick(AppState *state, int side)
{
    const char *text = gtk_entry_get_text(GTK_ENTRY(state->duel_entry[side]));
    if (text == NULL || *text == '\0') {
        return NULL;
    }
    for (int i = 0; i < state->count; i++) {
        if (g_ascii_strcasecmp(state->roster[i].name, text) == 0) {
            return &state->roster[i];
        }
    }
    return NULL;
}

/* Fill one side of the comparison, and colour the stats against the other. */
static void duel_show_side(AppState *state, int side, const Pokemon *p,
                           const Pokemon *other)
{
    if (p == NULL) {
        gtk_label_set_text(GTK_LABEL(state->duel_name[side]), "-");
        gtk_label_set_text(GTK_LABEL(state->duel_types[side]), "");
        gtk_label_set_text(GTK_LABEL(state->duel_moves[side]), "");
        gtk_label_set_text(GTK_LABEL(state->duel_total[side]), "");
        gtk_image_clear(GTK_IMAGE(state->duel_sprite[side]));
        for (int i = 0; i < 6; i++) {
            gtk_label_set_text(GTK_LABEL(state->duel_stat[side][i]), "-");
            gtk_progress_bar_set_fraction(
                GTK_PROGRESS_BAR(state->duel_stat_bar[side][i]), 0.0);
        }
        return;
    }

    gtk_label_set_text(GTK_LABEL(state->duel_name[side]), p->name);

    GdkPixbuf *small_sprite = sprite_for(p->dex);
    if (small_sprite != NULL) {
        GdkPixbuf *big = gdk_pixbuf_scale_simple(small_sprite, SPRITE_LARGE,
                                                 SPRITE_LARGE, GDK_INTERP_NEAREST);
        gtk_image_set_from_pixbuf(GTK_IMAGE(state->duel_sprite[side]), big);
        g_object_unref(big);
    }

    GString *types = g_string_new(NULL);
    append_type_badge(types, p->type1);
    append_type_badge(types, p->type2);
    gtk_label_set_markup(GTK_LABEL(state->duel_types[side]), types->str);
    g_string_free(types, TRUE);

    char total[64];
    snprintf(total, sizeof total, "BST %d", p->total);
    gtk_label_set_text(GTK_LABEL(state->duel_total[side]), total);

    int mine[6], theirs[6];
    get_stats(p, mine);
    if (other != NULL) {
        get_stats(other, theirs);
    }

    for (int i = 0; i < 6; i++) {
        /* Big enough for the markup, not just the number: a truncated
         * "<span ...>" is invalid markup and Pango refuses to render it. */
        char text[96];
        if (other == NULL) {
            snprintf(text, sizeof text, "%d", mine[i]);
        } else if (mine[i] > theirs[i]) {
            /* The higher stat of the pair is called out in green. */
            snprintf(text, sizeof text,
                     "<span foreground=\"#7ddc8c\"><b>%d</b></span>", mine[i]);
        } else if (mine[i] < theirs[i]) {
            snprintf(text, sizeof text,
                     "<span foreground=\"#d98b83\">%d</span>", mine[i]);
        } else {
            snprintf(text, sizeof text, "%d", mine[i]);
        }
        gtk_label_set_markup(GTK_LABEL(state->duel_stat[side][i]), text);
        set_stat_bar(state->duel_stat_bar[side][i], mine[i]);
    }

    /* The four moves it will actually fight with. */
    GString *moves = g_string_new(NULL);
    int slots[TEAM_MOVES], n = 0;
    choose_moveset(p, slots, &n);
    for (int i = 0; i < n; i++) {
        const MoveData *m = &move_table[slots[i]];
        char *escaped = g_markup_escape_text(m->name, -1);
        char detail[48];
        if (m->category == CAT_STATUS) {
            snprintf(detail, sizeof detail, "status");
        } else if (m->power > 0) {
            snprintf(detail, sizeof detail, "power %d", m->power);
        } else {
            /* Seismic Toss, Gyro Ball and the rest work their power out mid-fight. */
            snprintf(detail, sizeof detail, "varies");
        }
        g_string_append_printf(
            moves, "<span background=\"%s\" foreground=\"#12151a\" size=\"small\">"
                   " %s </span> <span size=\"small\" foreground=\"#9aa4b2\">%s</span>\n",
            TYPE_COLOURS[m->type], escaped, detail);
        g_free(escaped);
    }
    if (n == 0) {
        g_string_append(moves, "<span foreground=\"#9aa4b2\">no usable moves</span>");
    }
    gtk_label_set_markup(GTK_LABEL(state->duel_moves[side]), moves->str);
    g_string_free(moves, TRUE);
}

static void duel_refresh(AppState *state)
{
    const Pokemon *a = duel_pick(state, 0);
    const Pokemon *b = duel_pick(state, 1);
    duel_show_side(state, 0, a, b);
    duel_show_side(state, 1, b, a);
}

static void on_duel_entry_changed(GtkEditable *editable, gpointer data)
{
    (void)editable;
    duel_refresh(data);
}

static void on_duel_run(GtkButton *button, gpointer data)
{
    (void)button;
    AppState *state = data;

    const Pokemon *a = duel_pick(state, 0);
    const Pokemon *b = duel_pick(state, 1);
    if (a == NULL || b == NULL) {
        gtk_label_set_markup(GTK_LABEL(state->duel_result),
                             "<span foreground=\"#e8a33d\">"
                             "Pick two Pokemon first.</span>");
        gtk_label_set_text(GTK_LABEL(state->duel_detail), "");
        return;
    }

    int runs = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->duel_runs));

    SeriesStats s;
    simulate_series(a, b, state->chart, runs, &s);

    double pa = 100.0 * s.a_wins / (s.battles > 0 ? s.battles : 1);
    double pb = 100.0 * s.b_wins / (s.battles > 0 ? s.battles : 1);

    /*
     * A win rate from a finite sample has an error bar. This is the usual
     * normal approximation, 1.96 standard errors, which is roughly a 95%
     * interval -- enough to tell "clearly ahead" from "too close to call".
     */
    double p  = pa / 100.0;
    double se = 100.0 * sqrt(p * (1.0 - p) / (s.battles > 0 ? s.battles : 1));
    double margin = 1.96 * se;

    GString *out = g_string_new(NULL);
    g_string_append_printf(
        out, "<span size=\"large\"><b>%s wins %.1f%%</b>   |   "
             "<b>%s wins %.1f%%</b></span>\n"
             "<span foreground=\"#9aa4b2\">give or take %.1f%% "
             "(95%% confidence over %d battles)   |   %d draws</span>",
        a->name, pa, b->name, pb, margin, s.battles, s.draws);
    gtk_label_set_markup(GTK_LABEL(state->duel_result), out->str);
    g_string_free(out, TRUE);

    int battles = (s.battles > 0) ? s.battles : 1;

    /*
     * Per-battle averages rather than raw totals: "1,058,247 damage" over a
     * thousand battles means nothing to a reader, but "1058 per battle" can be
     * compared against an HP bar directly.
     */
    GString *detail = g_string_new(NULL);
    g_string_append_printf(detail,
        "%-26s %14s   %14s\n"
        "%-26s %14d   %14d\n"
        "%-26s %14.1f   %14.1f\n"
        "%-26s %14.2f   %14.2f\n"
        "%-26s %14.2f   %14.2f\n",
        "", a->name, b->name,
        "Battles won", s.a_wins, s.b_wins,
        "Damage dealt per battle",
        (double)s.a_damage / battles, (double)s.b_damage / battles,
        "Critical hits per battle",
        (double)s.a_crits / battles, (double)s.b_crits / battles,
        "Missed attacks per battle",
        (double)s.a_misses / battles, (double)s.b_misses / battles);

    g_string_append_printf(detail,
        "\n%d battles fought.  A fight lasts %.1f turns on average "
        "(shortest %d, longest %d).\n",
        s.battles,
        (double)s.total_turns / battles, s.min_turns, s.max_turns);

    g_string_append_printf(detail, "\nHow often each move was chosen\n");
    for (int i = 0; i < TEAM_MOVES; i++) {
        if (s.a_move_used[i] > 0) {
            g_string_append_printf(detail, "  %-12s %-20s %5.1f per battle\n",
                                   i == 0 ? a->name : "", moveset_name(a, i),
                                   (double)s.a_move_used[i] / battles);
        }
    }
    for (int i = 0; i < TEAM_MOVES; i++) {
        if (s.b_move_used[i] > 0) {
            g_string_append_printf(detail, "  %-12s %-20s %5.1f per battle\n",
                                   i == 0 ? b->name : "", moveset_name(b, i),
                                   (double)s.b_move_used[i] / battles);
        }
    }

    gtk_label_set_text(GTK_LABEL(state->duel_detail), detail->str);
    g_string_free(detail, TRUE);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->duel_bar), pa / 100.0);
}

static void on_duel_swap(GtkButton *button, gpointer data)
{
    (void)button;
    AppState *state = data;
    char *left = g_strdup(gtk_entry_get_text(GTK_ENTRY(state->duel_entry[0])));
    gtk_entry_set_text(GTK_ENTRY(state->duel_entry[0]),
                       gtk_entry_get_text(GTK_ENTRY(state->duel_entry[1])));
    gtk_entry_set_text(GTK_ENTRY(state->duel_entry[1]), left);
    g_free(left);
}

/* Build one column of the side-by-side comparison. */
static GtkWidget *build_duel_side(AppState *state, int side)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "panel");
    gtk_widget_set_size_request(box, 330, -1);

    state->duel_entry[side] = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->duel_entry[side]),
                                   side == 0 ? "Type a name..." : "and another...");
    attach_name_completion(state->duel_entry[side], state);
    gtk_box_pack_start(GTK_BOX(box), state->duel_entry[side], FALSE, FALSE, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    state->duel_sprite[side] = gtk_image_new();
    gtk_widget_set_size_request(state->duel_sprite[side], SPRITE_LARGE, SPRITE_LARGE);
    gtk_box_pack_start(GTK_BOX(header), state->duel_sprite[side], FALSE, FALSE, 0);

    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    state->duel_name[side] = gtk_label_new("-");
    gtk_label_set_xalign(GTK_LABEL(state->duel_name[side]), 0.0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(state->duel_name[side]), "title");
    gtk_box_pack_start(GTK_BOX(titles), state->duel_name[side], FALSE, FALSE, 0);

    state->duel_types[side] = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->duel_types[side]), 0.0);
    gtk_box_pack_start(GTK_BOX(titles), state->duel_types[side], FALSE, FALSE, 0);

    state->duel_total[side] = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->duel_total[side]), 0.0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(state->duel_total[side]), "subtle");
    gtk_box_pack_start(GTK_BOX(titles), state->duel_total[side], FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(header), titles, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    for (int i = 0; i < 6; i++) {
        GtkWidget *name = gtk_label_new(STAT_NAMES[i]);
        gtk_label_set_xalign(GTK_LABEL(name), 0.0);
        gtk_widget_set_size_request(name, 62, -1);
        gtk_grid_attach(GTK_GRID(grid), name, 0, i, 1, 1);

        state->duel_stat_bar[side][i] = gtk_progress_bar_new();
        gtk_widget_set_hexpand(state->duel_stat_bar[side][i], TRUE);
        gtk_widget_set_valign(state->duel_stat_bar[side][i], GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(grid), state->duel_stat_bar[side][i], 1, i, 1, 1);

        state->duel_stat[side][i] = gtk_label_new("-");
        gtk_label_set_xalign(GTK_LABEL(state->duel_stat[side][i]), 1.0);
        gtk_widget_set_size_request(state->duel_stat[side][i], 36, -1);
        gtk_grid_attach(GTK_GRID(grid), state->duel_stat[side][i], 2, i, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 4);

    GtkWidget *moves_title = gtk_label_new("Moves it will fight with");
    gtk_label_set_xalign(GTK_LABEL(moves_title), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(moves_title), "heading");
    gtk_box_pack_start(GTK_BOX(box), moves_title, FALSE, FALSE, 0);

    state->duel_moves[side] = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->duel_moves[side]), 0.0);
    gtk_box_pack_start(GTK_BOX(box), state->duel_moves[side], FALSE, FALSE, 0);

    return box;
}

static GtkWidget *build_duel_page(AppState *state)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    /* --- the two sides --- */
    GtkWidget *sides = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(sides), build_duel_side(state, 0), TRUE, TRUE, 0);

    GtkWidget *middle = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(middle, GTK_ALIGN_CENTER);
    GtkWidget *vs = gtk_label_new("vs");
    gtk_style_context_add_class(gtk_widget_get_style_context(vs), "title");
    gtk_box_pack_start(GTK_BOX(middle), vs, FALSE, FALSE, 0);
    GtkWidget *swap = gtk_button_new_with_label("swap");
    gtk_box_pack_start(GTK_BOX(middle), swap, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sides), middle, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(sides), build_duel_side(state, 1), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), sides, FALSE, FALSE, 0);

    /* --- controls --- */
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *runs_label = gtk_label_new("Battles");
    gtk_box_pack_start(GTK_BOX(controls), runs_label, FALSE, FALSE, 0);

    state->duel_runs = gtk_spin_button_new_with_range(1, 100000, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->duel_runs), 1000);
    gtk_box_pack_start(GTK_BOX(controls), state->duel_runs, FALSE, FALSE, 0);

    GtkWidget *run = gtk_button_new_with_label("Run the simulation");
    gtk_box_pack_start(GTK_BOX(controls), run, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), controls, FALSE, FALSE, 0);

    /* --- results --- */
    GtkWidget *results = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(results), 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(results), "panel");

    state->duel_result = gtk_label_new("Pick two Pokemon and run the simulation.");
    gtk_label_set_xalign(GTK_LABEL(state->duel_result), 0.0);
    gtk_box_pack_start(GTK_BOX(results), state->duel_result, FALSE, FALSE, 0);

    state->duel_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(state->duel_bar),
                                "s-high");
    gtk_box_pack_start(GTK_BOX(results), state->duel_bar, FALSE, FALSE, 0);

    state->duel_detail = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->duel_detail), 0.0);
    gtk_widget_set_valign(state->duel_detail, GTK_ALIGN_START);
    PangoAttrList *mono = pango_attr_list_new();
    pango_attr_list_insert(mono, pango_attr_family_new("monospace"));
    gtk_label_set_attributes(GTK_LABEL(state->duel_detail), mono);
    pango_attr_list_unref(mono);

    /*
     * The move breakdown grows with however many moves each side used, so it
     * can outgrow the panel on a short window. A scroller means the numbers
     * are always reachable instead of being silently clipped.
     */
    GtkWidget *detail_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detail_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(detail_scroll), state->duel_detail);
    gtk_box_pack_start(GTK_BOX(results), detail_scroll, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(page), results, TRUE, TRUE, 0);

    g_signal_connect(state->duel_entry[0], "changed",
                     G_CALLBACK(on_duel_entry_changed), state);
    g_signal_connect(state->duel_entry[1], "changed",
                     G_CALLBACK(on_duel_entry_changed), state);
    g_signal_connect(run, "clicked", G_CALLBACK(on_duel_run), state);
    g_signal_connect(swap, "clicked", G_CALLBACK(on_duel_swap), state);

    /* Something in the boxes to start with. */
    gtk_entry_set_text(GTK_ENTRY(state->duel_entry[0]), "Charizard");
    gtk_entry_set_text(GTK_ENTRY(state->duel_entry[1]), "Blastoise");

    return page;
}

/* ------------------------------------------------------------------ *
 * Ranking tab
 * ------------------------------------------------------------------ */

/*
 * The round robin takes seconds, not milliseconds, so it runs on its own
 * thread. GTK is not thread-safe, so the worker touches nothing but the
 * numbers in AppState; a timeout on the main loop reads those and updates the
 * widgets. That is the standard division of labour for background work in GTK.
 */
static void rank_progress_cb(double fraction, void *user_data)
{
    AppState *state = user_data;
    state->progress = fraction;
}

static gpointer rank_worker(gpointer data)
{
    AppState *state = data;
    GTimer   *timer = g_timer_new();

    run_tournament(state->roster, state->count, state->chart,
                   state->worker_runs, state->results,
                   &state->cancel, rank_progress_cb, state);

    state->worker_seconds = g_timer_elapsed(timer, NULL);
    g_timer_destroy(timer);
    state->worker_done = 1;
    return NULL;
}

static void rank_fill_table(AppState *state)
{
    gtk_list_store_clear(state->rank_store);

    /* Sort a copy by wins so the rank column means something. */
    int *order = g_malloc_n(state->count, sizeof *order);
    for (int i = 0; i < state->count; i++) {
        order[i] = i;
    }
    for (int i = 1; i < state->count; i++) {      /* insertion sort, once */
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && state->results[order[j]].wins < state->results[key].wins) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    for (int r = 0; r < state->count; r++) {
        int              i = order[r];
        const RankEntry *e = &state->results[i];
        const Pokemon   *p = &state->roster[i];
        int battles = (e->battles > 0) ? e->battles : 1;

        GtkTreeIter it;
        gtk_list_store_append(state->rank_store, &it);
        gtk_list_store_set(state->rank_store, &it,
            RK_RANK,     r + 1,
            RK_ICON,     sprite_for(p->dex),
            RK_NAME,     p->name,
            RK_TYPE1,    type_name(p->type1),
            RK_TYPE2,    (p->type2 == TYPE_NONE) ? "" : type_name(p->type2),
            RK_WINS,     e->wins,
            RK_LOSSES,   e->losses,
            RK_DRAWS,    e->draws,
            RK_WINPCT,   100.0 * e->wins / battles,
            RK_TOTAL,    p->total,
            RK_AVGTURNS, (double)e->turns / battles,
            RK_DMGRATIO, (double)e->damage_dealt /
                         (e->damage_taken > 0 ? (double)e->damage_taken : 1.0),
            RK_INDEX,    i,
            -1);
    }
    g_free(order);
}

/* Called on the main loop while the worker runs. */
static gboolean rank_poll(gpointer data)
{
    AppState *state = data;

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->rank_progress),
                                  state->progress);
    char text[128];
    long long pairs = (long long)state->count * (state->count - 1) / 2;
    snprintf(text, sizeof text, "%.0f%% -- %lld of %lld pairings",
             state->progress * 100.0,
             (long long)(state->progress * pairs), pairs);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->rank_progress), text);

    if (!state->worker_done) {
        return G_SOURCE_CONTINUE;
    }

    g_thread_join(state->worker);
    state->worker = NULL;

    rank_fill_table(state);

    snprintf(text, sizeof text,
             "%lld battles in %.1f s   (%.0f/second)",
             pairs * state->worker_runs, state->worker_seconds,
             (double)(pairs * state->worker_runs) /
             (state->worker_seconds > 0 ? state->worker_seconds : 1));
    gtk_label_set_text(GTK_LABEL(state->rank_status), text);
    gtk_button_set_label(GTK_BUTTON(state->rank_button), "Run the tournament");
    gtk_widget_set_sensitive(state->rank_button, TRUE);
    gtk_widget_set_sensitive(state->rank_runs, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->rank_progress), 1.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->rank_progress), "done");

    return G_SOURCE_REMOVE;
}

static void on_rank_run(GtkButton *button, gpointer data)
{
    AppState *state = data;

    if (state->worker != NULL) {            /* already running: cancel it */
        state->cancel = 1;
        gtk_button_set_label(button, "Stopping...");
        gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
        return;
    }

    if (state->results == NULL) {
        state->results = g_malloc0_n(state->count, sizeof(RankEntry));
    }

    state->worker_runs = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(state->rank_runs));
    state->cancel      = 0;
    state->worker_done = 0;
    state->progress    = 0.0;

    gtk_button_set_label(button, "Stop");
    gtk_widget_set_sensitive(state->rank_runs, FALSE);
    gtk_label_set_text(GTK_LABEL(state->rank_status), "running...");

    state->worker = g_thread_new("tournament", rank_worker, state);
    g_timeout_add(100, rank_poll, state);
}

static gboolean rank_row_visible(GtkTreeModel *model, GtkTreeIter *iter,
                                 gpointer data)
{
    AppState *state = data;
    int index = -1;
    gtk_tree_model_get(model, iter, RK_INDEX, &index, -1);
    if (index < 0 || index >= state->count) {
        return FALSE;
    }
    const Pokemon *p = &state->roster[index];

    if (state->rank_type_filter != TYPE_NONE &&
        p->type1 != state->rank_type_filter &&
        p->type2 != state->rank_type_filter) {
        return FALSE;
    }
    if (state->rank_search_text[0] != '\0') {
        char lowered[NAME_LEN];
        snprintf(lowered, sizeof lowered, "%s", p->name);
        for (char *c = lowered; *c; c++) {
            *c = (char)g_ascii_tolower(*c);
        }
        if (strstr(lowered, state->rank_search_text) == NULL) {
            return FALSE;
        }
    }
    return TRUE;
}

static void on_rank_search(GtkSearchEntry *entry, gpointer data)
{
    AppState *state = data;
    snprintf(state->rank_search_text, sizeof state->rank_search_text,
             "%s", gtk_entry_get_text(GTK_ENTRY(entry)));
    for (char *c = state->rank_search_text; *c; c++) {
        *c = (char)g_ascii_tolower(*c);
    }
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(state->rank_filter));
}

static void on_rank_type(GtkComboBox *combo, gpointer data)
{
    AppState *state = data;
    int active = gtk_combo_box_get_active(combo);
    state->rank_type_filter = (active <= 0) ? TYPE_NONE : active - 1;
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(state->rank_filter));
}

static GtkWidget *build_rank_page(AppState *state)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    state->rank_type_filter = TYPE_NONE;

    /* --- run bar --- */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    state->rank_button = gtk_button_new_with_label("Run the tournament");
    gtk_box_pack_start(GTK_BOX(bar), state->rank_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("battles per pairing"),
                       FALSE, FALSE, 0);
    state->rank_runs = gtk_spin_button_new_with_range(1, 501, 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->rank_runs), 11);
    gtk_box_pack_start(GTK_BOX(bar), state->rank_runs, FALSE, FALSE, 0);

    state->rank_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->rank_progress), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->rank_progress),
                              "not run yet");
    gtk_widget_set_valign(state->rank_progress, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(state->rank_progress, TRUE);
    gtk_box_pack_start(GTK_BOX(bar), state->rank_progress, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(page), bar, FALSE, FALSE, 0);

    state->rank_status = gtk_label_new(
        "1025 species, 524,800 pairings. 11 battles each is about 6 million "
        "battles and takes a few seconds.");
    gtk_label_set_xalign(GTK_LABEL(state->rank_status), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->rank_status),
                                "subtle");
    gtk_box_pack_start(GTK_BOX(page), state->rank_status, FALSE, FALSE, 0);

    /* --- filters --- */
    GtkWidget *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    state->rank_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->rank_search),
                                   "Filter by name...");
    gtk_widget_set_hexpand(state->rank_search, TRUE);
    gtk_box_pack_start(GTK_BOX(filters), state->rank_search, TRUE, TRUE, 0);

    state->rank_type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->rank_type_combo),
                                   "All types");
    for (int i = 0; i < TYPE_COUNT; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->rank_type_combo),
                                       TYPE_NAMES[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->rank_type_combo), 0);
    gtk_box_pack_start(GTK_BOX(filters), state->rank_type_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), filters, FALSE, FALSE, 0);

    /* --- the table --- */
    state->rank_store = gtk_list_store_new(RK_N,
        G_TYPE_INT, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_DOUBLE, G_TYPE_INT,
        G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_INT);

    state->rank_filter = gtk_tree_model_filter_new(
        GTK_TREE_MODEL(state->rank_store), NULL);
    gtk_tree_model_filter_set_visible_func(
        GTK_TREE_MODEL_FILTER(state->rank_filter), rank_row_visible, state, NULL);

    GtkTreeModel *sorted = gtk_tree_model_sort_new_with_model(state->rank_filter);
    state->rank_tree = gtk_tree_view_new_with_model(sorted);

    add_column(state->rank_tree, "#",      RK_RANK,   46,  TRUE);
    add_icon_column(state->rank_tree, RK_ICON);
    add_column(state->rank_tree, "Name",   RK_NAME,   116, FALSE);
    add_column(state->rank_tree, "Type",   RK_TYPE1,  72,  FALSE);
    add_column(state->rank_tree, "",       RK_TYPE2,  72,  FALSE);
    add_column(state->rank_tree, "Battles won",  RK_WINS,   92,  TRUE);
    add_column(state->rank_tree, "Battles lost", RK_LOSSES, 92,  TRUE);
    add_column(state->rank_tree, "Draws",        RK_DRAWS,  62,  TRUE);
    add_percent_column(state->rank_tree, "Win rate", RK_WINPCT);
    add_column(state->rank_tree, "Base stats",   RK_TOTAL,  84,  TRUE);
    add_decimal_column(state->rank_tree, "Avg turns",  RK_AVGTURNS);
    add_decimal_column(state->rank_tree, "Dmg ratio",  RK_DMGRATIO);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), state->rank_tree);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    /* A column heading can only say so much; this says the rest. */
    GtkWidget *legend = gtk_label_new(
        "Every species fights every other species the chosen number of times.  "
        "Win rate is battles won out of all battles fought, draws included.  "
        "Avg turns is how long its fights last.  "
        "Dmg ratio is damage dealt divided by damage taken \u2014 above 1.00 means it "
        "hits harder than it is hit.  Click any heading to sort by it.");
    gtk_label_set_xalign(GTK_LABEL(legend), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(legend), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(legend), "subtle");
    gtk_box_pack_start(GTK_BOX(page), legend, FALSE, FALSE, 0);

    g_signal_connect(state->rank_button, "clicked", G_CALLBACK(on_rank_run), state);
    g_signal_connect(state->rank_search, "search-changed",
                     G_CALLBACK(on_rank_search), state);
    g_signal_connect(state->rank_type_combo, "changed",
                     G_CALLBACK(on_rank_type), state);

    return page;
}

/* ------------------------------------------------------------------ *
 * Console report (unchanged from Stage 1)
 * ------------------------------------------------------------------ */

static void print_pokemon(const Pokemon *p)
{
    printf("  #%-4d %-12s %-8s %-8s  %3d/%3d/%3d/%3d/%3d/%3d = %3d  %2d moves\n",
           p->dex, p->name, type_name(p->type1),
           (p->type2 == TYPE_NONE) ? "-" : type_name(p->type2),
           p->hp, p->attack, p->defense, p->sp_atk, p->sp_def, p->speed,
           p->total, p->move_count);
}

static void report(const Pokemon *roster, int count,
                   double chart[TYPE_COUNT][TYPE_COUNT])
{
    printf("\n=== Roster ===\n");
    printf("  %d species loaded, dex 1 to %d\n", count, roster[count - 1].dex);

    int dual = 0, total_moves = 0, fewest = MAX_MOVES, most = 0, best = 0;
    for (int i = 0; i < count; i++) {
        if (roster[i].type2 != TYPE_NONE) {
            dual++;
        }
        total_moves += roster[i].move_count;
        if (roster[i].move_count < fewest) {
            fewest = roster[i].move_count;
        }
        if (roster[i].move_count > most) {
            most = roster[i].move_count;
        }
        if (roster[i].total > roster[best].total) {
            best = i;
        }
    }
    printf("  %d dual-typed, %d single-typed\n", dual, count - dual);
    printf("  %d move entries, %.1f per species (fewest %d, most %d)\n",
           total_moves, (double)total_moves / count, fewest, most);
    printf("  every row's stated Total matches the sum of its six stats\n");

    printf("\n=== Highest base stat total ===\n");
    print_pokemon(&roster[best]);

    printf("\n=== Species knowing only one move ===\n");
    for (int i = 0; i < count; i++) {
        if (roster[i].move_count == 1) {
            printf("  #%-4d %-12s knows only %s\n",
                   roster[i].dex, roster[i].name, roster[i].moves[0].name);
        }
    }

    printf("\n=== Type chart spot checks ===\n");
    printf("  Rock   -> Charizard (Fire/Flying)  = %gx\n",
           effectiveness(chart, type_index("Rock"), &roster[5]));
    printf("  Normal -> Gengar    (Ghost/Poison) = %gx\n",
           effectiveness(chart, type_index("Normal"), &roster[93]));
}

/* ------------------------------------------------------------------ *
 * Duel report (console)
 * ------------------------------------------------------------------ */

/* Case-insensitive species lookup, so --duel does not demand exact case. */
static const Pokemon *find_species(const Pokemon *roster, int count,
                                   const char *name)
{
    for (int i = 0; i < count; i++) {
        const char *a = roster[i].name;
        const char *b = name;
        while (*a && *b &&
               g_ascii_tolower((unsigned char)*a) == g_ascii_tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return &roster[i];
        }
    }
    return NULL;
}

static void print_moveset(const Pokemon *p)
{
    int moves[TEAM_MOVES], n = 0;
    choose_moveset(p, moves, &n);
    printf("    moves:");
    for (int i = 0; i < n; i++) {
        const MoveData *m = &move_table[moves[i]];
        printf(" %s(%s %d)", m->name,
               m->category == CAT_STATUS ? "sta"
               : m->category == CAT_PHYSICAL ? "phy" : "spe",
               m->power);
    }
    if (n == 0) {
        printf(" none");
    }
    printf("\n");
}

static void report_duel(const Pokemon *a, const Pokemon *b,
                        double chart[TYPE_COUNT][TYPE_COUNT], int runs)
{
    SeriesStats s;
    simulate_series(a, b, chart, runs, &s);

    printf("\n%s  vs  %s        %d battles\n", a->name, b->name, runs);
    printf("--------------------------------------------------\n");
    printf("  %-12s %s/%s  BST %d\n", a->name, type_name(a->type1),
           a->type2 == TYPE_NONE ? "-" : type_name(a->type2), a->total);
    print_moveset(a);
    printf("  %-12s %s/%s  BST %d\n", b->name, type_name(b->type1),
           b->type2 == TYPE_NONE ? "-" : type_name(b->type2), b->total);
    print_moveset(b);

    double pct_a = 100.0 * s.a_wins / (s.battles > 0 ? s.battles : 1);
    double pct_b = 100.0 * s.b_wins / (s.battles > 0 ? s.battles : 1);
    printf("\n  %-12s %5d wins  %5.1f%%\n", a->name, s.a_wins, pct_a);
    printf("  %-12s %5d wins  %5.1f%%\n", b->name, s.b_wins, pct_b);
    printf("  draws        %5d\n", s.draws);
    printf("  turns        avg %.1f   min %d   max %d\n",
           (double)s.total_turns / (s.battles > 0 ? s.battles : 1),
           s.min_turns, s.max_turns);
    printf("  damage dealt %s %lld, %s %lld\n",
           a->name, s.a_damage, b->name, s.b_damage);
    printf("  crits        %s %d, %s %d\n", a->name, s.a_crits, b->name, s.b_crits);
    printf("  misses       %s %d, %s %d\n", a->name, s.a_misses, b->name, s.b_misses);

    printf("  move usage   %s:", a->name);
    for (int i = 0; i < TEAM_MOVES; i++) {
        if (s.a_move_used[i] > 0) {
            printf(" %s=%d", moveset_name(a, i), s.a_move_used[i]);
        }
    }
    printf("\n               %s:", b->name);
    for (int i = 0; i < TEAM_MOVES; i++) {
        if (s.b_move_used[i] > 0) {
            printf(" %s=%d", moveset_name(b, i), s.b_move_used[i]);
        }
    }
    printf("\n");
}

/* ------------------------------------------------------------------ *
 * Tournament report (console)
 * ------------------------------------------------------------------ */

static void tournament_progress(double fraction, void *user_data)
{
    (void)user_data;
    fprintf(stderr, "\r  %5.1f%% ", fraction * 100.0);
    fflush(stderr);
}

static int compare_rank(const void *x, const void *y)
{
    const RankEntry *a = x;
    const RankEntry *b = y;
    if (a->wins != b->wins) {
        return (b->wins > a->wins) ? 1 : -1;
    }
    return (int)(b->damage_dealt - a->damage_dealt);
}

static void report_tournament(const Pokemon *roster, int count,
                              double chart[TYPE_COUNT][TYPE_COUNT], int runs)
{
    static RankEntry table[MAX_POKEMON];

    long long pairs = (long long)count * (count - 1) / 2;
    printf("Round robin: %d species, %lld pairings, %d battles each = %lld battles\n",
           count, pairs, runs, pairs * runs);

    GTimer *timer = g_timer_new();
    run_tournament(roster, count, chart, runs, table, NULL,
                   tournament_progress, NULL);
    double seconds = g_timer_elapsed(timer, NULL);
    g_timer_destroy(timer);

    fprintf(stderr, "\r");
    printf("Finished in %.1f s (%.0f battles/second)\n\n",
           seconds, (double)(pairs * runs) / (seconds > 0 ? seconds : 1));

    /* Rank a copy so the dex order of `table` is left alone. */
    static RankEntry sorted[MAX_POKEMON];
    memcpy(sorted, table, sizeof(RankEntry) * count);
    qsort(sorted, count, sizeof(RankEntry), compare_rank);

    printf("%-5s %-14s %7s %7s %7s  %s\n",
           "rank", "name", "wins", "losses", "win%", "avg turns");
    for (int i = 0; i < 25 && i < count; i++) {
        const RankEntry *e = &sorted[i];
        const Pokemon   *p = &roster[e->dex - 1];
        printf("%-5d %-14s %7d %7d %6.1f%%  %.1f\n",
               i + 1, p->name, e->wins, e->losses,
               100.0 * e->wins / (e->battles > 0 ? e->battles : 1),
               (double)e->turns / (e->battles > 0 ? e->battles : 1));
    }

    printf("\n  ... bottom 5 ...\n");
    for (int i = count - 5; i < count; i++) {
        const RankEntry *e = &sorted[i];
        const Pokemon   *p = &roster[e->dex - 1];
        printf("%-5d %-14s %7d %7d %6.1f%%  %.1f\n",
               i + 1, p->name, e->wins, e->losses,
               100.0 * e->wins / (e->battles > 0 ? e->battles : 1),
               (double)e->turns / (e->battles > 0 ? e->battles : 1));
    }
}

/* ------------------------------------------------------------------ *
 * Self-tests
 * ------------------------------------------------------------------ */

static int tests_run    = 0;
static int tests_failed = 0;

static void check(int condition, const char *what)
{
    tests_run++;
    if (condition) {
        printf("  [pass] %s\n", what);
    } else {
        printf("  [FAIL] %s\n", what);
        tests_failed++;
    }
}

static int run_tests(const Pokemon *roster, int count,
                     double chart[TYPE_COUNT][TYPE_COUNT])
{
    printf("=== Self-tests ===\n");

    /* --- the CSV splitter, the part most likely to be subtly wrong --- */
    char  sample[] = "a,,c";
    char *fields[3];
    int   got = split_csv(sample, fields, 3);
    check(got == 3, "split_csv finds 3 fields in \"a,,c\"");
    check(got == 3 && strcmp(fields[0], "a") == 0, "  field 0 is \"a\"");
    check(got == 3 && fields[1][0] == '\0', "  field 1 is empty, not skipped");
    check(got == 3 && strcmp(fields[2], "c") == 0, "  field 2 is \"c\"");

    char  overflow[] = "a,b,c,d";
    char *small[2];
    check(split_csv(overflow, small, 2) == -1, "split_csv rejects too many fields");

    /* --- roster --- */
    check(count == MAX_POKEMON, "roster holds exactly 1025 species");
    check(strcmp(roster[0].name, "Bulbasaur") == 0, "dex 1 is Bulbasaur");
    check(roster[0].type1 == type_index("Grass") &&
          roster[0].type2 == type_index("Poison"),
          "  Bulbasaur is Grass/Poison");
    check(roster[0].hp == 45 && roster[0].attack == 49 && roster[0].speed == 45,
          "  Bulbasaur's stats are right");
    check(strcmp(roster[1024].name, "Pecharunt") == 0, "dex 1025 is Pecharunt");

    int contiguous = 1, totals_ok = 1, names_ok = 1, types_ok = 1, levels_ok = 1;
    int single_typed = 0;
    for (int i = 0; i < count; i++) {
        if (roster[i].dex != i + 1) {
            contiguous = 0;
        }
        if (roster[i].total != stat_sum(&roster[i])) {
            totals_ok = 0;
        }
        if (roster[i].name[0] == '\0' || strlen(roster[i].name) >= NAME_LEN - 1) {
            names_ok = 0;
        }
        if (roster[i].type1 < 0 || roster[i].type1 >= TYPE_COUNT) {
            types_ok = 0;
        }
        if (roster[i].type2 == TYPE_NONE) {
            single_typed++;
        }
        for (int m = 0; m < roster[i].move_count; m++) {
            if (roster[i].moves[m].level < 0 || roster[i].moves[m].level > 100 ||
                roster[i].moves[m].name[0] == '\0') {
                levels_ok = 0;
            }
        }
    }
    check(contiguous, "dex numbers run 1..1025 with no gaps");
    check(totals_ok,  "every stated Total equals the sum of six stats");
    check(names_ok,   "no name is empty or hit the buffer limit");
    check(types_ok,   "every first type is a known type");
    check(levels_ok,  "every move has a name and a level in 0..100");
    check(single_typed == 499, "499 species are single-typed");
    check(count - single_typed == 526, "526 species are dual-typed");

    int duplicates = 0;
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(roster[i].name, roster[j].name) == 0) {
                duplicates++;
            }
        }
    }
    check(duplicates == 0, "no two species share a name");

    /*
     * UTF-8 survived the round trip byte for byte.
     *
     * The literals are split deliberately. A hex escape in C has no length
     * limit -- it swallows every hex digit that follows -- so "\x99d" is one
     * escape for 0x99d, which overflows a char, rather than 0x99 then 'd'.
     * Ending the literal ends the escape, and adjacent literals are then
     * joined back together by the compiler.
     */
    check(strcmp(roster[82].name, "Farfetch\xe2\x80\x99" "d") == 0,
          "Farfetch'd keeps its U+2019 apostrophe");
    check(strlen(roster[82].name) == 12, "  and is 12 bytes long for 10 glyphs");
    check(strcmp(roster[668].name, "Flab\xc3\xa9" "b\xc3\xa9") == 0,
          "Flabebe keeps its accents");

    check(roster[10].move_count == 1 &&
          strcmp(roster[10].moves[0].name, "Harden") == 0,
          "Metapod knows exactly one move, and it is Harden");
    check(roster[131].move_count == 1 &&
          strcmp(roster[131].moves[0].name, "Transform") == 0,
          "Ditto knows exactly one move, and it is Transform");

    int lv0 = 0;
    for (int i = 0; i < count; i++) {
        for (int m = 0; m < roster[i].move_count; m++) {
            if (roster[i].moves[m].level == 0) {
                lv0++;
            }
        }
    }
    check(lv0 == 271, "271 moves are learned on evolution (Lv 0)");

    check(roster[0].move_count == 15, "Bulbasaur has 15 level-up moves");
    check(strcmp(roster[0].moves[2].name, "Vine Whip") == 0,
          "  a two-word move name survives intact");
    check(roster[0].moves[2].level == 3, "  and keeps its level");

    /* --- type chart --- */
    int values_ok = 1;
    for (int r = 0; r < TYPE_COUNT; r++) {
        for (int c = 0; c < TYPE_COUNT; c++) {
            double v = chart[r][c];
            if (v != 0.0 && v != 0.5 && v != 1.0 && v != 2.0) {
                values_ok = 0;
            }
        }
    }
    check(values_ok, "every chart multiplier is 0, 0.5, 1 or 2");
    check(chart[type_index("Fire")][type_index("Grass")] == 2.0, "Fire beats Grass");
    check(chart[type_index("Water")][type_index("Fire")] == 2.0, "Water beats Fire");
    check(chart[type_index("Normal")][type_index("Ghost")] == 0.0,
          "Normal cannot hit Ghost");
    check(chart[type_index("Electric")][type_index("Ground")] == 0.0,
          "Electric cannot hit Ground");
    check(chart[type_index("Dragon")][type_index("Fairy")] == 0.0,
          "Dragon cannot hit Fairy");
    check(chart[type_index("Fighting")][type_index("Rock")] == 2.0,
          "Fighting beats Rock");

    check(effectiveness(chart, type_index("Rock"), &roster[5]) == 4.0,
          "Rock hits Charizard (Fire/Flying) for 4x");
    check(effectiveness(chart, type_index("Normal"), &roster[93]) == 0.0,
          "Normal hits Gengar (Ghost/Poison) for 0x");
    check(effectiveness(chart, type_index("Electric"), &roster[129]) == 4.0,
          "Electric hits Gyarados (Water/Flying) for 4x");

    /* --- colour table must line up with the type table --- */
    int colours_ok = 1;
    for (int t = 0; t < TYPE_COUNT; t++) {
        if (TYPE_COLOURS[t] == NULL || TYPE_COLOURS[t][0] != '#' ||
            strlen(TYPE_COLOURS[t]) != 7) {
            colours_ok = 0;
        }
    }
    check(colours_ok, "all 18 type colours are present and well formed");

    /* --- move table and the battle engine --- */
    check(move_table_count == 708, "move table holds all 708 moves");
    check(find_move("Flamethrower") >= 0, "Flamethrower is in the move table");
    {
        int fid = find_move("Flamethrower");
        check(fid >= 0 && move_table[fid].power == 90 &&
              move_table[fid].accuracy == 100 &&
              move_table[fid].category == CAT_SPECIAL &&
              move_table[fid].type == type_index("Fire"),
              "  and is Fire, special, 90 power, 100 accuracy");
        int hid = find_move("Harden");
        check(hid >= 0 && move_table[hid].category == CAT_STATUS,
              "Harden is a status move");
        int tid = find_move("Tackle");
        check(tid >= 0 && move_table[tid].power == 40 &&
              move_table[tid].category == CAT_PHYSICAL,
              "Tackle is physical with 40 power");
    }

    int unresolved_moves = 0;
    for (int i = 0; i < count; i++) {
        for (int m = 0; m < roster[i].move_count; m++) {
            if (roster[i].moves[m].id < 0) {
                unresolved_moves++;
            }
        }
    }
    check(unresolved_moves == 0, "every roster move resolves to the move table");

    int weights_ok = 1;
    for (int i = 0; i < count; i++) {
        if (roster[i].weight_hg <= 0) {
            weights_ok = 0;
        }
    }
    check(weights_ok, "every species has a weight");
    check(roster[142].weight_hg == 4600, "Snorlax weighs 460.0 kg");

    /* Movesets */
    {
        int moves[TEAM_MOVES], n = 0;
        choose_moveset(&roster[5], moves, &n);            /* Charizard */
        check(n == TEAM_MOVES, "Charizard fights with four moves");
        choose_moveset(&roster[10], moves, &n);           /* Metapod */
        check(n == 1 && strcmp(move_table[moves[0]].name, "Harden") == 0,
              "Metapod fights with its one move, Harden");
    }

    /* Determinism: the same seed must reproduce the same series exactly. */
    {
        SeriesStats s1, s2;
        battle_seed(12345);
        simulate_series(&roster[5], &roster[8], chart, 200, &s1);   /* Charizard v Blastoise */
        battle_seed(12345);
        simulate_series(&roster[5], &roster[8], chart, 200, &s2);
        check(s1.a_wins == s2.a_wins && s1.b_wins == s2.b_wins &&
              s1.total_turns == s2.total_turns,
              "the same seed reproduces the same series");
        check(s1.battles == 200, "a 200-battle series runs 200 battles");
        check(s1.a_wins + s1.b_wins + s1.draws == 200,
              "  and every battle has an outcome");
    }

    /* Type advantage has to actually show up in the results. */
    {
        SeriesStats water, fire;
        battle_seed(99);
        simulate_series(&roster[8], &roster[5], chart, 400, &water); /* Blastoise v Charizard */
        check(water.a_wins > water.b_wins,
              "Blastoise beats Charizard more often than not (Water v Fire/Flying)");

        /*
         * Win rate is the wrong thing to assert for a type advantage: a
         * Toxic-and-recover staller can lose every damage race and still win
         * the fight, which is exactly what Vileplume does to Charizard. What
         * the type chart actually promises is that the super-effective side
         * hits harder, so that is what gets checked.
         */
        battle_seed(99);
        simulate_series(&roster[5], &roster[44], chart, 400, &fire); /* Charizard v Vileplume */
        check(fire.a_damage > fire.b_damage * 3,
              "Charizard out-damages Vileplume heavily (Fire into Grass/Poison)");
    }

    /* Two Pokemon that cannot hurt each other must terminate, not hang. */
    {
        SeriesStats stall;
        battle_seed(7);
        simulate_series(&roster[10], &roster[13], chart, 20, &stall); /* Metapod v Kakuna */
        check(stall.battles == 20, "Metapod v Kakuna terminates instead of hanging");
        /*
         * Two Pokemon whose only move is Harden used to sit there until the
         * turn cap and be called a draw. Now they burn through Harden's 30 PP
         * and Struggle each other down, so the fight resolves on its own --
         * which is why this asserts the cap is NOT what ends it.
         */
        check(stall.max_turns < TURN_CAP,
              "  and it resolves through PP and Struggle, not the turn cap");
        check(stall.draws == 0, "  with a real winner rather than a timeout");
    }

    /* Arceus should be a long way above a Magikarp. */
    {
        SeriesStats s;
        battle_seed(4242);
        simulate_series(&roster[492], &roster[128], chart, 200, &s); /* Arceus v Magikarp */
        check(s.a_wins >= 195, "Arceus beats Magikarp essentially every time");
    }

    printf("\n  %d checks, %d passed, %d failed\n",
           tests_run, tests_run - tests_failed, tests_failed);
    return tests_failed == 0;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
#ifdef _WIN32
    /*
     * Without this the console uses the legacy code page and the UTF-8 names
     * (Nidoran, Flabebe, Farfetch'd) print as mojibake. The bytes in memory
     * are correct either way; this only fixes what you see. GTK does its own
     * text handling, so this matters for --report and --test only.
     */
    SetConsoleOutputCP(CP_UTF8);
#endif

    const char *roster_path = "pokemon_1025_stats_types_moves.csv";
    const char *chart_path  = "type_chart_18x18.csv";
    const char *moves_path  = "moves.csv";
    const char *weight_path = "pokemon_weights.csv";
    int         testing     = 0;
    int         reporting   = 0;
    int         duelling    = 0;
    int         tourney     = 0;
    int         tourney_runs = 11;
    int         duel_runs   = 1000;
    const char *duel_a      = NULL;
    const char *duel_b      = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            testing = 1;
        } else if (strcmp(argv[i], "--report") == 0) {
            reporting = 1;
        } else if (strcmp(argv[i], "--duel") == 0) {
            duelling = 1;
        } else if (strcmp(argv[i], "--tournament") == 0) {
            tourney = 1;
        } else if (duelling && duel_a == NULL) {
            duel_a = argv[i];
        } else if (duelling && duel_b == NULL) {
            duel_b = argv[i];
        } else if (duelling) {
            duel_runs = atoi(argv[i]);
            if (duel_runs < 1) {
                duel_runs = 1;
            }
        } else if (tourney) {
            tourney_runs = atoi(argv[i]);
            if (tourney_runs < 1) {
                tourney_runs = 1;
            }
        }
    }

    /*
     * 1025 Pokemon is about 1.5 MB. That is far too big for the stack, which
     * defaults to 1 MB on Windows, so these are `static`: one fixed allocation
     * that lives for the whole run, rather than locals that would blow the
     * stack before main() had done anything.
     */
    static Pokemon roster[MAX_POKEMON];
    static double  chart[TYPE_COUNT][TYPE_COUNT];
    int            count = 0;

    if (!load_roster(roster_path, roster, &count)) {
        return 1;
    }
    if (!load_type_chart(chart_path, chart)) {
        return 1;
    }
    if (!load_moves(moves_path)) {
        return 1;
    }
    if (!load_weights(weight_path, roster, count)) {
        return 1;
    }

    int unresolved = resolve_roster_moves(roster, count);
    if (unresolved > 0) {
        fprintf(stderr, "warning: %d roster moves are missing from %s\n",
                unresolved, moves_path);
    }
    battle_seed(0x5eed1e);
    battle_prepare(roster, count);

    if (testing) {
        return run_tests(roster, count, chart) ? 0 : 1;
    }
    if (reporting) {
        printf("PokeSim -- data layer\n");
        report(roster, count, chart);
        return 0;
    }
    if (tourney) {
        report_tournament(roster, count, chart, tourney_runs);
        return 0;
    }
    if (duelling) {
        if (duel_a == NULL || duel_b == NULL) {
            fprintf(stderr, "usage: --duel \"Name A\" \"Name B\" [runs]\n");
            return 1;
        }
        const Pokemon *a = find_species(roster, count, duel_a);
        const Pokemon *b = find_species(roster, count, duel_b);
        if (a == NULL) {
            fprintf(stderr, "No such Pokemon: %s\n", duel_a);
            return 1;
        }
        if (b == NULL) {
            fprintf(stderr, "No such Pokemon: %s\n", duel_b);
            return 1;
        }
        report_duel(a, b, chart, duel_runs);
        return 0;
    }

    static AppState state;
    state.roster      = roster;
    state.count       = count;
    state.chart       = chart;
    state.type_filter = TYPE_NONE;

    GtkApplication *app = gtk_application_new("com.ricky.pokesim",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state);

    /*
     * Our own flags are already handled above, so GTK is given no arguments
     * at all -- otherwise it would reject --report and --test as unknown
     * options before our code ever saw them.
     */
    int status = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    return status;
}
