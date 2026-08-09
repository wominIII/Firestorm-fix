/**
 * @file fsmcpbridge.h
 * @brief Local file bridge used by the Firestorm MCP server.
 */

#ifndef FS_FSMCPBRIDGE_H
#define FS_FSMCPBRIDGE_H

#include <string>

class LLEventTimer;

class FSMCPBridge
{
public:
    static void init();
    static void shutdown();
    static void publishNow();
    static std::string getBridgeDirectory();

private:
    static void writeStatus(bool enabled, const std::string& message);
    static bool writeAtomicXML(const std::string& filename, const class LLSD& data);

    static LLEventTimer* sPublishTimer;
};

#endif // FS_FSMCPBRIDGE_H
