/* port/gdx_net_rules_audit.h -- which settings a G-NET room locks, and why (leftover L1).
 * Copyright (c) Zorkats
 *
 * Every setting that racer, CPU or course code reads is in one table here, in one of five classes:
 *
 *   GRID        the host's value is every racer's value: it travels in the room's grid.
 *   HOST_CPU    only the host runs the CPUs, so the host's value is the room's. It travels in `room` for
 *               the next host once D28.1 adds that.
 *   PERSONAL    input feel. Each player keeps their own.
 *   LOCAL       what one copy shows or keeps. It changes nothing another copy sees.
 *   NOT_IN_ROOM a room never reaches the code that reads it, or the room overrides it.
 *
 * A GRID or HOST_CPU entry also says where the room carries it: a slice that already does, or the slice
 * that will. port/tests/gdx_net_rules_audit_tests.cpp scans the source for the names passed to CVarGet*
 * and fails on a gameplay name missing from this table, and on an entry the source no longer has. Names
 * the scan cannot see, because a macro, a variable or a table passes them, are marked BY_HAND.
 */

#ifndef GDX_NET_RULES_AUDIT_H
#define GDX_NET_RULES_AUDIT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GdxNetRuleClass {
    GDX_NET_RULE_GRID = 0,
    GDX_NET_RULE_HOST_CPU,
    GDX_NET_RULE_PERSONAL,
    GDX_NET_RULE_LOCAL,
    GDX_NET_RULE_NOT_IN_ROOM,
    GDX_NET_RULE_CLASS_COUNT
} GdxNetRuleClass;

typedef enum GdxNetRuleSeen {
    GDX_NET_RULE_SCANNED = 0, /* passed to CVarGet* as a string literal */
    GDX_NET_RULE_BY_HAND      /* passed through a macro, a variable or a table */
} GdxNetRuleSeen;

typedef struct GdxNetRuleEntry {
    const char* name;    /* the CVar */
    GdxNetRuleClass cls;
    GdxNetRuleSeen seen;
    const char* carried; /* GRID and HOST_CPU: the slice that carries it to the other copies, "O3" when
                          * the room's grid rules carry it today; NULL for the other classes */
    const char* why;     /* one line: what the value does and why it sits in this class */
} GdxNetRuleEntry;

/* The table, in a fixed order. */
int gdx_net_rules_audit_count(void);
const GdxNetRuleEntry* gdx_net_rules_audit_entry(int index);

/* The entry for a CVar name, or NULL. */
const GdxNetRuleEntry* gdx_net_rules_audit_find(const char* name);

/* 1 when a name is under a namespace whose settings change a race (Gameplay, Tuning, SingleRace,
 * Multiplayer, Practice, Hacks, CustomGrid), so the table must classify it wherever it is read. */
int gdx_net_rules_audit_is_gameplay_name(const char* name);

/* The class as a short word for messages: "grid", "host cpu", "personal", "local", "not in room". */
const char* gdx_net_rules_audit_class_name(GdxNetRuleClass cls);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_RULES_AUDIT_H */
