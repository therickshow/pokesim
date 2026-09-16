/*
 * thestrongestpokemon -- Stage 2: a GTK front end for the data layer.
 *
 * The loader lives in thestrongestpokemon_data.c. This file is the part you
 * look at: a browsable Pokedex with search, filtering, sortable columns, a
 * detail panel, and the full type chart.
 *
 *     thestrongestpokemon              open the window
 *     thestrongestpokemon --report     the old console summary
 *     thestrongestpokemon --test       run the self-tests
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

#include "thestrongestpokemon_data.h"

#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Columns in the list store behind the table. */
enum {
    COL_DEX, COL_NAME, COL_TYPE1, COL_TYPE2,
    COL_HP, COL_ATK, COL_DEF, COL_SPA, COL_SPD, COL_SPE, COL_TOTAL,
    COL_INDEX,          /* index into roster[] -- hidden from the user */
    N_COLUMNS
};

enum { MOVE_COL_LEVEL, MOVE_COL_NAME, MOVE_N_COLUMNS };

/*
 * The window is a fixed 16:10 -- the same shape as the display -- and cannot
 * be resized. Every width further down is budgeted against these numbers, so
 * allowing a resize would only let the user break the layout. They are
 * logical pixels: on this HiDPI screen GTK doubles them for you.
 */
#define WINDOW_WIDTH   1200
#define WINDOW_HEIGHT   750
#define PANEL_WIDTH     380
#define TABLE_WIDTH     780

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
} AppState;

static void get_stats(const Pokemon *p, int out[6])
{
    out[0] = p->hp;     out[1] = p->attack; out[2] = p->defense;
    out[3] = p->sp_atk; out[4] = p->sp_def; out[5] = p->speed;
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
        "separator { background-color: #39404a; }\n";

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
    add_column(tree, title, column, 64, TRUE);
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
                                      G_TYPE_INT, G_TYPE_INT);

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

    /* 50 + 124 + 76 + 76 + 7*64 = 774, just inside TABLE_WIDTH. */
    add_column(state->tree, "#",    COL_DEX,   50,  TRUE);
    add_column(state->tree, "Name", COL_NAME,  124, FALSE);
    add_column(state->tree, "Type", COL_TYPE1, 76,  FALSE);
    add_column(state->tree, "",     COL_TYPE2, 76,  FALSE);
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

    state->detail_name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->detail_name), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(state->detail_name),
                                "title");
    gtk_box_pack_start(GTK_BOX(details), state->detail_name, FALSE, FALSE, 0);

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

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "The Strongest Pokemon -- Pokedex");
    gtk_window_set_default_size(GTK_WINDOW(window), WINDOW_WIDTH, WINDOW_HEIGHT);
    gtk_widget_set_size_request(window, WINDOW_WIDTH, WINDOW_HEIGHT);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_dex_page(state),
                             gtk_label_new("Pokedex"));
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
    int         testing     = 0;
    int         reporting   = 0;
    int         paths_given = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            testing = 1;
        } else if (strcmp(argv[i], "--report") == 0) {
            reporting = 1;
        } else if (paths_given == 0) {
            roster_path = argv[i];
            paths_given++;
        } else {
            chart_path = argv[i];
            paths_given++;
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

    if (testing) {
        return run_tests(roster, count, chart) ? 0 : 1;
    }
    if (reporting) {
        printf("thestrongestpokemon -- data layer\n");
        report(roster, count, chart);
        return 0;
    }

    static AppState state;
    state.roster      = roster;
    state.count       = count;
    state.chart       = chart;
    state.type_filter = TYPE_NONE;

    GtkApplication *app = gtk_application_new("com.ricky.thestrongestpokemon",
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
