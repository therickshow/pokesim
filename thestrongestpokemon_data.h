/*
 * thestrongestpokemon_data.h -- the roster and type chart, and how to load them.
 *
 * This header is the boundary between "where the data comes from" and "what we
 * do with it". thestrongestpokemon_data.c owns the parsing; the GUI and the
 * console report both just include this and use the results. Splitting it this
 * way means the loader can be tested and reused without dragging GTK along.
 *
 * Note there is no main() in the .c file that implements this. build.py
 * compiles every .c in the folder into one executable, so exactly one file may
 * define main() -- that file is thestrongestpokemon.c.
 */

#ifndef THESTRONGESTPOKEMON_DATA_H
#define THESTRONGESTPOKEMON_DATA_H

#include <stddef.h>

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

/* Highest single base stat in the game (Blissey's 255 HP). Used to scale
 * the stat bars in the GUI so they are comparable between species. */
#define MAX_SINGLE_STAT   255

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

/*
 * Type order matters and is NOT alphabetical: it is the order the columns
 * appear in type_chart_18x18.csv. load_type_chart() checks the file's header
 * against this array and refuses to run if they disagree, because a silently
 * transposed chart would make every battle wrong in a way nothing would catch.
 */
extern const char *TYPE_NAMES[TYPE_COUNT];

/* Canonical colour per type, same order as TYPE_NAMES. GUI only. */
extern const char *TYPE_COLOURS[TYPE_COUNT];

int         stat_sum(const Pokemon *p);
int         type_index(const char *name);   /* TYPE_NONE if not a type */
const char *type_name(int index);           /* "-" if out of range     */

/*
 * Both return 1 on success and 0 on failure, printing the reason to stderr.
 * Each tries the path as given, then one and two directories up, so the
 * program works whether it is run from the project folder or from build/.
 */
int load_roster(const char *path, Pokemon *roster, int *count);
int load_type_chart(const char *path, double chart[TYPE_COUNT][TYPE_COUNT]);

/*
 * Effectiveness of an attacking type against a (possibly dual-typed)
 * defender. Dual types multiply, which is why Charizard takes 4x from Rock.
 */
double effectiveness(double chart[TYPE_COUNT][TYPE_COUNT],
                     int attack_type, const Pokemon *defender);

/*
 * Exposed only so the self-tests can reach it: this is the piece most likely
 * to be subtly wrong, so it gets tested directly rather than through a file.
 */
int split_csv(char *line, char *fields[], int max);

#endif /* THESTRONGESTPOKEMON_DATA_H */
