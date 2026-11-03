#ifndef GDX_COURSE_JUNCTION_H
#define GDX_COURSE_JUNCTION_H

#define GDX_JUNCTION_MAX_PIECES 128
#define GDX_JUNCTION_MAX_CUTTERS 512
#define GDX_JUNCTION_DISTANCE 1.0
#define GDX_JUNCTION_PARALLEL_COS 0.98

typedef struct {
    double pos[3];
    double uv[2];
} GdxJunctionVertex;

typedef struct {
    GdxJunctionVertex v[3];
} GdxJunctionTriangle;

typedef struct {
    double plane[5][4];
    double min[3];
    double max[3];
    int railVolume;
} GdxJunctionCutter;

typedef struct {
    GdxJunctionTriangle pieces[2][GDX_JUNCTION_MAX_PIECES];
} GdxJunctionWorkspace;

int gdx_junction_cutter(const GdxJunctionTriangle* owner, GdxJunctionCutter* out);
int gdx_junction_rail_cutter(const GdxJunctionTriangle* road, double depth, GdxJunctionCutter* out);
short gdx_junction_quantize(double value);
// Negative means invalid input or capacity exhaustion; no partial result is published.
int gdx_junction_subtract(const GdxJunctionTriangle* input, const GdxJunctionCutter* cutters,
                          int cutterCount, GdxJunctionWorkspace* workspace,
                          GdxJunctionTriangle* output, int capacity);

#endif
