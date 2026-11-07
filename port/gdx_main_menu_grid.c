/* port/gdx_main_menu_grid.c -- the main menu's 3x3 card grid. See gdx_main_menu_grid.h.
 * Copyright (c) Zorkats
 */
#include "gdx_main_menu_grid.h"

#define ROWS (GDX_MAIN_MENU_GRID_CARDS / GDX_MAIN_MENU_GRID_COLUMNS)
#define FIRST_LEFT 56
#define COLUMN_PITCH 80
#define FIRST_TOP 38
#define ROW_PITCH 66

static int card_ok(int card) {
    return card >= 0 && card < GDX_MAIN_MENU_GRID_CARDS;
}

static int available_at(int row, int column, unsigned available) {
    if (row < 0 || row >= ROWS || column < 0 || column >= GDX_MAIN_MENU_GRID_COLUMNS) {
        return 0;
    }
    return (available >> (unsigned) (row * GDX_MAIN_MENU_GRID_COLUMNS + column)) & 1u;
}

/* The available card of `row` nearest to `column`, the left one on a tie, or -1. */
static int nearest_in_row(int row, int column, unsigned available) {
    int distance;

    for (distance = 0; distance < GDX_MAIN_MENU_GRID_COLUMNS; distance++) {
        if (available_at(row, column - distance, available)) {
            return row * GDX_MAIN_MENU_GRID_COLUMNS + column - distance;
        }
        if (available_at(row, column + distance, available)) {
            return row * GDX_MAIN_MENU_GRID_COLUMNS + column + distance;
        }
    }
    return -1;
}

int gdx_main_menu_grid_left(int card) {
    if (!card_ok(card)) {
        card = 0;
    }
    return FIRST_LEFT + (card % GDX_MAIN_MENU_GRID_COLUMNS) * COLUMN_PITCH;
}

int gdx_main_menu_grid_top(int card) {
    if (!card_ok(card)) {
        card = 0;
    }
    return FIRST_TOP + (card / GDX_MAIN_MENU_GRID_COLUMNS) * ROW_PITCH;
}

int gdx_main_menu_grid_move(int current, int direction, unsigned available) {
    int row;
    int column;
    int step;
    int found;

    if (!card_ok(current)) {
        return current;
    }
    row = current / GDX_MAIN_MENU_GRID_COLUMNS;
    column = current % GDX_MAIN_MENU_GRID_COLUMNS;
    switch (direction) {
        case GDX_MAIN_MENU_GRID_LEFT:
        case GDX_MAIN_MENU_GRID_RIGHT:
            step = (direction == GDX_MAIN_MENU_GRID_LEFT) ? -1 : 1;
            for (column += step; column >= 0 && column < GDX_MAIN_MENU_GRID_COLUMNS; column += step) {
                if (available_at(row, column, available)) {
                    return row * GDX_MAIN_MENU_GRID_COLUMNS + column;
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
    int card;
    int found;

    if (card_ok(current)) {
        if ((available >> (unsigned) current) & 1u) {
            return current;
        }
        found = nearest_in_row(current / GDX_MAIN_MENU_GRID_COLUMNS, current % GDX_MAIN_MENU_GRID_COLUMNS, available);
        if (found >= 0) {
            return found;
        }
    }
    for (card = 0; card < GDX_MAIN_MENU_GRID_CARDS; card++) {
        if ((available >> (unsigned) card) & 1u) {
            return card;
        }
    }
    return 0;
}
