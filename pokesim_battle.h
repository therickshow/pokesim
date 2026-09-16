/*
 * pokesim_battle.h -- the battle simulator.
 *
 * One battle is two Pokemon, four moves each, alternating turns in speed
 * order until one faints or the turn cap is hit. Battles are random: damage
 * rolls, critical hits, accuracy and status all use the RNG, so a single
 * result means little and the interesting number is the win rate over many
 * repeats. simulate_series() does exactly that.
 */

#ifndef POKESIM_BATTLE_H
#define POKESIM_BATTLE_H

#include "pokesim_data.h"

#define TEAM_MOVES   4          /* every Pokemon carries four moves        */
#define TURN_CAP  1000          /* see the note on draws below             */

/*
 * The L in the damage formula, and the one correction applied to raw stats.
 *
 * Ricky chose raw base stats rather than the level/IV/EV formula. That is
 * fine for five of the six stats, because damage depends on Attack *divided
 * by* Defence -- and a ratio does not care what scale its two halves are on.
 * Raw 84/85 gives the same answer as a real 173/175.
 *
 * HP is the exception: it is used on its own, not as a ratio. A real level-50
 * Pokemon has roughly 2.5 to 4 times its base HP, so using base HP directly
 * left everything far too fragile -- battles finished in two turns and frail
 * Pokemon lost through 4x type advantages simply because 35 HP cannot absorb
 * two hits. Pikachu out-damaged Gyarados by a factor of two and still lost
 * 999 fights out of 1000.
 *
 * So HP alone is multiplied by HP_SCALE to put it back on the same footing as
 * the other five, and L goes back to a normal 50. That also restores the
 * fixed-damage moves: Seismic Toss and Night Shade deal damage equal to L, so
 * at L=5 they were doing 5 damage and were effectively dead moves.
 *
 * 14 was chosen by measurement, not taste. Sweeping the value and timing
 * battles gives 4.6 turns at 8, 7.0 at 12, 8.3 at 14, 10.1 at 16 and 14.4 at
 * 20, scaling cleanly with no runaway tail now that PP and Struggle end
 * stalemates. 14 puts the average fight at about eight turns and the longest
 * at under twenty -- enough room for Toxic to ramp, for a setup move to pay
 * for itself and for healing to matter, without the whole tournament
 * slowing down. */
#define BATTLE_LEVEL 50
#define HP_SCALE     14

typedef enum {
    STATUS_NONE = 0,
    STATUS_BURN,
    STATUS_POISON,
    STATUS_TOXIC,
    STATUS_PARALYSIS,
    STATUS_SLEEP,
    STATUS_FREEZE
} StatusCondition;

typedef enum { RESULT_A_WINS = 0, RESULT_B_WINS, RESULT_DRAW } BattleResult;

/* Everything worth reporting about a run of battles between two species. */
typedef struct {
    int       battles;
    int       a_wins, b_wins, draws;
    long long total_turns;
    int       min_turns, max_turns;
    long long a_damage, b_damage;       /* total dealt across all battles  */
    int       a_crits, b_crits;
    int       a_misses, b_misses;
    int       a_move_used[TEAM_MOVES];  /* how often each move was chosen  */
    int       b_move_used[TEAM_MOVES];
    int       a_hp_left, b_hp_left;     /* summed over battles they won    */
} SeriesStats;

/* Seed the generator. Same seed plus same pairing gives the same results. */
void battle_seed(unsigned int seed);

/*
 * The four moves a species fights with: the last four it learns, by level.
 * Moves the engine cannot model (Fling and Natural Gift need held items,
 * Bide and Spit Up need multi-turn state) are skipped. Species with fewer
 * than four usable moves simply carry fewer.
 */
void choose_moveset(const Pokemon *p, int out[TEAM_MOVES], int *count);

/* A single battle. `stats` may be NULL if only the winner matters. */
BattleResult simulate_battle(const Pokemon *a, const Pokemon *b,
                             double chart[TYPE_COUNT][TYPE_COUNT],
                             SeriesStats *stats);

/* Run `runs` battles and fill `out`. Zeroes `out` first. */
void simulate_series(const Pokemon *a, const Pokemon *b,
                     double chart[TYPE_COUNT][TYPE_COUNT],
                     int runs, SeriesStats *out);

/* Name of a move slot, for reporting. Returns "-" for an empty slot. */
const char *moveset_name(const Pokemon *p, int slot);

/*
 * Build the lookup tables the engine needs. Must be called once after
 * load_moves() and resolve_roster_moves(), before any battle runs.
 */
void battle_prepare(const Pokemon *roster, int count);

/* One species' record across a whole round-robin. */
typedef struct {
    int       dex;
    int       wins, losses, draws;
    int       battles;
    long long damage_dealt, damage_taken;
    long long turns;
} RankEntry;

/*
 * Every species against every other, `runs_per_pair` times each. `out` must
 * have room for `count` entries and is filled in dex order.
 *
 * `progress` is called occasionally with a fraction from 0 to 1 -- it runs on
 * whichever thread called this, so a GUI must marshal back to the main loop.
 * Setting *cancel to non-zero stops the run early. Both may be NULL.
 */
typedef void (*ProgressFn)(double fraction, void *user_data);

void run_tournament(const Pokemon *roster, int count,
                    double chart[TYPE_COUNT][TYPE_COUNT],
                    int runs_per_pair, RankEntry *out,
                    volatile int *cancel, ProgressFn progress, void *user_data);

#endif /* POKESIM_BATTLE_H */
