/**
 * @file fsrlvremotebridge.cpp
 * @brief Viewer-side front end for an owner-worn RLV relay controller.
 */

#include "llviewerprecompiledheaders.h"

#include "fsrlvremotebridge.h"
#include "fsfloaterrlvremotebrowser.h"

#include "llagent.h"
#include "llbase64.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerchat.h"

#include <boost/algorithm/string.hpp>

extern void really_send_chat_from_viewer(const std::string& utf8_out_text, EChatType type, S32 channel);

namespace
{
LLUUID sControllerID;
LLUUID sTargetID;
std::string sStatus("controller_missing");
LLSD sFolders = LLSD::emptyArray();
std::string sCurrentPath;

std::string encodeField(const std::string& value)
{
    return LLBase64::encode(reinterpret_cast<const U8*>(value.data()), value.size());
}

std::string decodeField(const std::string& value)
{
    return LLBase64::decodeAsString(value);
}

std::string joinFolderPath(const std::string& parent, const std::string& child)
{
    return parent.empty() ? child : parent + "/" + child;
}
}

bool FSRLVRemoteBridge::handleObjectMessage(const LLUUID& object_id, const LLUUID& owner_id,
                                            const std::string& message)
{
    if (message.rfind("FSRLVB|", 0) != 0) return false;

    // CHAT_TYPE_OWNER from an attachment can identify a child prim. Validate
    // the linkset root instead of requiring the emitting prim itself to carry
    // the attachment flag.
    if (owner_id != gAgentID)
        return false;
    LLViewerObject* source = gObjectList.findObject(object_id);
    LLViewerObject* controller = source ? source->getRootEdit() : nullptr;
    if (!controller || controller->isDead() || !controller->isAttachment() || !controller->permYouOwner())
        return false;

    std::vector<std::string> fields;
    boost::split(fields, message, boost::is_any_of("|"), boost::token_compress_off);
    if (fields.size() < 2) return true;

    const std::string& type = fields[1];
    if (type == "HELLO")
    {
        if (fields.size() >= 3 && LLUUID(fields[2]) == object_id)
        {
            sControllerID = object_id;
            setStatus("controller_ready");
        }
        return true;
    }
    if (object_id != sControllerID) return true;

    if (type == "AUTHORIZED" && fields.size() >= 3)
    {
        const LLUUID target(fields[2]);
        if (target == sTargetID)
        {
            setStatus("authorized");
            requestFolder(std::string());
        }
    }
    else if (type == "DENIED" && fields.size() >= 3 && LLUUID(fields[2]) == sTargetID)
    {
        sFolders = LLSD::emptyArray();
        sCurrentPath.clear();
        setStatus("denied");
    }
    else if (type == "PENDING" && fields.size() >= 3 && LLUUID(fields[2]) == sTargetID)
    {
        setStatus("pending");
    }
    else if (type == "FOLDERS" && fields.size() >= 5 && LLUUID(fields[2]) == sTargetID)
    {
        const std::string parent_path = decodeField(fields[3]);
        const std::string response = decodeField(fields[4]);
        LLSD folders = LLSD::emptyArray();
        std::vector<std::string> entries;
        boost::split(entries, response, boost::is_any_of(","), boost::token_compress_off);
        for (const std::string& raw_entry : entries)
        {
            const std::string::size_type marker = raw_entry.rfind('|');
            if (marker == std::string::npos || marker == 0) continue;
            const std::string name = raw_entry.substr(0, marker);
            const std::string worn = raw_entry.substr(marker + 1);
            if (name.empty()) continue;
            LLSD folder;
            folder["name"] = name;
            folder["path"] = joinFolderPath(parent_path, name);
            folder["worn"] = worn;
            folders.append(folder);
        }
        sFolders = folders;
        sCurrentPath = parent_path;
        setStatus("authorized");
    }
    else if (type == "ACTION" && fields.size() >= 4 && LLUUID(fields[2]) == sTargetID)
    {
        setStatus(fields[3] == "ok" ? "authorized" : "action_denied");
    }
    else if (type == "RELEASED")
    {
        sTargetID.setNull();
        sFolders = LLSD::emptyArray();
        sCurrentPath.clear();
        setStatus("controller_ready");
    }
    else if (type == "ERROR")
    {
        setStatus(fields.size() >= 3 ? "error:" + decodeField(fields[2]) : "error");
    }
    return true;
}

void FSRLVRemoteBridge::discoverController()
{
    really_send_chat_from_viewer("FSRLVC|*|DISCOVER", CHAT_TYPE_NORMAL, CONTROL_CHANNEL);
}

void FSRLVRemoteBridge::requestAccess(const LLUUID& avatar_id)
{
    if (!controllerReady() || avatar_id.isNull() || avatar_id == gAgentID) return;
    sTargetID = avatar_id;
    sFolders = LLSD::emptyArray();
    sCurrentPath.clear();
    setStatus("pending");
    sendControllerCommand("PROBE|" + avatar_id.asString());
}

void FSRLVRemoteBridge::requestFolder(const std::string& path)
{
    if (!controllerReady() || sTargetID.isNull()) return;
    setStatus("pending");
    sendControllerCommand("LIST|" + sTargetID.asString() + "|" + encodeField(path));
}

void FSRLVRemoteBridge::applyFolder(const std::string& path, const std::string& action)
{
    if (!targetAuthorized() || path.empty()) return;
    if (action != "attach" && action != "attachall" && action != "detach" && action != "detachall") return;
    setStatus("pending");
    sendControllerCommand("ACTION|" + sTargetID.asString() + "|" + action + "|" + encodeField(path));
}

void FSRLVRemoteBridge::releaseTarget()
{
    if (!controllerReady() || sTargetID.isNull()) return;
    sendControllerCommand("RELEASE|" + sTargetID.asString());
    sTargetID.setNull();
    sFolders = LLSD::emptyArray();
    sCurrentPath.clear();
    setStatus("controller_ready");
}

const LLUUID& FSRLVRemoteBridge::getControllerID() { return sControllerID; }
const LLUUID& FSRLVRemoteBridge::getTargetID() { return sTargetID; }
const std::string& FSRLVRemoteBridge::getStatus() { return sStatus; }
const std::string& FSRLVRemoteBridge::getCurrentPath() { return sCurrentPath; }
const LLSD& FSRLVRemoteBridge::getFolders() { return sFolders; }
bool FSRLVRemoteBridge::controllerReady()
{
    LLViewerObject* source = sControllerID.notNull() ? gObjectList.findObject(sControllerID) : nullptr;
    LLViewerObject* controller = source ? source->getRootEdit() : nullptr;
    if (!controller || controller->isDead() || !controller->isAttachment() || !controller->permYouOwner())
    {
        sControllerID.setNull();
        return false;
    }
    return true;
}
bool FSRLVRemoteBridge::targetAuthorized() { return sTargetID.notNull() && sStatus == "authorized"; }

void FSRLVRemoteBridge::sendControllerCommand(const std::string& command)
{
    if (sControllerID.isNull()) return;
    really_send_chat_from_viewer("FSRLVC|" + sControllerID.asString() + "|" + command,
                                 CHAT_TYPE_NORMAL, CONTROL_CHANNEL);
}

void FSRLVRemoteBridge::setStatus(const std::string& status)
{
    sStatus = status;
    FSFloaterRLVRemoteBrowser::refreshIfOpen();
}

std::string FSRLVRemoteBridge::getControllerScript()
{
    return R"LSL(// Firestorm Remote RLV Browser Controller v1.0
// Put this script into a modifiable object, save it, then wear the object.
// It never bypasses the target's RLV Relay permission mode.

integer CONTROL_CHANNEL = -70197742;
integer RELAY_CHANNEL = -1812221819;
integer replyChannel;
integer controlListen;
integer relayListen;
integer replyListen;
key owner;
key target;
string session;
string pendingPath;

viewer(string message) { llOwnerSay("FSRLVB|" + message); }
relay(string command)
{
    if (target == NULL_KEY) return;
    llRegionSay(RELAY_CHANNEL, session + "," + (string)target + "," + command);
}

default
{
    state_entry()
    {
        owner = llGetOwner();
        replyChannel = -100000000 - (integer)llFrand(1900000000.0);
        controlListen = llListen(CONTROL_CHANNEL, "", owner, "");
        relayListen = llListen(RELAY_CHANNEL, "", NULL_KEY, "");
        replyListen = llListen(replyChannel, "", NULL_KEY, "");
        session = "fs" + (string)((integer)llFrand(2000000000.0));
        viewer("HELLO|" + (string)llGetKey() + "|1");
    }

    attach(key id)
    {
        if (id) { owner = id; llResetScript(); }
    }

    changed(integer change)
    {
        if (change & CHANGED_OWNER) llResetScript();
    }

    listen(integer channel, string name, key id, string message)
    {
        if (channel == CONTROL_CHANNEL)
        {
            if (id != owner) return;
            list p = llParseStringKeepNulls(message, ["|"], []);
            if (llList2String(p, 0) != "FSRLVC") return;
            if (llList2String(p, 1) == "*" && llList2String(p, 2) == "DISCOVER")
            {
                viewer("HELLO|" + (string)llGetKey() + "|1");
                return;
            }
            if ((key)llList2String(p, 1) != llGetKey()) return;
            string op = llList2String(p, 2);
            if (op == "PROBE")
            {
                target = (key)llList2String(p, 3);
                session = "fs" + (string)((integer)llFrand(2000000000.0));
                viewer("PENDING|" + (string)target);
                relay("!version");
                llSetTimerEvent(20.0);
            }
            else if (op == "LIST" && (key)llList2String(p, 3) == target)
            {
                pendingPath = llBase64ToString(llList2String(p, 4));
                string suffix = "";
                if (pendingPath != "") suffix = ":" + pendingPath;
                relay("@getinvworn" + suffix + "=" + (string)replyChannel);
                viewer("PENDING|" + (string)target);
                llSetTimerEvent(20.0);
            }
            else if (op == "ACTION" && (key)llList2String(p, 3) == target)
            {
                string action = llList2String(p, 4);
                string path = llBase64ToString(llList2String(p, 5));
                string command;
                if (action == "attach") command = "@attachover:" + path + "=force";
                else if (action == "attachall") command = "@attachoverall:" + path + "=force";
                else if (action == "detach") command = "@detach:" + path + "=force";
                else if (action == "detachall") command = "@detachall:" + path + "=force";
                if (command != "") { relay(command); viewer("PENDING|" + (string)target); }
                llSetTimerEvent(20.0);
            }
            else if (op == "RELEASE" && (key)llList2String(p, 3) == target)
            {
                relay("!release");
                viewer("RELEASED");
                target = NULL_KEY;
                llSetTimerEvent(0.0);
            }
            return;
        }

        if (channel == RELAY_CHANNEL)
        {
            list p = llParseStringKeepNulls(message, [","], []);
            if (llList2String(p, 0) == "ping" &&
                (key)llList2String(p, 1) == llGetKey() &&
                llList2String(p, 2) == "ping")
            {
                key relayOwner = llGetOwnerKey(id);
                if (target != NULL_KEY && relayOwner == target)
                    llRegionSay(RELAY_CHANNEL, "ping," + (string)target + ",!pong");
                return;
            }
            if (llList2String(p, 0) != session || llGetOwnerKey(id) != target) return;
            string command = llList2String(p, 2);
            string answer = llToLower(llList2String(p, 3));
            if (command == "!version")
            {
                // Relay !version replies with its numeric protocol version,
                // not the normal "ok" acknowledgement.
                if (answer == "ko") viewer("DENIED|" + (string)target);
                else viewer("AUTHORIZED|" + (string)target);
            }
            else if (answer == "ok")
            {
                if (llSubStringIndex(command, "@getinvworn") != 0)
                {
                    viewer("ACTION|" + (string)target + "|ok");
                    llSetTimerEvent(0.0);
                }
            }
            else if (answer == "ko")
            {
                if (llSubStringIndex(command, "@getinvworn") == 0)
                    viewer("ERROR|" + llStringToBase64("Relay refused folder listing"));
                else viewer("ACTION|" + (string)target + "|ko");
                llSetTimerEvent(0.0);
            }
            return;
        }

        if (channel == replyChannel && target != NULL_KEY && id == target)
        {
            viewer("FOLDERS|" + (string)target + "|" + llStringToBase64(pendingPath) + "|" + llStringToBase64(message));
            llSetTimerEvent(0.0);
        }
    }

    timer()
    {
        viewer("DENIED|" + (string)target);
        llSetTimerEvent(0.0);
    }
}
)LSL";
}
