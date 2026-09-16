/*
 * thestrongestpokemon_battle.c -- the battle simulator.
 *
 * The damage formula is the real one:
 *
 *   Damage = ( ((2L/5 + 2) * Power * A/D) / 50 + 2 ) * STAB * Type * Crit
 *            * Random * Burn
 *
 * with L fixed at BATTLE_LEVEL (see the header for why). A/D is Attack over
 * Defense for physical moves and Sp. Atk over Sp. Def for special ones.
 *
 * Twenty-nine damaging moves have no fixed power and are worked out here
 * instead -- Seismic Toss deals damage equal to the level, Gyro Ball scales
 * with the speed ratio, Low Kick with the target's weight, and so on.
 */

#include "thestrongestpokemon_battle.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * Random numbers
 *
 * A small xorshift generator rather than rand(): it is fast, it has the same
 * sequence on every platform, and seeding it explicitly makes any single
 * battle reproducible when something looks wrong.
 * ------------------------------------------------------------------ */

static unsigned int rng_state = 2463534242u;

void battle_seed(unsigned int seed)
{
    rng_state = (seed == 0) ? 2463534242u : seed;
}

static unsigned int rng_next(void)
{
    unsigned int x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* Uniform in [0, n). */
static int rng_below(int n)
{
    return (n <= 0) ? 0 : (int)(rng_next() % (unsigned int)n);
}

/* True with probability percent/100. */
static int rng_chance(int percent)
{
    return rng_below(100) < percent;
}

/* ------------------------------------------------------------------ *
 * Moves the engine cannot model, and moves with computed power
 * ------------------------------------------------------------------ */

typedef enum {
    SP_NONE = 0,
    SP_UNUSABLE,        /* needs data this simulator does not have  */
    SP_LEVEL_DAMAGE,    /* Seismic Toss, Night Shade                */
    SP_HALVE_HP,        /* Super Fang, Nature's Madness             */
    SP_ENDEAVOR,
    SP_FINAL_GAMBIT,
    SP_COUNTER,         /* 2x the last physical damage taken        */
    SP_MIRROR_COAT,     /* 2x the last special damage taken         */
    SP_METAL_BURST,     /* 1.5x the last damage taken               */
    SP_FLAIL,           /* power rises as the user's HP falls       */
    SP_ELECTRO_BALL,    /* power rises with the speed advantage     */
    SP_GYRO_BALL,       /* power rises with the speed disadvantage  */
    SP_WRING_OUT,       /* power scales with the target's HP        */
    SP_LOW_KICK,        /* power from the target's weight           */
    SP_HEAVY_SLAM,      /* power from the weight ratio              */
    SP_OHKO,            /* Fissure, Guillotine, Horn Drill, Sheer Cold */
    SP_PRESENT,
    SP_BEAT_UP
} SpecialMove;

static const struct { const char *name; SpecialMove kind; } SPECIAL_MOVES[] = {
    { "Seismic Toss",     SP_LEVEL_DAMAGE },
    { "Night Shade",      SP_LEVEL_DAMAGE },
    { "Super Fang",       SP_HALVE_HP     },
    { "Nature\xe2\x80\x99s Madness", SP_HALVE_HP },
    { "Endeavor",         SP_ENDEAVOR     },
    { "Final Gambit",     SP_FINAL_GAMBIT },
    { "Counter",          SP_COUNTER      },
    { "Mirror Coat",      SP_MIRROR_COAT  },
    { "Metal Burst",      SP_METAL_BURST  },
    { "Flail",            SP_FLAIL        },
    { "Reversal",         SP_FLAIL        },
    { "Electro Ball",     SP_ELECTRO_BALL },
    { "Gyro Ball",        SP_GYRO_BALL    },
    { "Crush Grip",       SP_WRING_OUT    },
    { "Wring Out",        SP_WRING_OUT    },
    { "Low Kick",         SP_LOW_KICK     },
    { "Grass Knot",       SP_LOW_KICK     },
    { "Heat Crash",       SP_HEAVY_SLAM   },
    { "Heavy Slam",       SP_HEAVY_SLAM   },
    { "Fissure",          SP_OHKO         },
    { "Guillotine",       SP_OHKO         },
    { "Horn Drill",       SP_OHKO         },
    { "Sheer Cold",       SP_OHKO         },
    { "Present",          SP_PRESENT      },
    { "Beat Up",          SP_BEAT_UP      },
    /* No held items and no multi-turn state, so these four do nothing. */
    { "Fling",            SP_UNUSABLE     },
    { "Natural Gift",     SP_UNUSABLE     },
    { "Bide",             SP_UNUSABLE     },
    { "Spit Up",          SP_UNUSABLE     },
};

static SpecialMove special_kind(const char *name)
{
    for (size_t i = 0; i < sizeof SPECIAL_MOVES / sizeof SPECIAL_MOVES[0]; i++) {
        if (strcmp(SPECIAL_MOVES[i].name, name) == 0) {
            return SPECIAL_MOVES[i].kind;
        }
    }
    return SP_NONE;
}

/* ------------------------------------------------------------------ *
 * Status moves
 *
 * moves.csv says a move is "status" but not what it does -- that lives in a
 * separate effects table with several hundred entries, most of which are
 * irrelevant to a one-on-one fight. Rather than decode all of them, the ones
 * that genuinely change a duel are listed here by name. Any status move not
 * in this table does nothing, which the move chooser accounts for by never
 * selecting it.
 * ------------------------------------------------------------------ */

enum { ST_ATK = 0, ST_DEF, ST_SPA, ST_SPD, ST_SPE, ST_COUNT };

typedef struct {
    const char     *name;
    int             self[ST_COUNT];
    int             foe[ST_COUNT];
    int             heal_percent;
    StatusCondition inflict;
} StatusEffect;

static const StatusEffect STATUS_EFFECTS[] = {
    /* name                 self atk/def/spa/spd/spe   foe ...           heal  inflict */
    { "Swords Dance",     { 2,0,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Nasty Plot",       { 0,0,2,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Calm Mind",        { 0,0,1,1,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Dragon Dance",     { 1,0,0,0,1}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Bulk Up",          { 1,1,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Quiver Dance",     { 0,0,1,1,1}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Shell Smash",      { 2,-1,2,-1,2},{0,0,0,0,0},  0, STATUS_NONE },
    { "Growth",           { 1,0,1,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Work Up",          { 1,0,1,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Howl",             { 1,0,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Hone Claws",       { 1,0,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Coil",             { 1,1,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Curse",            { 1,1,0,0,-1},{0,0,0,0,0},   0, STATUS_NONE },
    { "Iron Defense",     { 0,2,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Acid Armor",       { 0,2,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Barrier",          { 0,2,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Harden",           { 0,1,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Withdraw",         { 0,1,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Defense Curl",     { 0,1,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Amnesia",          { 0,0,0,2,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Cosmic Power",     { 0,1,0,1,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Agility",          { 0,0,0,0,2}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Rock Polish",      { 0,0,0,0,2}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Autotomize",       { 0,0,0,0,2}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Shift Gear",       { 1,0,0,0,2}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Tail Glow",        { 0,0,3,0,0}, {0,0,0,0,0},   0, STATUS_NONE },
    { "Belly Drum",       { 6,0,0,0,0}, {0,0,0,0,0},   0, STATUS_NONE },

    /* Debuffs */
    { "Growl",            { 0,0,0,0,0}, {-1,0,0,0,0},  0, STATUS_NONE },
    { "Charm",            { 0,0,0,0,0}, {-2,0,0,0,0},  0, STATUS_NONE },
    { "Leer",             { 0,0,0,0,0}, {0,-1,0,0,0},  0, STATUS_NONE },
    { "Tail Whip",        { 0,0,0,0,0}, {0,-1,0,0,0},  0, STATUS_NONE },
    { "Screech",          { 0,0,0,0,0}, {0,-2,0,0,0},  0, STATUS_NONE },
    { "Metal Sound",      { 0,0,0,0,0}, {0,0,0,-2,0},  0, STATUS_NONE },
    { "Fake Tears",       { 0,0,0,0,0}, {0,0,0,-2,0},  0, STATUS_NONE },
    { "Scary Face",       { 0,0,0,0,0}, {0,0,0,0,-2},  0, STATUS_NONE },
    { "String Shot",      { 0,0,0,0,0}, {0,0,0,0,-2},  0, STATUS_NONE },
    { "Cotton Spore",     { 0,0,0,0,0}, {0,0,0,0,-2},  0, STATUS_NONE },

    /* Healing */
    { "Recover",          { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Roost",            { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Synthesis",        { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Moonlight",        { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Morning Sun",      { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Slack Off",        { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Soft-Boiled",      { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Milk Drink",       { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Shore Up",         { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Heal Order",       { 0,0,0,0,0}, {0,0,0,0,0},  50, STATUS_NONE },
    { "Life Dew",         { 0,0,0,0,0}, {0,0,0,0,0},  25, STATUS_NONE },
    { "Rest",             { 0,0,0,0,0}, {0,0,0,0,0}, 100, STATUS_SLEEP },

    /* Status conditions */
    { "Toxic",            { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_TOXIC     },
    { "Will-O-Wisp",      { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_BURN      },
    { "Thunder Wave",     { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_PARALYSIS },
    { "Stun Spore",       { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_PARALYSIS },
    { "Glare",            { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_PARALYSIS },
    { "Nuzzle",           { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_PARALYSIS },
    { "Poison Powder",    { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_POISON    },
    { "Poison Gas",       { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_POISON    },
    { "Sleep Powder",     { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_SLEEP     },
    { "Spore",            { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_SLEEP     },
    { "Hypnosis",         { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_SLEEP     },
    { "Lovely Kiss",      { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_SLEEP     },
    { "Sing",             { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_SLEEP     },
    { "Dark Void",        { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_SLEEP     },
    { "Grass Whistle",    { 0,0,0,0,0}, {0,0,0,0,0},   0, STATUS_SLEEP     },
};

static const StatusEffect *status_effect(const char *name)
{
    for (size_t i = 0; i < sizeof STATUS_EFFECTS / sizeof STATUS_EFFECTS[0]; i++) {
        if (strcmp(STATUS_EFFECTS[i].name, name) == 0) {
            return &STATUS_EFFECTS[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Recoil and drain
 *
 * A move that hurts its user, or heals it, changes who wins a long fight
 * enormously -- a Flare Blitz user trades a third of its own health for every
 * hit, and a Giga Drain user claws half of its damage back. Neither shows up
 * anywhere in moves.csv, so the percentages live here, keyed by name.
 * Anything not listed does neither.
 * ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int         recoil_percent;   /* of the damage dealt, taken by the user */
    int         drain_percent;    /* of the damage dealt, healed to the user */
} RecoilDrain;

static const RecoilDrain RECOIL_DRAIN[] = {
    /* Recoil */
    { "Take Down",        25,  0 },
    { "Submission",       25,  0 },
    { "Wild Charge",      25,  0 },
    { "Head Charge",      25,  0 },
    { "Double-Edge",      33,  0 },
    { "Brave Bird",       33,  0 },
    { "Flare Blitz",      33,  0 },
    { "Volt Tackle",      33,  0 },
    { "Wood Hammer",      33,  0 },
    { "Wave Crash",       33,  0 },
    { "Light of Ruin",    50,  0 },
    { "Head Smash",       50,  0 },

    /* Drain */
    { "Absorb",            0, 50 },
    { "Mega Drain",        0, 50 },
    { "Giga Drain",        0, 50 },
    { "Leech Life",        0, 50 },
    { "Drain Punch",       0, 50 },
    { "Horn Leech",        0, 50 },
    { "Parabolic Charge",  0, 50 },
    { "Bitter Blade",      0, 50 },
    { "Bouncy Bubble",     0, 50 },
    { "Dream Eater",       0, 50 },
    { "Draining Kiss",     0, 75 },
    { "Oblivion Wing",     0, 75 },
    { "Matcha Gotcha",     0, 50 },
};

static const RecoilDrain *recoil_drain(const char *name)
{
    for (size_t i = 0; i < sizeof RECOIL_DRAIN / sizeof RECOIL_DRAIN[0]; i++) {
        if (strcmp(RECOIL_DRAIN[i].name, name) == 0) {
            return &RECOIL_DRAIN[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Precomputed per-move lookups
 *
 * special_kind(), status_effect() and recoil_drain() all walk a table doing
 * strcmp. That is fine once, but the move chooser calls them for every move
 * of every Pokemon on every turn -- and a full round robin is hundreds of
 * millions of turns, where it dominates the runtime. They are resolved once
 * here instead, indexed by move id.
 * ------------------------------------------------------------------ */

static SpecialMove         move_special[MAX_MOVE_TABLE];
static const StatusEffect *move_effect[MAX_MOVE_TABLE];
static const RecoilDrain  *move_rd[MAX_MOVE_TABLE];

/* Movesets are fixed per species, so work them out once rather than on every
 * battler that is created. */
static int cached_moves[MAX_POKEMON + 1][TEAM_MOVES];
static int cached_count[MAX_POKEMON + 1];
static int tables_ready = 0;

static void choose_moveset_uncached(const Pokemon *p, int out[TEAM_MOVES],
                                    int *count);

void battle_prepare(const Pokemon *roster, int count)
{
    for (int i = 0; i < move_table_count && i < MAX_MOVE_TABLE; i++) {
        move_special[i] = special_kind(move_table[i].name);
        move_effect[i]  = status_effect(move_table[i].name);
        move_rd[i]      = recoil_drain(move_table[i].name);
    }
    for (int i = 0; i < count; i++) {
        int dex = roster[i].dex;
        if (dex >= 1 && dex <= MAX_POKEMON) {
            choose_moveset_uncached(&roster[i], cached_moves[dex],
                                    &cached_count[dex]);
        }
    }
    tables_ready = 1;
}

/* ------------------------------------------------------------------ *
 * Battler
 * ------------------------------------------------------------------ */

typedef struct {
    const Pokemon  *sp;
    int             hp, max_hp;
    int             stage[ST_COUNT];
    StatusCondition status;
    int             sleep_turns;
    int             toxic_turns;
    int             moves[TEAM_MOVES];
    int             pp[TEAM_MOVES];
    int             move_count;
    int             last_damage;        /* taken, for Counter and friends */
    MoveCategory    last_damage_cat;
    int             self_ko;            /* Final Gambit */
    int             setup_used;
} Battler;

/* The Gen 3+ stage multipliers, as a ratio to avoid floating point drift. */
static int apply_stage(int base, int stage)
{
    if (stage > 6)  stage = 6;
    if (stage < -6) stage = -6;
    int value = (stage >= 0)
        ? base * (2 + stage) / 2
        : base * 2 / (2 - stage);
    return (value < 1) ? 1 : value;
}

static int eff_attack(const Battler *b)
{
    int a = apply_stage(b->sp->attack, b->stage[ST_ATK]);
    /* A burn halves physical attack. */
    return (b->status == STATUS_BURN) ? (a / 2 > 0 ? a / 2 : 1) : a;
}
static int eff_defense(const Battler *b)  { return apply_stage(b->sp->defense, b->stage[ST_DEF]); }
static int eff_sp_atk(const Battler *b)   { return apply_stage(b->sp->sp_atk,  b->stage[ST_SPA]); }
static int eff_sp_def(const Battler *b)   { return apply_stage(b->sp->sp_def,  b->stage[ST_SPD]); }

static int eff_speed(const Battler *b)
{
    int s = apply_stage(b->sp->speed, b->stage[ST_SPE]);
    return (b->status == STATUS_PARALYSIS) ? (s / 2 > 0 ? s / 2 : 1) : s;
}

static void battler_init(Battler *b, const Pokemon *sp)
{
    memset(b, 0, sizeof *b);
    b->sp = sp;
    /* HP is the one stat that is not used as a ratio -- see HP_SCALE. */
    b->max_hp = sp->hp * HP_SCALE;
    b->hp     = b->max_hp;
    b->status = STATUS_NONE;
    choose_moveset(sp, b->moves, &b->move_count);

    /*
     * Power points are what stops a fight going forever. Without them a
     * Pokemon carrying Recover or Moonlight heals half its bar every turn
     * indefinitely and simply cannot be beaten by anything that does less
     * than that per turn -- which made Toxic stalling a 98% strategy rather
     * than a strong one. The counts come straight from moves.csv.
     */
    for (int i = 0; i < b->move_count; i++) {
        int pp = move_table[b->moves[i]].pp;
        b->pp[i] = (pp > 0) ? pp : 5;
    }
}

/* ------------------------------------------------------------------ *
 * Choosing which four moves a species fights with
 * ------------------------------------------------------------------ */

static void choose_moveset_uncached(const Pokemon *p, int out[TEAM_MOVES],
                                    int *count)
{
    *count = 0;
    for (int i = 0; i < TEAM_MOVES; i++) {
        out[i] = -1;
    }

    /*
     * Walk the learn list backwards, so the last four moves it learns win --
     * a simple, uniform rule that needs no judgement about what is "good".
     * Duplicates and moves the engine cannot model are skipped.
     */
    for (int i = p->move_count - 1; i >= 0 && *count < TEAM_MOVES; i--) {
        int id = p->moves[i].id;
        if (id < 0) {
            continue;
        }
        if (special_kind(move_table[id].name) == SP_UNUSABLE) {
            continue;
        }
        /* A status move we do not model would just waste a turn. */
        if (move_table[id].category == CAT_STATUS &&
            status_effect(move_table[id].name) == NULL) {
            continue;
        }

        int already = 0;
        for (int j = 0; j < *count; j++) {
            if (out[j] == id) {
                already = 1;
            }
        }
        if (!already) {
            out[(*count)++] = id;
        }
    }

    /*
     * Some species have no modellable move at all (Metapod knows only Harden,
     * which we do model, but Ditto knows only Transform, which we do not).
     * Falling back to the raw last move keeps them in the tournament, where
     * they will simply lose or time out.
     */
    if (*count == 0 && p->move_count > 0 && p->moves[p->move_count - 1].id >= 0) {
        out[(*count)++] = p->moves[p->move_count - 1].id;
    }
}

void choose_moveset(const Pokemon *p, int out[TEAM_MOVES], int *count)
{
    if (tables_ready && p->dex >= 1 && p->dex <= MAX_POKEMON) {
        for (int i = 0; i < TEAM_MOVES; i++) {
            out[i] = cached_moves[p->dex][i];
        }
        *count = cached_count[p->dex];
        return;
    }
    choose_moveset_uncached(p, out, count);
}

const char *moveset_name(const Pokemon *p, int slot)
{
    int  moves[TEAM_MOVES], n = 0;
    choose_moveset(p, moves, &n);
    if (slot < 0 || slot >= n || moves[slot] < 0) {
        return "-";
    }
    return move_table[moves[slot]].name;
}

/* ------------------------------------------------------------------ *
 * Damage
 * ------------------------------------------------------------------ */

/* Power for the moves whose power is not a fixed number. */
static int computed_power(SpecialMove kind, const Battler *user,
                          const Battler *target)
{
    switch (kind) {
    case SP_FLAIL: {
        int ratio = user->hp * 48 / (user->max_hp > 0 ? user->max_hp : 1);
        if (ratio <= 1)  return 200;
        if (ratio <= 4)  return 150;
        if (ratio <= 9)  return 100;
        if (ratio <= 16) return 80;
        if (ratio <= 32) return 40;
        return 20;
    }
    case SP_ELECTRO_BALL: {
        int ts = eff_speed(target), us = eff_speed(user);
        if (ts <= 0) return 150;
        int r = us / ts;
        if (r >= 4) return 150;
        if (r == 3) return 120;
        if (r == 2) return 80;
        if (us * 2 > ts) return 60;
        return 40;
    }
    case SP_GYRO_BALL: {
        int us = eff_speed(user);
        int p = (us <= 0) ? 150 : 25 * eff_speed(target) / us + 1;
        return (p > 150) ? 150 : (p < 1 ? 1 : p);
    }
    case SP_WRING_OUT: {
        int p = 120 * target->hp / (target->max_hp > 0 ? target->max_hp : 1);
        return (p < 1) ? 1 : p;
    }
    case SP_LOW_KICK: {
        int w = target->sp->weight_hg;     /* hectograms */
        if (w >= 2000) return 120;
        if (w >= 1000) return 100;
        if (w >= 500)  return 80;
        if (w >= 250)  return 60;
        if (w >= 100)  return 40;
        return 20;
    }
    case SP_HEAVY_SLAM: {
        int uw = user->sp->weight_hg, tw = target->sp->weight_hg;
        if (tw <= 0) return 40;
        int r = uw / tw;
        if (r >= 5) return 120;
        if (r == 4) return 100;
        if (r == 3) return 80;
        if (r == 2) return 60;
        return 40;
    }
    case SP_BEAT_UP:
        /* One-on-one, so a single hit based on the user's own Attack. */
        return user->sp->attack / 10 + 5;
    case SP_PRESENT: {
        int roll = rng_below(10);
        if (roll < 4) return 40;
        if (roll < 7) return 80;
        if (roll < 8) return 120;
        return 0;                 /* the "heals the target" case */
    }
    default:
        return 0;
    }
}

/*
 * Damage for one hit. Returns the HP to remove; may be 0 for a move that
 * cannot hurt this target (an immunity, or a computed power of zero).
 * `crit` and `missed` report back for the statistics.
 */
static int compute_damage(const Battler *user, const Battler *target,
                          const MoveData *mv, SpecialMove kind,
                          double chart[TYPE_COUNT][TYPE_COUNT],
                          int *crit_out)
{
    *crit_out = 0;

    double type_mult = effectiveness(chart, mv->type, target->sp);

    /* Fixed-damage and HP-based moves ignore the formula, but not immunity. */
    switch (kind) {
    case SP_LEVEL_DAMAGE:
        return (type_mult == 0.0) ? 0 : BATTLE_LEVEL;
    case SP_HALVE_HP:
        return (type_mult == 0.0) ? 0 : (target->hp / 2 > 0 ? target->hp / 2 : 1);
    case SP_ENDEAVOR:
        if (type_mult == 0.0 || target->hp <= user->hp) return 0;
        return target->hp - user->hp;
    case SP_FINAL_GAMBIT:
        return (type_mult == 0.0) ? 0 : user->hp;
    case SP_COUNTER:
        return (target->last_damage_cat == CAT_PHYSICAL) ? user->last_damage * 2 : 0;
    case SP_MIRROR_COAT:
        return (target->last_damage_cat == CAT_SPECIAL) ? user->last_damage * 2 : 0;
    case SP_METAL_BURST:
        return user->last_damage * 3 / 2;
    case SP_OHKO:
        return (type_mult == 0.0) ? 0 : target->hp;
    default:
        break;
    }

    int power = (mv->power > 0) ? mv->power : computed_power(kind, user, target);
    if (power <= 0 || type_mult == 0.0) {
        return 0;
    }

    int attack, defense;
    if (mv->category == CAT_PHYSICAL) {
        attack  = eff_attack(user);
        defense = eff_defense(target);
    } else {
        attack  = eff_sp_atk(user);
        defense = eff_sp_def(target);
    }
    if (defense < 1) {
        defense = 1;
    }

    double base = ((2.0 * BATTLE_LEVEL / 5.0 + 2.0) * power * attack / defense)
                  / 50.0 + 2.0;

    if (mv->type == user->sp->type1 || mv->type == user->sp->type2) {
        base *= 1.5;                                  /* same-type bonus */
    }
    base *= type_mult;

    if (rng_below(24) == 0) {                         /* 1 in 24 crit */
        base *= 1.5;
        *crit_out = 1;
    }

    base *= (85 + rng_below(16)) / 100.0;             /* the damage roll */

    int damage = (int)base;
    return (damage < 1) ? 1 : damage;
}

/* ------------------------------------------------------------------ *
 * The heuristic move chooser
 * ------------------------------------------------------------------ */

/* Average damage, ignoring the roll and crits, weighted by accuracy. */
static int expected_damage(const Battler *user, const Battler *target,
                           const MoveData *mv, SpecialMove kind,
                           double chart[TYPE_COUNT][TYPE_COUNT])
{
    if (mv->category == CAT_STATUS) {
        return 0;
    }

    double type_mult = effectiveness(chart, mv->type, target->sp);
    if (type_mult == 0.0) {
        return 0;
    }

    int estimate;
    switch (kind) {
    case SP_LEVEL_DAMAGE: estimate = BATTLE_LEVEL;             break;
    case SP_HALVE_HP:     estimate = target->hp / 2;           break;
    case SP_OHKO:         estimate = target->hp;               break;
    case SP_FINAL_GAMBIT: estimate = user->hp;                 break;
    case SP_ENDEAVOR:
        estimate = (target->hp > user->hp) ? target->hp - user->hp : 0;
        break;
    case SP_COUNTER:
    case SP_MIRROR_COAT:
    case SP_METAL_BURST:
        estimate = user->last_damage;                          break;
    default: {
        int power = (mv->power > 0) ? mv->power
                                    : computed_power(kind, user, target);
        if (power <= 0) {
            return 0;
        }
        int attack  = (mv->category == CAT_PHYSICAL) ? eff_attack(user)
                                                     : eff_sp_atk(user);
        int defense = (mv->category == CAT_PHYSICAL) ? eff_defense(target)
                                                     : eff_sp_def(target);
        if (defense < 1) {
            defense = 1;
        }
        double base = ((2.0 * BATTLE_LEVEL / 5.0 + 2.0) * power * attack / defense)
                      / 50.0 + 2.0;
        if (mv->type == user->sp->type1 || mv->type == user->sp->type2) {
            base *= 1.5;
        }
        base *= type_mult * 0.925;      /* mid-point of the 85-100% roll */
        estimate = (int)base;
        break;
    }
    }

    int accuracy = (mv->accuracy > 0) ? mv->accuracy : 100;
    return estimate * accuracy / 100;
}

/*
 * Ricky chose a heuristic chooser rather than plain best-damage, so the rules
 * are, in order:
 *
 *   1. If a hit would finish the target, take it.
 *   2. Below 30% HP, heal if there is a healing move.
 *   3. On the first turn, set up if the move raises a stat and the fight
 *      looks like it will last long enough to be worth a turn.
 *   4. If the target has no status and we carry one that would stick, use it.
 *   5. Otherwise the best expected damage.
 */
static int choose_move(Battler *user, Battler *target, int turn,
                       double chart[TYPE_COUNT][TYPE_COUNT])
{
    int best_slot = -1, best_value = -1;
    int heal_slot = -1, setup_slot = -1, status_slot = -1;

    for (int i = 0; i < user->move_count; i++) {
        if (user->pp[i] <= 0) {
            continue;                       /* out of power points */
        }
        const MoveData *mv = &move_table[user->moves[i]];
        SpecialMove kind = move_special[user->moves[i]];

        if (mv->category == CAT_STATUS) {
            const StatusEffect *fx = move_effect[user->moves[i]];
            if (fx == NULL) {
                continue;
            }
            if (fx->heal_percent > 0 && heal_slot < 0) {
                heal_slot = i;
            }
            if (fx->inflict != STATUS_NONE && fx->heal_percent == 0 &&
                status_slot < 0) {
                status_slot = i;
            }
            int raises = 0;
            for (int s = 0; s < ST_COUNT; s++) {
                if (fx->self[s] > 0 || fx->foe[s] < 0) {
                    raises = 1;
                }
            }
            if (raises && setup_slot < 0) {
                setup_slot = i;
            }
            continue;
        }

        int value = expected_damage(user, target, mv, kind, chart);
        if (value > best_value) {
            best_value = value;
            best_slot  = i;
        }
    }

    /* 1. Finish it if we can. */
    if (best_slot >= 0 && best_value >= target->hp) {
        return best_slot;
    }

    /* 2. Heal when badly hurt, but not at full-ish health. */
    if (heal_slot >= 0 && user->hp * 100 < user->max_hp * 30) {
        return heal_slot;
    }

    /* 3. Set up once, early, if we are not about to be knocked out. */
    if (setup_slot >= 0 && !user->setup_used && turn <= 2 &&
        user->hp * 100 > user->max_hp * 60) {
        user->setup_used = 1;
        return setup_slot;
    }

    /* 4. Land a status condition if the target has none. */
    if (status_slot >= 0 && target->status == STATUS_NONE && rng_chance(70)) {
        return status_slot;
    }

    /* 5. Hit it. */
    if (best_slot >= 0) {
        return best_slot;
    }

    /*
     * Everything is out of PP (or there was nothing usable to begin with).
     * -1 means Struggle, handled by the caller.
     */
    return -1;
}

/* ------------------------------------------------------------------ *
 * One turn
 * ------------------------------------------------------------------ */

static void apply_status_effect(Battler *user, Battler *target,
                                const StatusEffect *fx)
{
    for (int s = 0; s < ST_COUNT; s++) {
        user->stage[s]   += fx->self[s];
        target->stage[s] += fx->foe[s];
        if (user->stage[s] > 6)    user->stage[s] = 6;
        if (user->stage[s] < -6)   user->stage[s] = -6;
        if (target->stage[s] > 6)  target->stage[s] = 6;
        if (target->stage[s] < -6) target->stage[s] = -6;
    }

    if (fx->heal_percent > 0) {
        user->hp += user->max_hp * fx->heal_percent / 100;
        if (user->hp > user->max_hp) {
            user->hp = user->max_hp;
        }
        /* Rest puts the user to sleep to heal completely. */
        if (fx->inflict == STATUS_SLEEP && fx->heal_percent == 100) {
            user->status      = STATUS_SLEEP;
            user->sleep_turns = 2;
            return;
        }
    }

    if (fx->inflict != STATUS_NONE && target->status == STATUS_NONE) {
        /* Types that shrug the condition off entirely. */
        int t1 = target->sp->type1, t2 = target->sp->type2;
        int poison_immune = (t1 == type_index("Poison") || t2 == type_index("Poison") ||
                             t1 == type_index("Steel")  || t2 == type_index("Steel"));
        int burn_immune   = (t1 == type_index("Fire")   || t2 == type_index("Fire"));
        int para_immune   = (t1 == type_index("Electric") || t2 == type_index("Electric"));

        if ((fx->inflict == STATUS_POISON || fx->inflict == STATUS_TOXIC) && poison_immune) {
            return;
        }
        if (fx->inflict == STATUS_BURN && burn_immune) {
            return;
        }
        if (fx->inflict == STATUS_PARALYSIS && para_immune) {
            return;
        }

        target->status = fx->inflict;
        if (fx->inflict == STATUS_SLEEP) {
            target->sleep_turns = 1 + rng_below(3);
        }
        if (fx->inflict == STATUS_TOXIC) {
            target->toxic_turns = 1;
        }
    }
}

/* Returns the damage dealt, and reports a crit or a miss. */
static int take_turn(Battler *user, Battler *target, int turn,
                     double chart[TYPE_COUNT][TYPE_COUNT],
                     int *slot_out, int *crit_out, int *miss_out)
{
    *slot_out = -1;
    *crit_out = 0;
    *miss_out = 0;

    /* Conditions that can stop the turn before it starts. */
    if (user->status == STATUS_SLEEP) {
        if (user->sleep_turns > 0) {
            user->sleep_turns--;
            return 0;
        }
        user->status = STATUS_NONE;
    }
    if (user->status == STATUS_FREEZE) {
        if (!rng_chance(20)) {
            return 0;
        }
        user->status = STATUS_NONE;
    }
    if (user->status == STATUS_PARALYSIS && rng_chance(25)) {
        return 0;
    }

    int slot = choose_move(user, target, turn, chart);

    /*
     * Out of PP entirely, so it Struggles: a typeless 50-power physical hit
     * that also costs the user a quarter of the damage dealt. That is what
     * ends a stalemate between two Pokemon that have run dry.
     */
    if (slot < 0) {
        int attack  = eff_attack(user);
        int defense = eff_defense(target);
        if (defense < 1) {
            defense = 1;
        }
        double base = ((2.0 * BATTLE_LEVEL / 5.0 + 2.0) * 50 * attack / defense)
                      / 50.0 + 2.0;
        base *= (85 + rng_below(16)) / 100.0;
        int damage = (int)base;
        if (damage < 1) {
            damage = 1;
        }
        if (damage > target->hp) {
            damage = target->hp;
        }
        target->hp -= damage;
        target->last_damage     = damage;
        target->last_damage_cat = CAT_PHYSICAL;

        int recoil = damage / 4;
        user->hp -= (recoil > 0) ? recoil : 1;
        if (user->hp < 0) {
            user->hp = 0;
        }
        return damage;
    }

    *slot_out = slot;
    user->pp[slot]--;

    const MoveData *mv = &move_table[user->moves[slot]];
    SpecialMove kind = move_special[user->moves[slot]];

    /* Accuracy. A blank accuracy in the file means the move never misses. */
    if (mv->accuracy > 0 && !rng_chance(mv->accuracy)) {
        *miss_out = 1;
        return 0;
    }

    if (mv->category == CAT_STATUS) {
        const StatusEffect *fx = move_effect[user->moves[slot]];
        if (fx != NULL) {
            apply_status_effect(user, target, fx);
        }
        return 0;
    }

    int damage = compute_damage(user, target, mv, kind, chart, crit_out);
    if (damage > target->hp) {
        damage = target->hp;
    }
    target->hp -= damage;

    target->last_damage     = damage;
    target->last_damage_cat = mv->category;

    /* Recoil and drain both key off the damage actually dealt. */
    const RecoilDrain *rd = move_rd[user->moves[slot]];
    if (rd != NULL && damage > 0) {
        if (rd->recoil_percent > 0) {
            int hurt = damage * rd->recoil_percent / 100;
            user->hp -= (hurt > 0) ? hurt : 1;
            if (user->hp < 0) {
                user->hp = 0;
            }
        }
        if (rd->drain_percent > 0) {
            user->hp += damage * rd->drain_percent / 100;
            if (user->hp > user->max_hp) {
                user->hp = user->max_hp;
            }
        }
    }

    if (kind == SP_FINAL_GAMBIT) {
        user->self_ko = 1;
    }
    return damage;
}

/* Poison, burn and toxic chip away at the end of each round. */
static void end_of_turn(Battler *b)
{
    if (b->hp <= 0) {
        return;
    }
    switch (b->status) {
    case STATUS_BURN:
    case STATUS_POISON:
        b->hp -= (b->max_hp / 8 > 0) ? b->max_hp / 8 : 1;
        break;
    case STATUS_TOXIC:
        b->hp -= (b->max_hp * b->toxic_turns / 16 > 0)
                 ? b->max_hp * b->toxic_turns / 16 : 1;
        b->toxic_turns++;
        break;
    default:
        break;
    }
    if (b->hp < 0) {
        b->hp = 0;
    }
}

/* ------------------------------------------------------------------ *
 * A battle
 * ------------------------------------------------------------------ */

BattleResult simulate_battle(const Pokemon *a, const Pokemon *b,
                             double chart[TYPE_COUNT][TYPE_COUNT],
                             SeriesStats *stats)
{
    Battler ba, bb;
    battler_init(&ba, a);
    battler_init(&bb, b);

    int turn = 0;
    for (turn = 1; turn <= TURN_CAP; turn++) {
        /* Speed decides who moves first; a tie is settled by a coin flip. */
        int a_first = (eff_speed(&ba) != eff_speed(&bb))
                      ? (eff_speed(&ba) > eff_speed(&bb))
                      : rng_below(2);

        Battler *first  = a_first ? &ba : &bb;
        Battler *second = a_first ? &bb : &ba;

        for (int half = 0; half < 2; half++) {
            Battler *user   = (half == 0) ? first : second;
            Battler *target = (half == 0) ? second : first;
            if (user->hp <= 0 || target->hp <= 0) {
                continue;
            }

            int slot, crit, miss;
            int damage = take_turn(user, target, turn, chart, &slot, &crit, &miss);

            if (stats != NULL) {
                int is_a = (user == &ba);
                if (is_a) {
                    stats->a_damage += damage;
                    stats->a_crits  += crit;
                    stats->a_misses += miss;
                    if (slot >= 0 && slot < TEAM_MOVES) stats->a_move_used[slot]++;
                } else {
                    stats->b_damage += damage;
                    stats->b_crits  += crit;
                    stats->b_misses += miss;
                    if (slot >= 0 && slot < TEAM_MOVES) stats->b_move_used[slot]++;
                }
            }

            if (user->self_ko) {
                user->hp = 0;
            }
        }

        end_of_turn(&ba);
        end_of_turn(&bb);

        if (ba.hp <= 0 || bb.hp <= 0) {
            break;
        }
    }
    if (turn > TURN_CAP) {
        turn = TURN_CAP;
    }

    BattleResult result;
    if (ba.hp <= 0 && bb.hp <= 0) {
        result = RESULT_DRAW;
    } else if (bb.hp <= 0) {
        result = RESULT_A_WINS;
    } else if (ba.hp <= 0) {
        result = RESULT_B_WINS;
    } else {
        /*
         * The turn cap was reached with both alive -- two Metapods hardening
         * at each other, or a pair that simply cannot hurt one another. The
         * one with more of its HP bar left takes it; dead level is a draw.
         */
        int a_share = ba.hp * 1000 / (ba.max_hp > 0 ? ba.max_hp : 1);
        int b_share = bb.hp * 1000 / (bb.max_hp > 0 ? bb.max_hp : 1);
        result = (a_share > b_share) ? RESULT_A_WINS
               : (b_share > a_share) ? RESULT_B_WINS
               : RESULT_DRAW;
    }

    if (stats != NULL) {
        stats->battles++;
        stats->total_turns += turn;
        if (stats->min_turns == 0 || turn < stats->min_turns) stats->min_turns = turn;
        if (turn > stats->max_turns) stats->max_turns = turn;
        if (result == RESULT_A_WINS) { stats->a_wins++; stats->a_hp_left += ba.hp; }
        else if (result == RESULT_B_WINS) { stats->b_wins++; stats->b_hp_left += bb.hp; }
        else { stats->draws++; }
    }

    return result;
}

void simulate_series(const Pokemon *a, const Pokemon *b,
                     double chart[TYPE_COUNT][TYPE_COUNT],
                     int runs, SeriesStats *out)
{
    memset(out, 0, sizeof *out);
    for (int i = 0; i < runs; i++) {
        simulate_battle(a, b, chart, out);
    }
}

/* ------------------------------------------------------------------ *
 * Round robin
 * ------------------------------------------------------------------ */

void run_tournament(const Pokemon *roster, int count,
                    double chart[TYPE_COUNT][TYPE_COUNT],
                    int runs_per_pair, RankEntry *out,
                    volatile int *cancel, ProgressFn progress, void *user_data)
{
    for (int i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].dex = roster[i].dex;
    }

    /*
     * Only the pairs, not the ordered pairs: A against B is the same fight as
     * B against A, so running both would double the work for nothing. Each
     * result is credited to both sides.
     */
    long long total_pairs = (long long)count * (count - 1) / 2;
    long long done        = 0;
    long long next_report = 0;

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (cancel != NULL && *cancel) {
                return;
            }

            SeriesStats s;
            simulate_series(&roster[i], &roster[j], chart, runs_per_pair, &s);

            out[i].wins    += s.a_wins;
            out[i].losses  += s.b_wins;
            out[i].draws   += s.draws;
            out[i].battles += s.battles;
            out[i].damage_dealt += s.a_damage;
            out[i].damage_taken += s.b_damage;
            out[i].turns   += s.total_turns;

            out[j].wins    += s.b_wins;
            out[j].losses  += s.a_wins;
            out[j].draws   += s.draws;
            out[j].battles += s.battles;
            out[j].damage_dealt += s.b_damage;
            out[j].damage_taken += s.a_damage;
            out[j].turns   += s.total_turns;

            done++;
            if (progress != NULL && done >= next_report) {
                progress((double)done / (double)total_pairs, user_data);
                next_report = done + total_pairs / 200 + 1;
            }
        }
    }

    if (progress != NULL) {
        progress(1.0, user_data);
    }
}
