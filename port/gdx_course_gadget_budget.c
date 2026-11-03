#include "gdx_course_gadget_budget.h"

int gdx_course_gadget_error;

int gdx_gadget_demand_add(GdxGadgetDemand* demand, const signed char parts[9], const GdxGadgetDemand* limit) {
    static const signed char maximum[9] = { 11, 3, 4, 4, 3, 3, 3, 15, 5 };
    GdxGadgetDemand next;
    int i;

    if (!demand || !parts || !limit) {
        return 0;
    }
    next = *demand;
    for (i = 0; i < 9; i++) {
        if (parts[i] < -1 || parts[i] >= maximum[i]) {
            return 0;
        }
    }
    next.landmines += parts[5] >= 0 ? 6 : 0;
    next.jumps += parts[4] >= 0 ? 1 : 0;
    next.decorations += parts[6] >= 0 ? 1 : 0;
    next.decorations += parts[7] >= 0 ? (parts[7] % 3 == 0 ? 2 : 1) : 0;
    next.decorations += parts[8] >= 0 ? (parts[8] == 4 ? 1 : 2) : 0;
    next.features = next.landmines + next.jumps + next.decorations;
    next.effects += parts[0] >= 0 ? (parts[0] == 0 || parts[0] == 4 ? 2 : 1) : 0;
    next.effects += parts[2] >= 0 ? (parts[2] == 0 ? 2 : 1) : 0;
    next.effects += parts[3] >= 0 ? (parts[3] == 0 ? 2 : 1) : 0;
    next.dashes += parts[1] >= 0 ? 1 : 0;
    next.effects += parts[1] >= 0 ? 1 : 0;
    if (next.features > limit->features || next.decorations > limit->decorations ||
        next.landmines > limit->landmines || next.jumps > limit->jumps ||
        next.effects > limit->effects || next.dashes > limit->dashes) {
        return 0;
    }
    *demand = next;
    return 1;
}

int gdx_gadget_terrain_vertices(int samples, int remaining) {
    int vertices;

    if (samples < 2 || samples > 2048 || remaining < 0) {
        return -1;
    }
    // Each 32-vertex batch repeats its last pair at the next batch boundary.
    vertices = (samples + (samples - 2) / 15) * 2;
    return vertices <= remaining ? vertices : -1;
}
