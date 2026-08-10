/**
 * @file fsxtoysbridge.h
 * @brief Opt-in XToys private-webhook event bridge.
 */

#ifndef FS_XTOYS_BRIDGE_H
#define FS_XTOYS_BRIDGE_H

#include "v3dmath.h"
#include "llsd.h"

#include <string>

class FSXToysBridge
{
public:
    static void notifyMeanCollision(U8 type, F32 magnitude);
    static void notifyCollisionSound(const LLVector3d& position, F32 gain);
    static void sendTest();
    static void sendStop();

private:
    static void scheduleStop(F32 seconds);
    static void stopCoro(U64 generation, F32 seconds);
    static void sendEvent(const std::string& action, const LLSD& data);
    static void postCoro(std::string url, std::string token, LLSD body);
};

#endif // FS_XTOYS_BRIDGE_H
