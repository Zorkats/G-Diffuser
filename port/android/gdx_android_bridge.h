/* port/android/gdx_android_bridge.h — the parts of the JNI bridge the rest of the port calls.
 *
 * Only the document picker crosses back this way. Everything else in AndroidBridge.cpp is
 * entered FROM Kotlin and needs no declaration here.
 */
#ifndef GDX_ANDROID_BRIDGE_H
#define GDX_ANDROID_BRIDGE_H

#ifdef __ANDROID__

#include <string>

namespace gdx::android {

/* Opens the system document picker and blocks until the user chooses or cancels.
 *
 * Returns the path of a copy in the app's cache that the caller can open by name, or an empty
 * string if the pick was cancelled, could not be started, or did not come back. The copy is why
 * this exists at all: a picked document is a content:// stream, and scoped storage leaves the
 * engine's own file browser able to see little beyond the app's own directory, so a ROM sitting
 * in Downloads or on Drive is otherwise unreachable.
 *
 * MUST NOT be called from the Android UI thread — it waits for that thread to do the work. The
 * engine only ever calls it from the render thread, which is a different one.
 */
std::string PickDocumentBlocking();

/* Opens the Android ROM-hack archive manager. This is the scoped-storage replacement for the
 * desktop ROM Hacks window's "Open folder" action. */
bool OpenHackManager();

} // namespace gdx::android

#endif /* __ANDROID__ */
#endif /* GDX_ANDROID_BRIDGE_H */
