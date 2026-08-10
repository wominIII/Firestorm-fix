/**
 * @file fsmcpbridge.h
 * @brief Local file bridge used by the Firestorm MCP server.
 */

#ifndef FS_FSMCPBRIDGE_H
#define FS_FSMCPBRIDGE_H

#include <string>

class LLEventTimer;
class LLSD;

class FSMCPBridge
{
public:
    static void init();
    static void shutdown();
    static void publishNow();
    static std::string getBridgeDirectory();

private:
    static void processPendingRequest();
    static LLSD executeRequest(const LLSD& request);
    static void writeResult(const LLSD& result);
    static void writeStatus(bool enabled, const std::string& message);
    static bool writeAtomicXML(const std::string& filename, const LLSD& data);

    static LLEventTimer* sPublishTimer;
};

#endif // FS_FSMCPBRIDGE_H
