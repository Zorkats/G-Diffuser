#include "../gdx_course_gadget_budget.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const GdxGadgetDemand limit = { 120, 32, 48, 4, 192, 21 };
    GdxGadgetDemand demand = { 0 };
    GdxGadgetDemand before;
    signed char parts[9];
    int part;
    int value;
    static const int maximum[9] = { 11, 3, 4, 4, 3, 3, 3, 15, 5 };

    for (part = 0; part < 9; part++) {
        for (value = -1; value < maximum[part]; value++) {
            memset(parts, -1, sizeof(parts));
            parts[part] = (signed char) value;
            memset(&demand, 0, sizeof(demand));
            assert(gdx_gadget_demand_add(&demand, parts, &limit));
            assert(demand.features == demand.landmines + demand.jumps + demand.decorations);
        }
    }
    memset(parts, -1, sizeof(parts));
    memset(&demand, 0, sizeof(demand));
    parts[5] = 0;
    for (part = 0; part < 8; part++) {
        assert(gdx_gadget_demand_add(&demand, parts, &limit));
    }
    before = demand;
    assert(!gdx_gadget_demand_add(&demand, parts, &limit));
    assert(memcmp(&before, &demand, sizeof(demand)) == 0);
    parts[5] = 3;
    assert(!gdx_gadget_demand_add(&demand, parts, &limit));
    assert(memcmp(&before, &demand, sizeof(demand)) == 0);
    assert(gdx_gadget_terrain_vertices(2, 4) == 4);
    assert(gdx_gadget_terrain_vertices(16, 32) == 32);
    assert(gdx_gadget_terrain_vertices(17, 35) == -1);
    assert(gdx_gadget_terrain_vertices(17, 36) == 36);
    assert(gdx_gadget_terrain_vertices(1, 2048) == -1);
    assert(gdx_gadget_terrain_vertices(2049, 10000) == -1);
    puts("All gadget budget tests passed");
    return 0;
}
