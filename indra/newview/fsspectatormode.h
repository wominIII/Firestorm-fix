/**
 * @file fsspectatormode.h
 * @brief Local first-person spectator camera controls.
 */

#ifndef FS_FSSPECTATORMODE_H
#define FS_FSSPECTATORMODE_H

#include "v3dmath.h"

class LLViewerRegion;

class FSSpectatorMode
{
public:
    enum EState
    {
        STATE_INACTIVE,
        STATE_MOVING,
        STATE_HELD
    };

    static void toggle();
    static void toggleMouseMode();
    static void adjustMoveSpeed(S32 clicks);
    static void update();
    static void shutdown();

    static bool isCameraOverride() { return sState == STATE_MOVING; }
    static bool isEngaged() { return sState != STATE_INACTIVE; }
    static bool isMoving() { return sState == STATE_MOVING; }
    static EState getState() { return sState; }

private:
    static void enter();
    static void resumeMoving();
    static void hold();
    static void leave();
    static void applyCamera();
    static void setMouseCapture(bool captured);

    static EState sState;
    static LLVector3d sPositionGlobal;
    static F32 sYaw;
    static F32 sPitch;
    static F32 sRoll;
    static LLViewerRegion* sEntryRegion;
};

#endif // FS_FSSPECTATORMODE_H
