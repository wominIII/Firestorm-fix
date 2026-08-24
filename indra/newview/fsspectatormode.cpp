/**
 * @file fsspectatormode.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsspectatormode.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llfocusmgr.h"
#include "llfloatercamera.h"
#include "llkeyboard.h"
#include "llmath.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <cmath>

FSSpectatorMode::EState FSSpectatorMode::sState = FSSpectatorMode::STATE_INACTIVE;
LLVector3d FSSpectatorMode::sPositionGlobal;
F32 FSSpectatorMode::sYaw = 0.f;
F32 FSSpectatorMode::sPitch = 0.f;
F32 FSSpectatorMode::sRoll = 0.f;
LLViewerRegion* FSSpectatorMode::sEntryRegion = nullptr;
bool FSSpectatorMode::sMouseCaptured = false;

void FSSpectatorMode::toggle()
{
    if (sState == STATE_INACTIVE)
    {
        enter();
    }
    else
    {
        leave();
    }
}

void FSSpectatorMode::toggleMouseMode()
{
    if (sState == STATE_MOVING)
    {
        hold();
    }
    else if (sState == STATE_HELD)
    {
        resumeMoving();
    }
}

void FSSpectatorMode::adjustMoveSpeed(S32 clicks)
{
    if (sState != STATE_MOVING || clicks == 0)
    {
        return;
    }

    const F32 current_speed = gSavedSettings.getF32("FSSpectatorMoveSpeed");
    const F32 adjusted_speed = current_speed * std::pow(1.25f, static_cast<F32>(clicks));
    gSavedSettings.setF32("FSSpectatorMoveSpeed", llclamp(adjusted_speed, 0.5f, 200.f));
}

void FSSpectatorMode::enter()
{
    if (!gAgent.isInitialized() || !gViewerWindow || !gAgent.getRegion())
    {
        return;
    }

    if (LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        LLViewerJoystick::getInstance()->setOverrideCamera(false);
    }

    // Spectator movement reads the physical keys directly. Clear any avatar
    // control bits which were already set before those normal movement
    // callbacks start being consumed by spectator mode.
    resetAvatarMovementState();

    const LLVector3 at_axis = LLViewerCamera::getInstance()->getAtAxis();
    sYaw = std::atan2(at_axis.mV[VY], at_axis.mV[VX]);
    sPitch = std::asin(llclamp(at_axis.mV[VZ], -1.f, 1.f));
    sRoll = gAgentCamera.getRollAngle();
    sPositionGlobal = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    sEntryRegion = gAgent.getRegion();
    sState = STATE_MOVING;
    gSavedSettings.setS32("FSSpectatorModeState", static_cast<S32>(sState));

    gFocusMgr.setKeyboardFocus(nullptr);
    setMouseCapture(gFocusMgr.getAppHasFocus());
    sMouseCaptured = gFocusMgr.getAppHasFocus();
    applyCamera();
}

void FSSpectatorMode::hold()
{
    resetAvatarMovementState();
    const F32 cos_pitch = std::cos(sPitch);
    const LLVector3 forward(cos_pitch * std::cos(sYaw), cos_pitch * std::sin(sYaw), std::sin(sPitch));
    const LLVector3d focus_global = sPositionGlobal + LLVector3d(forward * 4.f);

    sState = STATE_HELD;
    gSavedSettings.setS32("FSSpectatorModeState", static_cast<S32>(sState));
    setMouseCapture(false);
    sMouseCaptured = false;

    // Hand the exact spectator viewpoint to Firestorm's native free camera.
    gAgentCamera.changeCameraToThirdPerson(false);
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(sPositionGlobal, focus_global, LLUUID::null);
    gAgentCamera.setRollAngle(sRoll);
    gAgentCamera.updateCamera();
}

void FSSpectatorMode::resumeMoving()
{
    const LLVector3 at_axis = LLViewerCamera::getInstance()->getAtAxis();
    sYaw = std::atan2(at_axis.mV[VY], at_axis.mV[VX]);
    sPitch = std::asin(llclamp(at_axis.mV[VZ], -1.f, 1.f));
    sRoll = gAgentCamera.getRollAngle();
    sPositionGlobal = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    sState = STATE_MOVING;
    gSavedSettings.setS32("FSSpectatorModeState", static_cast<S32>(sState));
    gFocusMgr.setKeyboardFocus(nullptr);
    setMouseCapture(gFocusMgr.getAppHasFocus());
    sMouseCaptured = gFocusMgr.getAppHasFocus();
    applyCamera();
}

void FSSpectatorMode::leave()
{
    setMouseCapture(false);
    sMouseCaptured = false;
    sState = STATE_INACTIVE;
    sEntryRegion = nullptr;
    gSavedSettings.setS32("FSSpectatorModeState", static_cast<S32>(sState));

    if (gAgent.isInitialized())
    {
        // Leaving from the held state can otherwise leave Firestorm's camera
        // floater in free-camera mode, where WASD continues to control the
        // camera instead of the avatar.
        LLFloaterCamera::resetCameraMode();
        gAgentCamera.changeCameraToThirdPerson(false);
        // Restore the follow camera without deriving a new avatar orientation
        // from the private spectator viewpoint.
        gAgentCamera.setFocusOnAvatar(true, false, false);
        gAgentCamera.resetCameraRoll();
        resetAvatarMovementState();
        gAgentCamera.updateCamera();
    }
}

void FSSpectatorMode::shutdown()
{
    if (sState != STATE_INACTIVE)
    {
        leave();
    }
}

void FSSpectatorMode::update()
{
    if (sState == STATE_INACTIVE)
    {
        return;
    }

    if (!gAgent.isInitialized() || !gAgent.getRegion() || gAgent.getRegion() != sEntryRegion)
    {
        leave();
        return;
    }

    if (sState == STATE_MOVING)
    {
        const bool app_has_focus = gFocusMgr.getAppHasFocus();
        if (!app_has_focus)
        {
            if (sMouseCaptured)
            {
                setMouseCapture(false);
                sMouseCaptured = false;
            }
            // Keep the spectator camera position, but never consume stale
            // mouse/keyboard input or recenter the OS cursor while another
            // application owns focus.
            applyCamera();
            return;
        }
        if (!sMouseCaptured)
        {
            setMouseCapture(true);
            sMouseCaptured = true;
        }

        const F32 nominal_sensitivity = 0.0025f;
        const F32 sensitivity = clamp_rescale(
            gSavedSettings.getF32("MouseSensitivity"), 0.f, 15.f, 0.5f, 2.75f) * nominal_sensitivity;
        const S32 dx = -gViewerWindow->getCurrentMouseDX();
        const S32 dy = -gViewerWindow->getCurrentMouseDY();

        sYaw += sensitivity * static_cast<F32>(dx);
        const F32 pitch_delta = sensitivity * static_cast<F32>(dy)
            * (gSavedSettings.getBOOL("InvertMouse") ? 1.f : -1.f);
        sPitch = llclamp(sPitch + pitch_delta, -F_PI_BY_TWO + 0.01f, F_PI_BY_TWO - 0.01f);

        const F32 cos_pitch = std::cos(sPitch);
        const LLVector3 forward(cos_pitch * std::cos(sYaw), cos_pitch * std::sin(sYaw), std::sin(sPitch));
        const LLVector3 left(-std::sin(sYaw), std::cos(sYaw), 0.f);
        LLVector3 movement;

        if (!gFocusMgr.getKeyboardFocus())
        {
            if (gKeyboard->getKeyDown('W')) movement += forward;
            if (gKeyboard->getKeyDown('S')) movement -= forward;
            if (gKeyboard->getKeyDown('A')) movement += left;
            if (gKeyboard->getKeyDown('D')) movement -= left;
            if (gKeyboard->getKeyDown(' ')) movement.mV[VZ] += 1.f;
            if (gKeyboard->getKeyDown(KEY_SHIFT)) movement.mV[VZ] -= 1.f;

            const F32 roll_speed = gSavedSettings.getF32("FSSpectatorRollSpeed") * DEG_TO_RAD;
            const F32 elapsed = llclamp(gFrameIntervalSeconds.value(), 0.f, 0.1f);
            if (gKeyboard->getKeyDown('Q')) sRoll -= roll_speed * elapsed;
            if (gKeyboard->getKeyDown('E')) sRoll += roll_speed * elapsed;
            if (sRoll > F_PI) sRoll -= F_TWO_PI;
            if (sRoll < -F_PI) sRoll += F_TWO_PI;
        }

        if (movement.normalize() > 0.f)
        {
            const F32 elapsed = llclamp(gFrameIntervalSeconds.value(), 0.f, 0.1f);
            movement *= gSavedSettings.getF32("FSSpectatorMoveSpeed") * elapsed;
            sPositionGlobal += LLVector3d(movement);
        }

        gViewerWindow->moveCursorToCenter();
        gViewerWindow->hideCursor();
    }
    else if (!gFocusMgr.getKeyboardFocus())
    {
        if (gKeyboard->getKeyDown('Q')) gAgentCamera.setRollLeftKey(1.f);
        if (gKeyboard->getKeyDown('E')) gAgentCamera.setRollRightKey(1.f);
    }

    if (sState == STATE_MOVING)
    {
        applyCamera();
    }
}

void FSSpectatorMode::applyCamera()
{
    const F32 cos_pitch = std::cos(sPitch);
    const LLVector3 forward(cos_pitch * std::cos(sYaw), cos_pitch * std::sin(sYaw), std::sin(sPitch));
    const LLVector3 level_left(-std::sin(sYaw), std::cos(sYaw), 0.f);
    const LLVector3 level_up = forward % level_left;
    const F32 cos_roll = std::cos(sRoll);
    const F32 sin_roll = std::sin(sRoll);
    const LLVector3 left = level_left * cos_roll + level_up * sin_roll;
    const LLVector3 up = level_up * cos_roll - level_left * sin_roll;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    camera->setOrigin(gAgent.getPosAgentFromGlobal(sPositionGlobal));
    camera->mXAxis = forward;
    camera->mYAxis = left;
    camera->mZAxis = up;
}

void FSSpectatorMode::setMouseCapture(bool captured)
{
    if (!gViewerWindow)
    {
        return;
    }

    if (captured)
    {
        gViewerWindow->moveCursorToCenter();
        gViewerWindow->hideCursor();
        gViewerWindow->getWindow()->setMouseClipping(true);
    }
    else
    {
        gViewerWindow->getWindow()->setMouseClipping(false);
        gViewerWindow->showCursor();
    }
}

void FSSpectatorMode::resetAvatarMovementState()
{
    if (!gAgent.isInitialized())
    {
        return;
    }

    // Movement key-up callbacks are intentionally consumed while spectator
    // mode is engaged, so explicitly clear both simulator control flags and
    // the camera-side key magnitudes at every state boundary.
    gAgent.resetControlFlags();
    gAgent.clearTempRun();
    gAgentCamera.setAtKey(0);
    gAgentCamera.setWalkKey(0);
    gAgentCamera.setLeftKey(0);
    gAgentCamera.setUpKey(0);
    gAgentCamera.setYawKey(0.f);
    gAgentCamera.setPitchKey(0.f);
    gAgentCamera.setRollLeftKey(0.f);
    gAgentCamera.setRollRightKey(0.f);
}
