/**
 * @file fsmcpbridge.cpp
 * @brief Local file bridge used by the Firestorm MCP server.
 */

#include "llviewerprecompiledheaders.h"

#include "fsmcpbridge.h"

#include "fsfloateriaassistant.h"
#include "llagent.h"
#include "lldir.h"
#include "lleventtimer.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llviewercontrol.h"

namespace
{
constexpr F32 MCP_PUBLISH_INTERVAL_SECONDS = 1.f;
const char* MCP_BRIDGE_DIRECTORY = "firestorm_mcp_bridge";
const char* MCP_SNAPSHOT_FILE = "snapshot.xml";
const char* MCP_STATUS_FILE = "status.xml";
}

LLEventTimer* FSMCPBridge::sPublishTimer = nullptr;

void FSMCPBridge::init()
{
    if (sPublishTimer)
    {
        return;
    }

    const std::string directory = getBridgeDirectory();
    if (!LLFile::isdir(directory) && LLFile::mkdir(directory) != 0)
    {
        LL_WARNS("FSMCPBridge") << "Unable to create MCP bridge directory" << LL_ENDL;
        return;
    }

    sPublishTimer = LLEventTimer::run_every(MCP_PUBLISH_INTERVAL_SECONDS, []()
    {
        FSMCPBridge::publishNow();
    });
    publishNow();
}

void FSMCPBridge::shutdown()
{
    delete sPublishTimer;
    sPublishTimer = nullptr;
    writeStatus(false, "viewer_stopped");
}

std::string FSMCPBridge::getBridgeDirectory()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, MCP_BRIDGE_DIRECTORY);
}

void FSMCPBridge::publishNow()
{
    const bool enabled = gSavedSettings.getBOOL("FSAIAssistantMCPEnabled");
    if (!enabled)
    {
        LLFile::remove(gDirUtilp->add(getBridgeDirectory(), MCP_SNAPSHOT_FILE), ENOENT);
        writeStatus(false, "disabled_in_viewer");
        return;
    }

    LLSD snapshot;
    if (gAgent.isInitialized())
    {
        snapshot = FSAIAssistantService::collectSnapshot();
    }
    else
    {
        // Runtime avatar and inventory services are not safe to inspect until login completes.
        snapshot["schema"] = "firestorm-ai-assistant-snapshot-v1";
        snapshot["wearables"] = LLSD::emptyArray();
        snapshot["attachments"] = LLSD::emptyArray();
        snapshot["animations"] = LLSD::emptyArray();
        snapshot["selected_objects"] = LLSD::emptyArray();
    }
    snapshot["captured_at"] = LLDate::now();
    snapshot["mcp_bridge"]["protocol_version"] = 1;
    snapshot["mcp_bridge"]["read_only"] = true;
    snapshot["mcp_bridge"]["viewer_logged_in"] = gAgent.isInitialized();

    const std::string snapshot_path = gDirUtilp->add(getBridgeDirectory(), MCP_SNAPSHOT_FILE);
    if (writeAtomicXML(snapshot_path, snapshot))
    {
        writeStatus(true, gAgent.isInitialized() ? "ready" : "waiting_for_login");
    }
    else
    {
        writeStatus(true, "snapshot_write_failed");
    }
}

void FSMCPBridge::writeStatus(bool enabled, const std::string& message)
{
    LLSD status;
    status["protocol_version"] = 1;
    status["enabled"] = enabled;
    status["state"] = message;
    status["updated_at"] = LLDate::now();
    status["snapshot_file"] = MCP_SNAPSHOT_FILE;
    status["read_only"] = true;
    writeAtomicXML(gDirUtilp->add(getBridgeDirectory(), MCP_STATUS_FILE), status);
}

bool FSMCPBridge::writeAtomicXML(const std::string& filename, const LLSD& data)
{
    const std::string temporary = filename + ".tmp";
    llofstream file(temporary.c_str(), std::ios::out | std::ios::trunc);
    if (!file.is_open())
    {
        return false;
    }

    LLSDSerialize::toPrettyXML(data, file);
    file.close();
    if (!file.good())
    {
        LLFile::remove(temporary, ENOENT);
        return false;
    }
    return LLFile::rename(temporary, filename) == 0;
}
