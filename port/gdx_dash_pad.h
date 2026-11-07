/* port/gdx_dash_pad.h -- Super and Brake dash pads.
 *
 * Course Edit stores a kind, a width and a Super pad's boost per control point (gdx_course_settings.h). This module
 * holds what the race does with them:
 *
 *   - Super: the dash's speed floor is multiplied by the pad's boost (1.1 to 3.0) and the boost lasts a little
 *     longer. The race's own speed caps still apply.
 *   - Brake: the first frame on the pad cuts the machine's speed to GDX_DASH_PAD_BRAKE_SPEED_SHARE of what it was,
 *     then the drive is held down for GDX_DASH_PAD_BRAKE_FRAMES. The heading never changes, so lap counting and
 *     the wrong-way check see a normal machine.
 *   - Width: a percent of the road, on flat road shapes. Pipes, cylinders and half-pipes keep the stock plate.
 *
 * The pad art is made at runtime from the stock dash texture in the player's own ROM: blue with a chevron for
 * Super, red with the chevron and the gradient turned backward for Brake. No pixel data is kept in the repo.
 */
#ifndef GDX_DASH_PAD_H
#define GDX_DASH_PAD_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_DASH_PAD_NORMAL 0
#define GDX_DASH_PAD_SUPER 1
#define GDX_DASH_PAD_BRAKE 2

#define GDX_DASH_PAD_RACERS 30
#define GDX_DASH_PAD_EFFECTS 192

#define GDX_DASH_PAD_BRAKE_FRAMES 45
#define GDX_DASH_PAD_BRAKE_SPEED_SHARE 0.55f
#define GDX_DASH_PAD_BRAKE_DRIVE 0.5f

/* The stock texture is 16x16 RGBA5551, big-endian, and the archive entry has a 0x50-byte header. */
#define GDX_DASH_PAD_TEX_SIDE 16
#define GDX_DASH_PAD_TEX_BYTES (GDX_DASH_PAD_TEX_SIDE * GDX_DASH_PAD_TEX_SIDE * 2)
#define GDX_DASH_PAD_ENTRY_PIXELS 0x50
#define GDX_DASH_PAD_ENTRY_BYTES (GDX_DASH_PAD_ENTRY_PIXELS + GDX_DASH_PAD_TEX_BYTES)

/* Builds the Super or Brake texture from the stock archive entry. Returns 0 when the entry is not a 16x16 image
 * or kind is not Super or Brake. */
int gdx_dash_pad_compose(const unsigned char* entry, unsigned long size, int kind, unsigned char* out);

/* The Super or Brake texture, composed on first use. NULL for a normal pad or when the stock texture could not
 * be read, so the caller draws the stock pad. */
const void* gdx_dash_pad_texture(int kind);

/* The stock texture as read from the archive, to put back in the dash tile after a Super or Brake pad. NULL
 * whenever gdx_dash_pad_texture would be. */
const void* gdx_dash_pad_stock_texture(void);

/* The boost of a Super pad when the player races on the menu's values (Ignore track settings). */
#define GDX_DASH_PAD_DEFAULT_BOOST 1.5f

/* Plate shape. stockOffset and stockHalf are the stock lateral centre and half width, roadWidth the full road
 * width where the pad starts. percent 0 keeps the stock plate. Otherwise the half width grows to percent of the
 * road's half width (never below stock) and the centre moves in so the plate stays on the road. */
void gdx_dash_pad_span(float stockOffset, float stockHalf, int percent, float roadWidth, float* offset, float* half);

/* The texture s span of a plate, in the same units as the stock 0x400. The stock pad shows its texture twice
 * across; a Super or Brake pad shows its chevron once. A wider plate repeats it instead of stretching it. */
int gdx_dash_pad_s_span(int kind, float half, float stockHalf);

/* The kind each built dash effect was made with, for the draw. */
void gdx_dash_pad_set_effect_kind(int effectIndex, int kind);
int gdx_dash_pad_effect_kind(int effectIndex);

/* Boost frames of a Super pad: a quarter of the extra boost is added to the stock length. */
int gdx_dash_pad_boost_frames(int stockFrames, float boost);

/* Per racer. reset clears a racer at race start. */
void gdx_dash_pad_racer_reset(int racerId);
void gdx_dash_pad_set_boost(int racerId, float boost);
float gdx_dash_pad_boost(int racerId);
/* A frame on a Brake pad. Returns 1 when this contact starts the brake, the frame to cut the speed. */
int gdx_dash_pad_brake_begin(int racerId);
/* Counts the brake down once per physics frame. Returns 1 while it holds the drive down. */
int gdx_dash_pad_brake_tick(int racerId);
/* What is left of the brake, 1.0 just after the pad down to 0.0. */
float gdx_dash_pad_brake_share(int racerId);

#ifdef __cplusplus
}
#endif

#endif
