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
#include "llframetimer.h"
#include "llinventorymodel.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string_view>
#include <vector>

namespace
{
U64 s_auto_stop_generation = 0;
std::map<LLUUID, LLSD> s_avatar_sound_candidates;
std::map<LLUUID, F64> s_last_sound_trigger;
std::map<std::string, F64> s_last_chat_trigger;

std::string trimmedLower(std::string value)
{
    LLStringUtil::trim(value);
    LLStringUtil::toLower(value);
    return value;
}

std::vector<std::string> splitLines(const std::string& value)
{
    std::vector<std::string> result;
    std::string current;
    for (const char c : value)
    {
        if (c == '\n' || c == '\r' || c == ',' || c == ';')
        {
            LLStringUtil::trim(current);
            if (!current.empty()) result.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    LLStringUtil::trim(current);
    if (!current.empty()) result.push_back(current);
    return result;
}

bool senderAllowed(const LLUUID& sender_id, const std::string& sender_name)
{
    const std::vector<std::string> allowed = splitLines(gSavedSettings.getString("FSXToysChatAllowedSenders"));
    if (allowed.empty()) return true;

    const std::string id = trimmedLower(sender_id.asString());
    const std::string name = trimmedLower(sender_name);
    for (const std::string& entry : allowed)
    {
        const std::string normalized = trimmedLower(entry);
        if (normalized == id || normalized == name) return true;
    }
    return false;
}

bool sourceEnabled(const std::string& source_scope)
{
    if (source_scope == "nearby") return gSavedSettings.getBOOL("FSXToysChatNearby");
    if (source_scope == "im") return gSavedSettings.getBOOL("FSXToysChatIM");
    if (source_scope == "object") return gSavedSettings.getBOOL("FSXToysChatObject");
    return false;
}

bool findKeyword(const std::string& message, std::string& matched_keyword)
{
    const bool ignore_case = gSavedSettings.getBOOL("FSXToysChatIgnoreCase");
    const bool default_exact = gSavedSettings.getString("FSXToysChatMatchMode") == "exact";
    std::string haystack = message;
    LLStringUtil::trim(haystack);
    if (ignore_case) LLStringUtil::toLower(haystack);

    for (std::string rule : splitLines(gSavedSettings.getString("FSXToysChatKeywords")))
    {
        if (rule.empty() || rule[0] == '#') continue;
        bool exact = default_exact;
        if (rule.rfind("exact:", 0) == 0)
        {
            exact = true;
            rule.erase(0, 6);
        }
        else if (rule.rfind("contains:", 0) == 0)
        {
            exact = false;
            rule.erase(0, 9);
        }
        LLStringUtil::trim(rule);
        if (rule.empty()) continue;

        std::string needle = rule;
        if (ignore_case) LLStringUtil::toLower(needle);
        if ((exact && haystack == needle) || (!exact && haystack.find(needle) != std::string::npos))
        {
            matched_keyword = rule;
            return true;
        }
    }
    return false;
}

S32 extractPercent(const std::string& message)
{
    for (std::string::size_type i = 0; i < message.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(message[i]))) continue;
        S32 value = 0;
        std::string::size_type j = i;
        while (j < message.size() && std::isdigit(static_cast<unsigned char>(message[j])))
        {
            value = value * 10 + (message[j] - '0');
            ++j;
        }
        if (value >= 0 && value <= 100) return value;
        i = j;
    }
    return -1;
}

std::set<LLUUID> selectedSoundIds()
{
    std::set<LLUUID> result;
    std::string value = gSavedSettings.getString("FSXToysSelectedSoundIds");
    std::string::size_type start = 0;
    while (start <= value.size())
    {
        const std::string::size_type comma = value.find(',', start);
        std::string token = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        LLStringUtil::trim(token);
        LLUUID id(token);
        if (id.notNull()) result.insert(id);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

std::string attachmentName(LLViewerObject* object)
{
    if (!object) return std::string();
    LLViewerObject* root = object->getRootEdit();
    if (!root) root = object;
    if (const LLViewerInventoryItem* item = gInventory.getItem(root->getAttachmentItemID()))
    {
        return item->getName();
    }
    return std::string();
}

void rememberSound(const LLUUID& sound_id, LLViewerObject* object,
                   const std::string& sound_name, const std::string& source_type)
{
    if (sound_id.isNull() || !object) return;
    LLSD& candidate = s_avatar_sound_candidates[sound_id];
    candidate["sound_id"] = sound_id;
    if (!sound_name.empty()) candidate["sound_name"] = sound_name;
    if (!candidate.has("sound_name")) candidate["sound_name"] = sound_id.asString();
    candidate["object_id"] = object->getID();
    candidate["attachment_name"] = attachmentName(object);
    candidate["source_type"] = source_type;
    candidate["last_seen"] = LLDate::now();
}

void scanAttachmentPrim(LLViewerObject* object)
{
    if (!object) return;
    if (!object->getInventoryRoot())
    {
        object->requestInventory();
    }
    else
    {
        LLInventoryObject::object_list_t contents;
        object->getInventoryContents(contents);
        for (const LLPointer<LLInventoryObject>& entry : contents)
        {
            const LLInventoryItem* item = entry.notNull()
                ? dynamic_cast<const LLInventoryItem*>(entry.get()) : nullptr;
            if (item && item->getActualType() == LLAssetType::AT_SOUND && item->getAssetUUID().notNull())
            {
                rememberSound(item->getAssetUUID(), object, item->getName(), "attachment_inventory");
            }
        }
    }
    for (const LLPointer<LLViewerObject>& child : object->getChildren())
    {
        if (child.notNull()) scanAttachmentPrim(child.get());
    }
}

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

void FSXToysBridge::notifyAvatarSound(const LLUUID& sound_id, const LLUUID& object_id,
                                      F32 gain, const std::string& source_type)
{
    if (sound_id.isNull() || object_id.isNull() || !gAgentAvatarp) return;
    LLViewerObject* object = gObjectList.findObject(object_id);
    if (!object || object->getAvatar() != gAgentAvatarp) return;

    std::string sound_name;
    LLInventoryObject::object_list_t contents;
    object->getInventoryContents(contents);
    for (const LLPointer<LLInventoryObject>& entry : contents)
    {
        const LLInventoryItem* item = entry.notNull()
            ? dynamic_cast<const LLInventoryItem*>(entry.get()) : nullptr;
        if (item && item->getActualType() == LLAssetType::AT_SOUND && item->getAssetUUID() == sound_id)
        {
            sound_name = item->getName();
            break;
        }
    }
    rememberSound(sound_id, object, sound_name, source_type);

    if (!gSavedSettings.getBOOL("FSXToysEnabled") ||
        !gSavedSettings.getBOOL("FSXToysTriggerSelectedSound") ||
        selectedSoundIds().count(sound_id) == 0)
    {
        return;
    }

    const F64 now = LLFrameTimer::getTotalSeconds();
    const F32 cooldown = llclamp(gSavedSettings.getF32("FSXToysSoundTriggerCooldown"), 0.1f, 60.f);
    const auto last = s_last_sound_trigger.find(sound_id);
    if (last != s_last_sound_trigger.end() && now - last->second < cooldown) return;
    s_last_sound_trigger[sound_id] = now;

    const LLSD& candidate = s_avatar_sound_candidates[sound_id];
    LLSD data;
    data["sound_id"] = sound_id;
    data["sound_name"] = candidate["sound_name"];
    data["attachment_name"] = candidate["attachment_name"];
    data["event_kind"] = "selected_sound";
    data["gain"] = llclampf(gain);
    data["source_type"] = source_type;
    // Reuse the established wall-collision action so existing XToys front-end
    // scripts react without requiring another global trigger to be wired.
    sendEvent("sl_wall_collision", data);
    scheduleStop(llclamp(gSavedSettings.getF32("FSXToysSoundTriggerDuration"), 0.5f, 60.f));
}

void FSXToysBridge::notifyChatMessage(const std::string& message, const LLUUID& sender_id,
                                      const std::string& sender_name, const std::string& source_scope)
{
    if (!gSavedSettings.getBOOL("FSXToysEnabled") ||
        !gSavedSettings.getBOOL("FSXToysChatEnabled") ||
        message.empty() || sender_id == gAgentID || !sourceEnabled(source_scope) ||
        !senderAllowed(sender_id, sender_name))
    {
        return;
    }

    std::string trimmed = message;
    LLStringUtil::trim(trimmed);
    if (trimmed.empty() || trimmed[0] == '@') return;

    std::string matched_keyword;
    bool caeils_hud = false;
    S32 hud_percent = -1;
    if (source_scope == "object" && gSavedSettings.getBOOL("FSXToysCaeilsHudV2Enabled"))
    {
        const std::string marker = trimmedLower(gSavedSettings.getString("FSXToysCaeilsHudV2Marker"));
        std::string normalized_message = trimmedLower(trimmed);
        if (!marker.empty() && normalized_message.find(marker) != std::string::npos)
        {
            caeils_hud = true;
            matched_keyword = "caeils_hud_v2";
            hud_percent = extractPercent(trimmed);
        }
    }

    if (!caeils_hud && !findKeyword(trimmed, matched_keyword)) return;

    const F64 now = LLFrameTimer::getTotalSeconds();
    const F32 cooldown = llclamp(gSavedSettings.getF32("FSXToysChatTriggerCooldown"), 0.1f, 60.f);
    const std::string cooldown_key = sender_id.asString() + "|" + source_scope + "|" + trimmedLower(matched_keyword);
    const auto last = s_last_chat_trigger.find(cooldown_key);
    if (last != s_last_chat_trigger.end() && now - last->second < cooldown) return;
    s_last_chat_trigger[cooldown_key] = now;

    const F32 duration = llclamp(gSavedSettings.getF32("FSXToysChatTriggerDuration"), 0.5f, 60.f);
    LLSD data;
    data["event_kind"] = caeils_hud ? "caeils_hud_v2" : "chat_keyword";
    data["keyword"] = matched_keyword;
    data["source_scope"] = source_scope;
    data["sender_id"] = sender_id;
    data["sender_name"] = sender_name;
    data["duration"] = duration;
    if (hud_percent >= 0)
    {
        data["hud_percent"] = hud_percent;
        data["level"] = llclamp((hud_percent + 9) / 10, 1, 10);
    }
    sendEvent("sl_chat_keyword", data);
    scheduleStop(duration);
}

void FSXToysBridge::refreshAvatarSoundCandidates()
{
    if (!gAgentAvatarp) return;
    for (const auto& attachment_entry : gAgentAvatarp->mAttachmentPoints)
    {
        const LLViewerJointAttachment* point = attachment_entry.second;
        if (!point) continue;
        for (const LLPointer<LLViewerObject>& object : point->mAttachedObjects)
        {
            if (object.notNull()) scanAttachmentPrim(object.get());
        }
    }
}

LLSD FSXToysBridge::getAvatarSoundCandidates()
{
    LLSD result = LLSD::emptyArray();
    for (const auto& candidate : s_avatar_sound_candidates)
    {
        result.append(candidate.second);
    }
    return result;
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
