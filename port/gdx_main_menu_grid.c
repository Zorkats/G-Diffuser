/* port/gdx_main_menu_grid.c -- the main menu's 3x3 card grid. See gdx_main_menu_grid.h.
 * Copyright (c) Zorkats
 */
#include "gdx_main_menu_grid.h"

#define ROWS (GDX_MAIN_MENU_GRID_CARDS / GDX_MAIN_MENU_GRID_COLUMNS)
#define FIRST_LEFT 56
#define COLUMN_PITCH 80
#define FIRST_TOP 38
#define ROW_PITCH 66

/* The card in each place of the grid, row by row, and the place of each card. Cards 0 to 7 are the stock modes in
 * ModeSelect order (decomp/include/fzx_game.h) and card 8 is G-NET:
 *     GP RACE (0)       TIME ATTACK (1)   VS BATTLE (3)
 *     PRACTICE (5)      G-NET (8)         DEATH RACE (2)
 *     COURSE EDIT (4)   OPTIONS (6)       CREATE MACHINE (7) */
static const signed char sCardAtPlace[GDX_MAIN_MENU_GRID_CARDS] = { 0, 1, 3, 5, 8, 2, 4, 6, 7 };
static const signed char sPlaceOfCard[GDX_MAIN_MENU_GRID_CARDS] = { 0, 1, 5, 2, 6, 3, 7, 8, 4 };

static int card_ok(int card) {
    return card >= 0 && card < GDX_MAIN_MENU_GRID_CARDS;
}

static int card_available(int card, unsigned available) {
    return (available >> (unsigned) card) & 1u;
}

/* The card at a row and column when the player can pick it, or -1. */
static int available_at(int row, int column, unsigned available) {
    int card;

    if (row < 0 || row >= ROWS || column < 0 || column >= GDX_MAIN_MENU_GRID_COLUMNS) {
        return -1;
    }
    card = sCardAtPlace[row * GDX_MAIN_MENU_GRID_COLUMNS + column];
    return card_available(card, available) ? card : -1;
}

/* The available card of `row` nearest to `column`, the left one on a tie, or -1. */
static int nearest_in_row(int row, int column, unsigned available) {
    int distance;
    int card;

    for (distance = 0; distance < GDX_MAIN_MENU_GRID_COLUMNS; distance++) {
        card = available_at(row, column - distance, available);
        if (card >= 0) {
            return card;
        }
        card = available_at(row, column + distance, available);
        if (card >= 0) {
            return card;
        }
    }
    return -1;
}

int gdx_main_menu_grid_left(int card) {
    if (!card_ok(card)) {
        card = 0;
    }
    return FIRST_LEFT + (sPlaceOfCard[card] % GDX_MAIN_MENU_GRID_COLUMNS) * COLUMN_PITCH;
}

int gdx_main_menu_grid_top(int card) {
    if (!card_ok(card)) {
        card = 0;
    }
    return FIRST_TOP + (sPlaceOfCard[card] / GDX_MAIN_MENU_GRID_COLUMNS) * ROW_PITCH;
}

int gdx_main_menu_grid_move(int current, int direction, unsigned available) {
    int row;
    int column;
    int step;
    int found;

    if (!card_ok(current)) {
        return current;
    }
    row = sPlaceOfCard[current] / GDX_MAIN_MENU_GRID_COLUMNS;
    column = sPlaceOfCard[current] % GDX_MAIN_MENU_GRID_COLUMNS;
    switch (direction) {
        case GDX_MAIN_MENU_GRID_LEFT:
        case GDX_MAIN_MENU_GRID_RIGHT:
            step = (direction == GDX_MAIN_MENU_GRID_LEFT) ? -1 : 1;
            for (column += step; column >= 0 && column < GDX_MAIN_MENU_GRID_COLUMNS; column += step) {
                found = available_at(row, column, available);
                if (found >= 0) {
                    return found;
                }
            }
            return current;
        case GDX_MAIN_MENU_GRID_UP:
        case GDX_MAIN_MENU_GRID_DOWN:
            step = (direction == GDX_MAIN_MENU_GRID_UP) ? -1 : 1;
            for (row += step; row >= 0 && row < ROWS; row += step) {
                found = nearest_in_row(row, column, available);
                if (found >= 0) {
                    return found;
                }
            }
            return current;
        default:
            return current;
    }
}

int gdx_main_menu_grid_settle(int current, unsigned available) {
    int place;
    int found;

    if (card_ok(current)) {
        if (card_available(current, available)) {
            return current;
        }
        place = sPlaceOfCard[current];
        found = nearest_in_row(place / GDX_MAIN_MENU_GRID_COLUMNS, place % GDX_MAIN_MENU_GRID_COLUMNS, available);
        if (found >= 0) {
            return found;
        }
    }
    for (place = 0; place < GDX_MAIN_MENU_GRID_CARDS; place++) {
        if (card_available(sCardAtPlace[place], available)) {
            return sCardAtPlace[place];
        }
    }
    return 0;
}
