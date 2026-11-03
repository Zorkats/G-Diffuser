#ifndef GDX_COURSE_GADGET_BUDGET_H
#define GDX_COURSE_GADGET_BUDGET_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxGadgetDemand {
    int features;
    int decorations;
    int landmines;
    int jumps;
    int effects;
    int dashes;
} GdxGadgetDemand;

extern int gdx_course_gadget_error;
int Course_GdxCanTestDrive(int courseIndex);

int gdx_gadget_demand_add(GdxGadgetDemand* demand, const signed char parts[9], const GdxGadgetDemand* limit);
int gdx_gadget_terrain_vertices(int samples, int remaining);

#ifdef __cplusplus
}
#endif
#endif
