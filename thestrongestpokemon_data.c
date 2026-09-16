/*
 * thestrongestpokemon_data.c -- loading and validating the CSVs.
 *
 * Nothing in here knows about GTK or about the console. It turns two files on
 * disk into an array of Pokemon and an 18x18 chart, and refuses loudly if
 * anything about them is not what we expect.
 */

#include "thestrongestpokemon_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *TYPE_NAMES[TYPE_COUNT] = {
    "Normal",   "Fighting", "Flying", "Poison", "Ground", "Rock",
    "Bug",      "Ghost",    "Steel",  "Fire",   "Water",  "Grass",
    "Electric", "Psychic",  "Ice",    "Dragon", "Dark",   "Fairy"
};

/* The familiar type colours, in the same order as TYPE_NAMES. */
const char *TYPE_COLOURS[TYPE_COUNT] = {
    "#A8A77A", "#C22E28", "#A98FF3", "#A33EA1", "#E2BF65", "#B6A136",
    "#A6B91A", "#735797", "#B7B7CE", "#EE8130", "#6390F0", "#7AC74C",
    "#F7D02C", "#F95587", "#96D9D6", "#6F35FC", "#705746", "#D685AD"
};

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

int stat_sum(const Pokemon *p)
{
    return p->hp + p->attack + p->defense + p->sp_atk + p->sp_def + p->speed;
}

int type_index(const char *name)
{
    for (int i = 0; i < TYPE_COUNT; i++) {
        if (strcmp(TYPE_NAMES[i], name) == 0) {
            return i;
        }
    }
    return TYPE_NONE;
}

const char *type_name(int index)
{
    if (index < 0 || index >= TYPE_COUNT) {
        return "-";
    }
    return TYPE_NAMES[index];
}

/* Strip leading and trailing whitespace (including the CR of CRLF) in place. */
static void trim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, len - start + 1);
    }
}

/*
 * Both CSVs begin with a UTF-8 byte-order mark (EF BB BF). Left in place it
 * becomes part of the very first field, so the header "Dex" does not compare
 * equal to "Dex" and the file looks corrupt for no visible reason.
 */
static void skip_bom(FILE *f)
{
    int a = fgetc(f);
    int b = fgetc(f);
    int c = fgetc(f);
    if (a == 0xEF && b == 0xBB && c == 0xBF) {
        return;                 /* BOM consumed, carry on */
    }
    rewind(f);
}

/*
 * Open a data file, trying the path as given and then one and two directories
 * up. The executable lives in build/, so double-clicking it there would
 * otherwise fail to find CSVs that sit in the project folder -- a confusing
 * "file not found" for something that works fine from the terminal.
 */
static FILE *open_data(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        return f;
    }

    static const char *prefixes[] = { "../", "../../" };
    char candidate[512];
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        snprintf(candidate, sizeof candidate, "%s%s", prefixes[i], path);
        f = fopen(candidate, "rb");
        if (f != NULL) {
            return f;
        }
    }
    return NULL;
}

/*
 * Split `line` on commas, in place, storing pointers in `fields`.
 * Returns the number of fields, or -1 if there were more than `max`.
 *
 * This deliberately does not use strtok(): strtok treats a run of delimiters
 * as one, so ",," would collapse and every column after an empty field would
 * shift left by one. Type2 is empty for 499 of the 1025 species, so that bug
 * would mis-type roughly half the roster while looking like it worked.
 */
int split_csv(char *line, char *fields[], int max)
{
    if (max <= 0) {
        return -1;
    }
    int n = 0;
    fields[n++] = line;
    for (char *c = line; *c != '\0'; c++) {
        if (*c == ',') {
            *c = '\0';
            if (n >= max) {
                return -1;
            }
            fields[n++] = c + 1;
        }
    }
    return n;
}

/* Parse a whole number, rejecting empty strings and trailing rubbish. */
static int parse_int(const char *s, int *out)
{
    if (s == NULL || *s == '\0') {
        return 0;
    }
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return 0;
    }
    *out = (int)value;
    return 1;
}

/* ------------------------------------------------------------------ *
 * Move list:  "Tackle (Lv 1); Growl (Lv 1); Vine Whip (Lv 3)"
 * ------------------------------------------------------------------ */

static int parse_moves(char *field, Pokemon *p, int line_no)
{
    p->move_count = 0;

    char *entry = field;
    while (entry != NULL && *entry != '\0') {
        char *semicolon = strchr(entry, ';');
        if (semicolon != NULL) {
            *semicolon = '\0';
        }
        trim(entry);

        if (*entry != '\0') {
            if (p->move_count >= MAX_MOVES) {
                fprintf(stderr, "line %d: %s has more than %d moves\n",
                        line_no, p->name, MAX_MOVES);
                return 0;
            }

            /* No move name contains '(', so the last one opens "(Lv N)". */
            char  *open      = strrchr(entry, '(');
            size_t entry_len = strlen(entry);
            if (open == NULL || entry_len == 0 || entry[entry_len - 1] != ')') {
                fprintf(stderr, "line %d: malformed move entry \"%s\"\n",
                        line_no, entry);
                return 0;
            }

            int level = 0;
            if (sscanf(open, "(Lv %d)", &level) != 1 || level < 0 || level > 100) {
                fprintf(stderr, "line %d: bad level in move entry \"%s\"\n",
                        line_no, entry);
                return 0;
            }

            *open = '\0';       /* cut the "(Lv N)" off, leaving the name */
            trim(entry);

            if (*entry == '\0' || strlen(entry) >= MOVE_NAME_LEN) {
                fprintf(stderr,
                        "line %d: move name \"%s\" is empty or too long "
                        "(max %d bytes)\n",
                        line_no, entry, MOVE_NAME_LEN - 1);
                return 0;
            }

            Move *m = &p->moves[p->move_count++];
            snprintf(m->name, sizeof m->name, "%s", entry);
            m->level = level;
        }

        entry = (semicolon != NULL) ? semicolon + 1 : NULL;
    }

    return 1;
}

/* ------------------------------------------------------------------ *
 * Roster loader
 * ------------------------------------------------------------------ */

int load_roster(const char *path, Pokemon *roster, int *count)
{
    FILE *f = open_data(path);
    if (f == NULL) {
        fprintf(stderr, "Cannot open roster file: %s\n", path);
        return 0;
    }
    skip_bom(f);

    char  line[LINE_LEN];
    char *fields[FIELD_COUNT];
    int   line_no = 0;
    int   n       = 0;

    /* Header */
    if (fgets(line, sizeof line, f) == NULL) {
        fprintf(stderr, "%s is empty\n", path);
        fclose(f);
        return 0;
    }
    line_no++;
    trim(line);
    if (split_csv(line, fields, FIELD_COUNT) != FIELD_COUNT ||
        strcmp(fields[0], "Dex") != 0 || strcmp(fields[1], "Name") != 0) {
        fprintf(stderr, "%s: unexpected header row\n", path);
        fclose(f);
        return 0;
    }

    while (fgets(line, sizeof line, f) != NULL) {
        line_no++;

        /* A truncated line means LINE_LEN is too small -- say so, don't guess. */
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n') {
            fprintf(stderr, "line %d: longer than %d bytes -- raise LINE_LEN\n",
                    line_no, LINE_LEN);
            fclose(f);
            return 0;
        }

        trim(line);
        if (*line == '\0') {
            continue;           /* tolerate a blank final line */
        }

        if (n >= MAX_POKEMON) {
            fprintf(stderr, "line %d: more than %d species -- raise MAX_POKEMON\n",
                    line_no, MAX_POKEMON);
            fclose(f);
            return 0;
        }

        int got = split_csv(line, fields, FIELD_COUNT);
        if (got != FIELD_COUNT) {
            fprintf(stderr, "line %d: expected %d columns, found %d\n",
                    line_no, FIELD_COUNT, got);
            fclose(f);
            return 0;
        }
        for (int i = 0; i < FIELD_COUNT; i++) {
            trim(fields[i]);
        }

        Pokemon *p = &roster[n];
        memset(p, 0, sizeof *p);

        if (!parse_int(fields[0], &p->dex)) {
            fprintf(stderr, "line %d: bad dex number \"%s\"\n", line_no, fields[0]);
            fclose(f);
            return 0;
        }
        if (p->dex != n + 1) {
            fprintf(stderr,
                    "line %d: dex numbers are not contiguous "
                    "(expected %d, found %d)\n",
                    line_no, n + 1, p->dex);
            fclose(f);
            return 0;
        }

        if (*fields[1] == '\0' || strlen(fields[1]) >= NAME_LEN) {
            fprintf(stderr,
                    "line %d: name \"%s\" is empty or too long (max %d bytes)\n",
                    line_no, fields[1], NAME_LEN - 1);
            fclose(f);
            return 0;
        }
        snprintf(p->name, sizeof p->name, "%s", fields[1]);

        p->type1 = type_index(fields[2]);
        if (p->type1 == TYPE_NONE) {
            fprintf(stderr, "line %d: %s has unknown type \"%s\"\n",
                    line_no, p->name, fields[2]);
            fclose(f);
            return 0;
        }
        /* An empty second type is normal; a non-empty unknown one is not. */
        if (*fields[3] == '\0') {
            p->type2 = TYPE_NONE;
        } else {
            p->type2 = type_index(fields[3]);
            if (p->type2 == TYPE_NONE) {
                fprintf(stderr, "line %d: %s has unknown second type \"%s\"\n",
                        line_no, p->name, fields[3]);
                fclose(f);
                return 0;
            }
        }

        int *stats[6] = { &p->hp,     &p->attack, &p->defense,
                          &p->sp_atk, &p->sp_def, &p->speed };
        for (int i = 0; i < 6; i++) {
            if (!parse_int(fields[4 + i], stats[i]) || *stats[i] < 1) {
                fprintf(stderr, "line %d: %s has bad stat \"%s\"\n",
                        line_no, p->name, fields[4 + i]);
                fclose(f);
                return 0;
            }
        }

        if (!parse_int(fields[10], &p->total)) {
            fprintf(stderr, "line %d: %s has bad total \"%s\"\n",
                    line_no, p->name, fields[10]);
            fclose(f);
            return 0;
        }

        /*
         * The checksum. Every row in the supplied file states a Total equal to
         * the sum of its six stats, so a mismatch means the row was misparsed
         * -- almost certainly a column shift. Fail loudly rather than quietly
         * ranking nonsense.
         */
        if (p->total != stat_sum(p)) {
            fprintf(stderr,
                    "line %d: %s -- stated total %d but stats sum to %d "
                    "(columns misaligned?)\n",
                    line_no, p->name, p->total, stat_sum(p));
            fclose(f);
            return 0;
        }

        if (!parse_moves(fields[11], p, line_no)) {
            fclose(f);
            return 0;
        }

        n++;
    }

    fclose(f);
    *count = n;
    return 1;
}

/* ------------------------------------------------------------------ *
 * Type chart loader
 * ------------------------------------------------------------------ */

int load_type_chart(const char *path, double chart[TYPE_COUNT][TYPE_COUNT])
{
    FILE *f = open_data(path);
    if (f == NULL) {
        fprintf(stderr, "Cannot open type chart file: %s\n", path);
        return 0;
    }
    skip_bom(f);

    char  line[LINE_LEN];
    char *fields[TYPE_COUNT + 1];

    if (fgets(line, sizeof line, f) == NULL) {
        fprintf(stderr, "%s is empty\n", path);
        fclose(f);
        return 0;
    }
    trim(line);
    if (split_csv(line, fields, TYPE_COUNT + 1) != TYPE_COUNT + 1) {
        fprintf(stderr, "%s: header should have %d columns\n",
                path, TYPE_COUNT + 1);
        fclose(f);
        return 0;
    }
    /* Guard against a transposed or reordered chart. */
    for (int i = 0; i < TYPE_COUNT; i++) {
        trim(fields[i + 1]);
        if (strcmp(fields[i + 1], TYPE_NAMES[i]) != 0) {
            fprintf(stderr, "%s: column %d is \"%s\", expected \"%s\"\n",
                    path, i + 1, fields[i + 1], TYPE_NAMES[i]);
            fclose(f);
            return 0;
        }
    }

    for (int row = 0; row < TYPE_COUNT; row++) {
        if (fgets(line, sizeof line, f) == NULL) {
            fprintf(stderr, "%s: expected %d rows, found %d\n",
                    path, TYPE_COUNT, row);
            fclose(f);
            return 0;
        }
        trim(line);
        if (split_csv(line, fields, TYPE_COUNT + 1) != TYPE_COUNT + 1) {
            fprintf(stderr, "%s: row %d should have %d columns\n",
                    path, row + 1, TYPE_COUNT + 1);
            fclose(f);
            return 0;
        }
        trim(fields[0]);
        if (strcmp(fields[0], TYPE_NAMES[row]) != 0) {
            fprintf(stderr, "%s: row %d is \"%s\", expected \"%s\"\n",
                    path, row + 1, fields[0], TYPE_NAMES[row]);
            fclose(f);
            return 0;
        }

        for (int col = 0; col < TYPE_COUNT; col++) {
            trim(fields[col + 1]);
            char  *end   = NULL;
            double value = strtod(fields[col + 1], &end);
            if (end == fields[col + 1] || *end != '\0') {
                fprintf(stderr, "%s: bad multiplier \"%s\" at %s -> %s\n",
                        path, fields[col + 1], TYPE_NAMES[row], TYPE_NAMES[col]);
                fclose(f);
                return 0;
            }
            if (value != 0.0 && value != 0.5 && value != 1.0 && value != 2.0) {
                fprintf(stderr, "%s: unexpected multiplier %g at %s -> %s\n",
                        path, value, TYPE_NAMES[row], TYPE_NAMES[col]);
                fclose(f);
                return 0;
            }
            chart[row][col] = value;
        }
    }

    fclose(f);
    return 1;
}

double effectiveness(double chart[TYPE_COUNT][TYPE_COUNT],
                     int attack_type, const Pokemon *defender)
{
    double multiplier = chart[attack_type][defender->type1];
    if (defender->type2 != TYPE_NONE) {
        multiplier *= chart[attack_type][defender->type2];
    }
    return multiplier;
}
