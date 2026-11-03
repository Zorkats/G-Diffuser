#pragma once
#include "gdx_course_snapshot.h"

namespace gdx::snapshot {
bool captureState(Generation&, const void* runtime);
bool restoreState(const Generation&, void* runtime);
bool validateState(const Generation&);
bool preflightState(const Generation&);
bool preflightStates(const std::vector<Generation>&);
Result readState(Backend&, Generation& identity, const Bytes& base);
Result loadState(Backend&, Generation& identity, const Bytes& base, void* runtime);
Result prepareSave(Backend&, Generation& identity, const void* runtime, const Bytes& previousBase);
}
