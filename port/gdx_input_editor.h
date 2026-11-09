#pragma once

#include <string>

#include "libultraship/window/gui/InputEditorWindow.h"

// The same action-first editor is embedded in Settings and used by the pop-out window.
class GdxInputEditor : public LUS::InputEditorWindow {
  public:
    using LUS::InputEditorWindow::InputEditorWindow;
    ~GdxInputEditor() override;

  protected:
    void DrawElement() override;
    void UpdateElement() override;

  private:
    void DrawActions(int context);
    void BeginCapture(int context, int output, int index);
    void EndCapture();
    void DrawCapture();

    int mPort = 0;
    int mCapturePort = 0;
    int mCaptureContext = 0;
    int mCaptureOutput = 0;
    int mCaptureIndex = -1;
    int mLastCaptureFrame = -1;
    char mProfileName[64] = "Default";
    char mProfileFile[512] = {};
    std::string mProfileStatus;
    bool mCapturing = false;
    bool mCaptureArmed = false;
    bool mAwaitingGamepadRelease = false;
    double mArmAfter = 0.0;
    double mUnblockAfter = 0.0;
};
