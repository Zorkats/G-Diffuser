/* Unit tests for port/gdx_audio_lle_stage.h, the LLE staging window reuse rule (issue #33). */
#include "../gdx_audio_lle_stage.h"

#include <stdio.h>

enum { XF_CMD = 0, XF_S16 = 1, XF_BYTESTREAM = 2 };

static int sFailures;
static int sChecks;

static void Check(int condition, const char* what) {
    sChecks++;
    if (!condition) {
        sFailures++;
        printf("FAIL: %s\n", what);
    }
}

int main(void) {
    /* The #33 case: a second voice wraps its loop in the same tick and asks for more frames of the same
       compressed chunk. It must get its own window. */
    Check(!gdx_lle_stage_can_reuse(32u, XF_BYTESTREAM, 0, 48u, XF_BYTESTREAM, 0),
          "larger compressed read of the same host pointer gets its own window");
    Check(!gdx_lle_stage_can_reuse(64u, XF_BYTESTREAM, 0, 144u, XF_BYTESTREAM, 0),
          "KO star sizes 64 then 144 do not share");

    /* Equal or smaller reads of the same bytes keep sharing, which saves a copy. */
    Check(gdx_lle_stage_can_reuse(48u, XF_BYTESTREAM, 0, 48u, XF_BYTESTREAM, 0), "equal read shares");
    Check(gdx_lle_stage_can_reuse(144u, XF_BYTESTREAM, 0, 64u, XF_BYTESTREAM, 0), "smaller read shares");

    /* The same bytes under a different endianness transform are different data. */
    Check(!gdx_lle_stage_can_reuse(64u, XF_S16, 0, 64u, XF_BYTESTREAM, 0), "transform mismatch does not share");

    /* Buffers the RSP writes keep the old one-window-per-host rule, so load-then-save round trips and
       persistent state stay coherent. */
    Check(gdx_lle_stage_can_reuse(32u, XF_S16, 0, 64u, XF_S16, 1), "output request reuses the loaded window");
    Check(gdx_lle_stage_can_reuse(32u, XF_S16, 1, 64u, XF_S16, 0), "input after an output window reuses it");
    Check(gdx_lle_stage_can_reuse(32u, XF_S16, 0, 32u, XF_BYTESTREAM, 1), "persistent slot keeps its window");

    printf("gdx_audio_lle_stage_tests: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
