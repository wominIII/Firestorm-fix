/**
 * @file fsxtoysbridge.cpp
 * @brief Opt-in XToys private-webhook event bridge.
 */

#include "llviewerprecompiledheaders.h"

#include "fsxtoysbridge.h"

#include "llagent.h"
#include "llapp.h"
#include "llcoros.h"
#include "llcorehttputil.h"
#include "lleventcoro.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace
{
U64 s_auto_stop_generation = 0;

bool normalizeWebhookAddress(std::string& webhook_id)
{
    LLStringUtil::trim(webhook_id);
    constexpr std::string_view https_webhook_prefix = "https://webhook.xtoys.app/";
    constexpr std::string_view websocket_webhook_prefix = "wss://webhook.xtoys.app/";
    if (webhook_id.rfind(https_webhook_prefix, 0) == 0)
    {
        webhook_id.erase(0, https_webhook_prefix.size());
    }
    else if (webhook_id.rfind(websocket_webhook_prefix, 0) == 0)
    {
        // XToys exposes the same webhook through WebSocket and HTTPS POST.
        // This bridge uses HTTPS POST but accepts the WebSocket URL copied by XToys.
        webhook_id.erase(0, websocket_webhook_prefix.size());
    }
    if (webhook_id.empty() || webhook_id.size() > 128)
    {
        return false;
    }
    return std::all_of(webhook_id.begin(), webhook_id.end(), [](unsigned char c)
    {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

std::string collisionAction(U8 type)
{
    switch (type)
    {
        case 1: return "sl_avatar_bump";
        case 2: return "sl_script_push";
        case 3: return "sl_selected_object_collision";
        case 4: return "sl_scripted_object_collision";
        case 5: return "sl_physical_object_collision";
        default: return "sl_collision";
    }
}
}

void FSXToysBridge::notifyMeanCollision(U8 type, F32 magnitude)
{
    if (!gSavedSettings.getBOOL("FSXToysEnabled") || !gSavedSettings.getBOOL("FSXToysTriggerMeanCollision"))
    {
        return;
    }

    LLSD data;
    data["collision_type"] = (S32)type;
    data["magnitude"] = llmax(0.f, magnitude);
    sendEvent(collisionAction(type), data);

    // Only avatar bumps are currently bound by the companion XToys script.
    // A subsequent bump resets the timer instead of allowing an old timer to
    // cut a newer interaction short.
    if (type == 1)
    {
        scheduleStop(10.f);
    }
}

void FSXToysBridge::notifyCollisionSound(const LLVector3d& position, F32 gain)
{
    if (!gSavedSettings.getBOOL("FSXToysEnabled") || !gSavedSettings.getBOOL("FSXToysTriggerCollisionSound"))
    {
        return;
    }

    const F32 range = llclamp(gSavedSettings.getF32("FSXToysCollisionSoundRange"), 0.5f, 10.f);
    const F64 distance = dist_vec(position, gAgent.getPositionGlobal());
    if (distance > range)
    {
        return;
    }

    LLSD data;
    data["gain"] = llclampf(gain);
    data["distance"] = (F32)distance;
    sendEvent("sl_wall_collision", data);
    scheduleStop(5.f);
}

void FSXToysBridge::sendTest()
{
    ++s_auto_stop_generation;
    LLSD data;
    data["source"] = "firestorm";
    sendEvent("firestorm_test", data);
}

void FSXToysBridge::sendStop()
{
    ++s_auto_stop_generation;
    LLSD data;
    data["source"] = "firestorm";
    sendEvent("firestorm_stop", data);
}

void FSXToysBridge::scheduleStop(F32 seconds)
{
    const U64 generation = ++s_auto_stop_generation;
    LLCoros::instance().launch("FSXToysAutoStop",
        boost::bind(&FSXToysBridge::stopCoro, generation, seconds));
}

void FSXToysBridge::stopCoro(U64 generation, F32 seconds)
{
    llcoro::suspendUntilTimeout(seconds);
    if (generation == s_auto_stop_generation && LLApp::isRunning()
        && gSavedSettings.getBOOL("FSXToysEnabled"))
    {
        sendStop();
    }
}

void FSXToysBridge::sendEvent(const std::string& action, const LLSD& data)
{
    if (!gSavedSettings.getBOOL("FSXToysEnabled"))
    {
        return;
    }

    std::string webhook_id = gSavedSettings.getString("FSXToysWebhookId");
    if (!normalizeWebhookAddress(webhook_id))
    {
        return;
    }

    LLSD body = data;
    body["action"] = action;
    const std::string token = gSavedSettings.getString("FSXToysWebhookToken");
    const std::string url = "https://webhook.xtoys.app/" + webhook_id;
    LLCoros::instance().launch("FSXToysWebhook", boost::bind(&FSXToysBridge::postCoro, url, token, body));
}

void FSXToysBridge::postCoro(std::string url, std::string token, LLSD body)
{
    LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("FSXToysWebhook", policy);
    auto request = std::make_shared<LLCore::HttpRequest>();
    auto headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append("Accept", "application/json");
    headers->append("Content-Type", "application/json");
    if (!token.empty())
    {
        headers->append("Authorization", "Bearer " + token);
    }

    // No IDs, tokens, positions or event payloads are written to viewer logs.
    adapter->postJsonAndSuspend(request, url, body, headers);
}
