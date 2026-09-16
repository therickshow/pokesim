/*
 * thestrongestpokemon -- Stage 1: the data layer.
 *
 * Loads the 1025-species roster and the 18x18 type chart, checks that both are
 * internally consistent, and reports what it found. There are no battles yet.
 * The whole point of this stage is to prove the data is sound before any
 * ranking logic is built on top of it.
 *
 *     thestrongestpokemon                 load, verify, report
 *     thestrongestpokemon --test          run the self-tests
 *
 * Author: Ricky
 * Created: 2026-09-16
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ------------------------------------------------------------------ *
 * Sizes.  Every one of these is set from the real data, not guessed.
 * The numbers in the comments are what the supplied CSVs actually contain.
 * ------------------------------------------------------------------ */

#define MAX_POKEMON      1025   /* exactly 1025 species in the file       */
#define MAX_MOVES          40   /* most is Gallade with 33                */
#define NAME_LEN           24   /* longest is 12 bytes (Farfetch'd)       */
#define MOVE_NAME_LEN      32   /* longest is 18 bytes (Nature's Madness) */
#define LINE_LEN         2048   /* longest raw line is 669 bytes          */
#define TYPE_COUNT         18
#define FIELD_COUNT        12   /* columns in the roster CSV              */

#define TYPE_NONE          (-1) /* a single-typed Pokemon's second type   */

/*
 * Type order matters and is NOT alphabetical: it is the order the columns
 * appear in type_chart_18x18.csv. If these two ever disagree the chart gets
 * silently transposed, so load_type_chart() checks the header against this
 * array and refuses to run if they differ.
 */
static const char *TYPE_NAMES[TYPE_COUNT] = {
    "Normal",   "Fighting", "Flying", "Poison", "Ground", "Rock",
    "Bug",      "Ghost",    "Steel",  "Fire",   "Water",  "Grass",
    "Electric", "Psychic",  "Ice",    "Dragon", "Dark",   "Fairy"
};

typedef struct {
    char name[MOVE_NAME_LEN];
    int  level;                 /* 0 means "learned on evolution" */
} Move;

typedef struct {
    int  dex;
    char name[NAME_LEN];
    int  type1;                 /* index into TYPE_NAMES         */
    int  type2;                 /* index, or TYPE_NONE if single */
    int  hp, attack, defense, sp_atk, sp_def, speed;
    int  total;                 /* as stated in the file         */
    Move moves[MAX_MOVES];
    int  move_count;
} Pokemon;

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

static int stat_sum(const Pokemon *p)
{
    return p->hp + p->attack + p->defense + p->sp_atk + p->sp_def + p->speed;
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
 * Split `line` on commas, in place, storing pointers in `fields`.
 * Returns the number of fields, or -1 if there were more than `max`.
 *
 * This deliberately does not use strtok(): strtok treats a run of delimiters
 * as one, so ",," would collapse and every column after an empty field would
 * shift left by one. Type2 is empty for 499 of the 1025 species, so that bug
 * would mis-type roughly half the roster while looking like it worked.
 */
static int split_csv(char *line, char *fields[], int max)
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

static int type_index(const char *name)
{
    for (int i = 0; i < TYPE_COUNT; i++) {
        if (strcmp(TYPE_NAMES[i], name) == 0) {
            return i;
        }
    }
    return TYPE_NONE;
}

static const char *type_name(int index)
{
    if (index < 0 || index >= TYPE_COUNT) {
        return "-";
    }
    return TYPE_NAMES[index];
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

static int load_roster(const char *path, Pokemon *roster, int *count)
{
    FILE *f = fopen(path, "rb");
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

static int load_type_chart(const char *path, double chart[TYPE_COUNT][TYPE_COUNT])
{
    FILE *f = fopen(path, "rb");
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

/*
 * Effectiveness of an attacking type against a (possibly dual-typed) defender.
 * Dual types multiply, which is why Charizard takes 4x from Rock.
 */
static double effectiveness(double chart[TYPE_COUNT][TYPE_COUNT],
                            int attack_type, const Pokemon *defender)
{
    double multiplier = chart[attack_type][defender->type1];
    if (defender->type2 != TYPE_NONE) {
        multiplier *= chart[attack_type][defender->type2];
    }
    return multiplier;
}

/* ------------------------------------------------------------------ *
 * Report
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

    printf("\n=== Samples ===\n");
    print_pokemon(&roster[0]);          /* Bulbasaur:  dual-typed   */
    print_pokemon(&roster[3]);          /* Charmander: single-typed */
    print_pokemon(&roster[82]);         /* Farfetch'd: UTF-8 name   */
    print_pokemon(&roster[668]);        /* Flabebe:    UTF-8 name   */
    print_pokemon(&roster[count - 1]);  /* Pecharunt:  last row     */

    printf("\n=== Highest base stat total ===\n");
    print_pokemon(&roster[best]);

    printf("\n=== Species knowing only one move ===\n");
    printf("  (these cannot win a fight, and two of them fight forever --\n");
    printf("   this is why the battle loop will need a turn cap)\n");
    for (int i = 0; i < count; i++) {
        if (roster[i].move_count == 1) {
            printf("  #%-4d %-12s knows only %s\n",
                   roster[i].dex, roster[i].name, roster[i].moves[0].name);
        }
    }

    printf("\n=== Type chart spot checks ===\n");
    const Pokemon *charizard = &roster[5];
    const Pokemon *gengar    = &roster[93];
    printf("  Rock   -> %-10s (%s/%s) = %gx\n", charizard->name,
           type_name(charizard->type1), type_name(charizard->type2),
           effectiveness(chart, type_index("Rock"), charizard));
    printf("  Water  -> %-10s (%s/%s) = %gx\n", charizard->name,
           type_name(charizard->type1), type_name(charizard->type2),
           effectiveness(chart, type_index("Water"), charizard));
    printf("  Normal -> %-10s (%s/%s) = %gx\n", gengar->name,
           type_name(gengar->type1), type_name(gengar->type2),
           effectiveness(chart, type_index("Normal"), gengar));
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

    /* No duplicate names -- a name is the natural lookup key later. */
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

    /* The species that make a turn cap necessary. */
    check(roster[10].move_count == 1 &&
          strcmp(roster[10].moves[0].name, "Harden") == 0,
          "Metapod knows exactly one move, and it is Harden");
    check(roster[131].move_count == 1 &&
          strcmp(roster[131].moves[0].name, "Transform") == 0,
          "Ditto knows exactly one move, and it is Transform");

    /* Evolution moves are recorded as level 0, not dropped. */
    int lv0 = 0;
    for (int i = 0; i < count; i++) {
        for (int m = 0; m < roster[i].move_count; m++) {
            if (roster[i].moves[m].level == 0) {
                lv0++;
            }
        }
    }
    check(lv0 == 271, "271 moves are learned on evolution (Lv 0)");

    /* Multi-word move names parsed whole, with their levels. */
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
    check(chart[type_index("Fire")][type_index("Grass")] == 2.0,
          "Fire beats Grass");
    check(chart[type_index("Water")][type_index("Fire")] == 2.0,
          "Water beats Fire");
    check(chart[type_index("Normal")][type_index("Ghost")] == 0.0,
          "Normal cannot hit Ghost");
    check(chart[type_index("Electric")][type_index("Ground")] == 0.0,
          "Electric cannot hit Ground");
    check(chart[type_index("Dragon")][type_index("Fairy")] == 0.0,
          "Dragon cannot hit Fairy");
    check(chart[type_index("Fighting")][type_index("Rock")] == 2.0,
          "Fighting beats Rock");

    /* Dual types multiply. */
    check(effectiveness(chart, type_index("Rock"), &roster[5]) == 4.0,
          "Rock hits Charizard (Fire/Flying) for 4x");
    check(effectiveness(chart, type_index("Normal"), &roster[93]) == 0.0,
          "Normal hits Gengar (Ghost/Poison) for 0x");
    check(effectiveness(chart, type_index("Electric"), &roster[129]) == 4.0,
          "Electric hits Gyarados (Water/Flying) for 4x");

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
     * are correct either way; this only fixes what you see.
     */
    SetConsoleOutputCP(CP_UTF8);
#endif

    const char *roster_path = "pokemon_1025_stats_types_moves.csv";
    const char *chart_path  = "type_chart_18x18.csv";
    int         testing     = 0;
    int         paths_given = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            testing = 1;
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

    printf("thestrongestpokemon -- Stage 1: data layer\n");
    printf("Loaded %s and %s\n", roster_path, chart_path);
    report(roster, count, chart);
    printf("\nRun with --test to check the loader.\n");
    return 0;
}
