# Copies the frozen Companion worker beside the Companion executable. Does nothing when the worker
# has not been frozen; tools/build_companion_worker.py creates it.
# Inputs: GDX_WORKER_SOURCE (build/companion-worker), GDX_WORKER_DEST (<exe dir>/worker).
# Copyright (c) Zorkats

if(EXISTS "${GDX_WORKER_SOURCE}/gdx_companion_worker" OR EXISTS "${GDX_WORKER_SOURCE}/gdx_companion_worker.exe")
    file(COPY "${GDX_WORKER_SOURCE}/" DESTINATION "${GDX_WORKER_DEST}")
    message(STATUS "Companion worker copied to ${GDX_WORKER_DEST}")
else()
    message(STATUS "Companion worker not frozen; skipping copy. Run: python -I tools/build_companion_worker.py")
endif()
