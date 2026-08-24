/**
 * @file fsspectatormode.h
 * @brief Local first-person spectator camera controls.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Firestorm Viewer Source Code
 * Copyright (C) 2026, Firestorm contributors.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1.
 * This library is distributed without any warranty; without even the implied
 * warranty of merchantability or fitness for a particular purpose.
 * See <https://www.gnu.org/licenses/> for the full license text.
 * $/LicenseInfo$
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
    static void resetAvatarMovementState();

    static EState sState;
    static LLVector3d sPositionGlobal;
    static F32 sYaw;
    static F32 sPitch;
    static F32 sRoll;
    static LLViewerRegion* sEntryRegion;
    static bool sMouseCaptured;
};

#endif // FS_FSSPECTATORMODE_H
