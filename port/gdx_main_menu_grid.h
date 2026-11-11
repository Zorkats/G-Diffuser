/* port/gdx_main_menu_grid.h -- the main menu's 3x3 card grid (plan item O2-9). Pure C: no IO, no game state.
 * Copyright (c) Zorkats
 *
 * The stock main menu shows eight mode signs in a 4x2 grid. The port adds a ninth card, G-NET, so the cards
 * sit in a 3x3 grid at three quarters of their size, with G-NET in the middle:
 *
 *     GP RACE      TIME ATTACK   VS BATTLE
 *     PRACTICE     G-NET         DEATH RACE
 *     COURSE EDIT  OPTIONS       CREATE MACH.
 *
 * A card is named by its number, not by its place: 0 to 7 are the stock modes in ModeSelect order
 * (decomp/include/fzx_game.h) and 8 is G-NET. Every function here takes and returns card numbers, and the bits
 * of `available` are card numbers too.
 *
 * A card can be unavailable (Course Edit and Create Machine need the 64DD disk). The cursor skips it in the
 * direction it moves, as the stock menu does in its own grid.
 */
#ifndef GDX_MAIN_MENU_GRID_H
#define GDX_MAIN_MENU_GRID_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MAIN_MENU_GRID_COLUMNS 3
#define GDX_MAIN_MENU_GRID_CARDS 9
/* The ninth card's number, after the eight stock modes. It sits in the middle of the grid. */
#define GDX_MAIN_MENU_GNET_CARD 8
/* Every card available. */
#define GDX_MAIN_MENU_GRID_ALL ((1u << GDX_MAIN_MENU_GRID_CARDS) - 1u)

#define GDX_MAIN_MENU_GRID_LEFT 0
#define GDX_MAIN_MENU_GRID_RIGHT 1
#define GDX_MAIN_MENU_GRID_UP 2
#define GDX_MAIN_MENU_GRID_DOWN 3

/* Card placement, in 320x240 screen pixels: the top-left corner of the card drawn at three quarters of its
 * 64x86 size. The columns are centred on x 80, 160 and 240; the rows are 66 pixels apart under the header. */
#define GDX_MAIN_MENU_GRID_SCALE 0.75f
int gdx_main_menu_grid_left(int card);
int gdx_main_menu_grid_top(int card);

/* The card the cursor lands on when it moves one step in `direction` from `current`. available has bit (1 << card)
 * set for each card the player can pick. Left and right move along the row, skipping unavailable cards. Up and down
 * go to the next row with an available card, to the same column when that card is available, else to the nearest
 * available one, the left one on a tie. Returns current when nothing is reachable that way. */
int gdx_main_menu_grid_move(int current, int direction, unsigned available);

/* current, or the nearest available card in its row when it is unavailable (the left one on a tie), or the first
 * available card in reading order when its row has none. An out-of-range current is treated as unavailable.
 * Returns 0 when nothing is available. */
int gdx_main_menu_grid_settle(int current, unsigned available);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MAIN_MENU_GRID_H */
