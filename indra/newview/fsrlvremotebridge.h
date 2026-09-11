/**
 * @file fsrlvremotebridge.h
 * @brief Viewer-side front end for an owner-worn RLV relay controller.
 */

#ifndef FS_RLV_REMOTE_BRIDGE_H
#define FS_RLV_REMOTE_BRIDGE_H

#include "llsd.h"
#include "lluuid.h"

#include <string>

class FSRLVRemoteBridge
{
public:
    static constexpr S32 CONTROL_CHANNEL = -70197742;

    static bool handleObjectMessage(const LLUUID& object_id, const LLUUID& owner_id,
                                    const std::string& message);
    static void discoverController();
    static void requestAccess(const LLUUID& avatar_id);
    static void requestFolder(const std::string& path);
    static void applyFolder(const std::string& path, const std::string& action);
    static void releaseTarget();

    static const LLUUID& getControllerID();
    static const LLUUID& getTargetID();
    static const std::string& getStatus();
    static const std::string& getCurrentPath();
    static const LLSD& getFolders();
    static bool controllerReady();
    static bool targetAuthorized();
    static std::string getControllerScript();

private:
    static void sendControllerCommand(const std::string& command);
    static void setStatus(const std::string& status);
};

#endif // FS_RLV_REMOTE_BRIDGE_H
