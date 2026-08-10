/**
 * @file fsmcpbridge.cpp
 * @brief Local file bridge used by the Firestorm MCP server.
 */

#include "llviewerprecompiledheaders.h"

#include "fsmcpbridge.h"

#include "fsfloateriaassistant.h"
#include "llagent.h"
#include "llassetstorage.h"
#include "lldir.h"
#include "lleventtimer.h"
#include "llfile.h"
#include "llfilesystem.h"
#include "llfoldertype.h"
#include "llinventoryfunctions.h"
#include "llselectmgr.h"
#include "llsdserialize.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"

#include <cmath>
#include <memory>

namespace
{
constexpr F32 MCP_PUBLISH_INTERVAL_SECONDS = 1.f;
const char* MCP_BRIDGE_DIRECTORY = "firestorm_mcp_bridge";
const char* MCP_SNAPSHOT_FILE = "snapshot.xml";
const char* MCP_STATUS_FILE = "status.xml";
const char* MCP_REQUEST_FILE = "request.xml";
const char* MCP_RESULT_FILE = "result.xml";
constexpr S32 MCP_MAX_SCRIPT_SOURCE_BYTES = 256 * 1024;

struct ScriptReadContext
{
    LLSD request;
    LLUUID object_id;
    LLUUID item_id;
    std::string name;
};

bool readVector3(const LLSD& value, LLVector3& result)
{
    if (!value.isArray() || value.size() != 3)
    {
        return false;
    }
    for (S32 i = 0; i < 3; ++i)
    {
        const F64 component = value[i].asReal();
        if (!std::isfinite(component)) return false;
        result.mV[i] = static_cast<F32>(component);
    }
    return true;
}

bool readQuaternion(const LLSD& value, LLQuaternion& result)
{
    if (!value.isArray() || value.size() != 4)
    {
        return false;
    }
    F32 components[4];
    for (S32 i = 0; i < 4; ++i)
    {
        const F64 component = value[i].asReal();
        if (!std::isfinite(component)) return false;
        components[i] = static_cast<F32>(component);
    }
    result.set(components);
    return result.normalize() > 0.0001f;
}

bool readRealArray(const LLSD& value, S32 expected_size, std::vector<F32>& result)
{
    if (!value.isArray() || static_cast<S32>(value.size()) != expected_size)
    {
        return false;
    }
    result.clear();
    for (S32 i = 0; i < expected_size; ++i)
    {
        const F64 component = value[i].asReal();
        if (!std::isfinite(component)) return false;
        result.push_back(static_cast<F32>(component));
    }
    return true;
}

LLSD failureResult(const LLSD& request, const std::string& message)
{
    LLSD result;
    result["request_id"] = request["request_id"];
    result["success"] = false;
    result["state"] = "failed";
    result["message"] = message;
    result["completed_at"] = LLDate::now();
    return result;
}

LLSD scriptResult(const LLSD& request, bool success, const std::string& state,
                  const std::string& message)
{
    LLSD result;
    result["request_id"] = request["request_id"];
    result["success"] = success;
    result["state"] = state;
    result["message"] = message;
    result["completed_at"] = LLDate::now();
    return result;
}

LLViewerInventoryItem* findTaskScript(LLViewerObject* object, const LLUUID& item_id)
{
    if (!object || item_id.isNull()) return nullptr;
    LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(object->getInventoryObject(item_id));
    return item && item->getType() == LLAssetType::AT_LSL_TEXT ? item : nullptr;
}

bool canReadScript(const LLViewerInventoryItem* item)
{
    return item &&
        item->getPermissions().allowCopyBy(gAgentID, gAgent.getGroupID()) &&
        item->getPermissions().allowModifyBy(gAgentID, gAgent.getGroupID());
}

bool canModifyScript(const LLViewerInventoryItem* item)
{
    return item && item->getPermissions().allowModifyBy(gAgentID, gAgent.getGroupID());
}
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
        LLFile::remove(gDirUtilp->add(getBridgeDirectory(), MCP_REQUEST_FILE), ENOENT);
        writeStatus(false, "disabled_in_viewer");
        return;
    }

    processPendingRequest();

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
    snapshot["mcp_bridge"]["protocol_version"] = 3;
    snapshot["mcp_bridge"]["read_only"] = false;
    snapshot["mcp_bridge"]["direct_transform_write"] = true;
    snapshot["mcp_bridge"]["script_management"] = true;
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

void FSMCPBridge::processPendingRequest()
{
    const std::string request_path = gDirUtilp->add(getBridgeDirectory(), MCP_REQUEST_FILE);
    if (!LLFile::isfile(request_path))
    {
        return;
    }

    LLSD request;
    llifstream file(request_path);
    const bool parsed = file.is_open() &&
        LLSDSerialize::fromXML(request, file) != LLSDParser::PARSE_FAILURE && request.isMap();
    file.close();
    LLFile::remove(request_path, ENOENT);

    if (!parsed)
    {
        writeResult(failureResult(request, "Invalid MCP request document"));
        return;
    }
    LLSD result = executeRequest(request);
    if (!result.isUndefined())
    {
        writeResult(result);
    }
}

LLSD FSMCPBridge::executeRequest(const LLSD& request)
{
    if (request["protocol_version"].asInteger() < 2 || request["protocol_version"].asInteger() > 3)
    {
        return failureResult(request, "Unsupported MCP write protocol version");
    }
    if (request["expires_at"].asReal() < LLDate::now().secondsSinceEpoch())
    {
        return failureResult(request, "MCP request expired before the viewer processed it");
    }
    const std::string action = request["action"].asString();
    if (action != "set_object_transform" && action != "set_object_face_material" &&
        action != "create_object_script" && action != "read_object_script" &&
        action != "update_object_script" && action != "delete_object_script" &&
        action != "set_object_script_running" && action != "reset_object_script")
    {
        return failureResult(request, "Unsupported MCP action");
    }

    const LLUUID object_id(request["object_id"].asString());
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    LLViewerObject* object = gObjectList.findObject(object_id);
    if (object_id.isNull() || !object || !object->isSelected() ||
        selection->getRootObjectCount() != 1 || selection->getFirstRootObject(true) != object)
    {
        return failureResult(request, "Target must be the only selected root object in Firestorm");
    }
    if (!object->isRootEdit())
    {
        return failureResult(request, "Linked child editing is not supported by MCP direct write");
    }

    const bool script_action = action == "create_object_script" || action == "read_object_script" ||
        action == "update_object_script" || action == "delete_object_script" ||
        action == "set_object_script_running" || action == "reset_object_script";
    if (script_action)
    {
        if (!object->permModify())
        {
            return failureResult(request, "Object does not grant modify permission");
        }
        if (!object->getRegion())
        {
            return failureResult(request, "Object is not in an active simulator region");
        }

        if (action == "create_object_script")
        {
            std::string name = request["name"].asString();
            LLStringUtil::trim(name);
            const std::string source = request["source"].asString();
            const bool running = request.has("running") ? request["running"].asBoolean() : true;
            if (name.empty() || name.size() > 63)
            {
                return failureResult(request, "Script name must contain 1 to 63 bytes");
            }
            if (source.size() > MCP_MAX_SCRIPT_SOURCE_BYTES)
            {
                return failureResult(request, "Script source exceeds the 256 KiB MCP limit");
            }
            const LLUUID parent = gInventory.findCategoryUUIDForType(LLFolderType::FT_LSL_TEXT);
            if (parent.isNull())
            {
                return failureResult(request, "Could not find the agent Scripts inventory folder");
            }

            LLPointer<LLInventoryCallback> callback = new LLBoostFuncInventoryCallback(
                [request, object_id, source, running](const LLUUID& inventory_item_id)
                {
                    if (inventory_item_id.isNull())
                    {
                        writeResult(failureResult(request, "Simulator did not create the temporary script inventory item"));
                        return;
                    }
                    const std::string url = gAgent.getRegionCapability("UpdateScriptAgent");
                    if (url.empty())
                    {
                        writeResult(failureResult(request, "Region does not provide UpdateScriptAgent"));
                        return;
                    }
                    LLResourceUploadInfo::ptr_t upload_info(std::make_shared<LLScriptAssetUpload>(
                        inventory_item_id, source,
                        [request, object_id, running](LLUUID item_id, LLUUID, LLUUID new_item_id, LLSD response)
                        {
                            const LLUUID agent_item_id = new_item_id.notNull() ? new_item_id : item_id;
                            LLViewerObject* target = gObjectList.findObject(object_id);
                            const LLViewerInventoryItem* inventory_item = gInventory.getItem(agent_item_id);
                            if (!target || target->isDead() || !target->permModify() || !inventory_item)
                            {
                                writeResult(failureResult(request, "Script uploaded, but the selected object or inventory item is no longer available"));
                                return;
                            }
                            target->saveScript(inventory_item, running, true);
                            LLSD result = scriptResult(request, true, "applied",
                                "Script uploaded and inserted into the object; a backup remains in the agent Scripts folder");
                            result["object_id"] = object_id;
                            result["agent_inventory_item_id"] = agent_item_id;
                            result["name"] = inventory_item->getName();
                            result["running"] = running;
                            result["compile_response"] = response;
                            writeResult(result);
                        },
                        [request](LLUUID, LLUUID, LLSD response, std::string reason)
                        {
                            LLSD result = failureResult(request, "Script source upload failed: " + reason);
                            result["upload_response"] = response;
                            writeResult(result);
                            return true;
                        }));
                    LLViewerAssetUpload::EnqueueInventoryUpload(url, upload_info);
                });
            create_inventory_item(gAgentID, gAgentSessionID, parent, LLTransactionID::tnull,
                name, "Created through Firestorm MCP", LLAssetType::AT_LSL_TEXT,
                LLInventoryType::IT_LSL, NO_INV_SUBTYPE, PERM_MOVE | PERM_TRANSFER, callback);
            return LLSD();
        }

        const LLUUID item_id(request["item_id"].asString());
        LLViewerInventoryItem* item = findTaskScript(object, item_id);
        if (!item)
        {
            if (!object->getInventoryRoot()) object->requestInventory();
            return failureResult(request, "Script item is not present in the loaded object inventory; refresh Contents and retry");
        }

        if (action == "read_object_script")
        {
            if (!canReadScript(item))
            {
                return failureResult(request, "Script source requires both copy and modify permission");
            }
            ScriptReadContext* context = new ScriptReadContext{request, object_id, item_id, item->getName()};
            gAssetStorage->getInvItemAsset(object->getRegion()->getHost(), gAgentID, gAgentSessionID,
                item->getPermissions().getOwner(), object_id, item_id, item->getAssetUUID(),
                item->getType(), &FSMCPBridge::onScriptSourceLoaded, context, true);
            return LLSD();
        }

        if (!canModifyScript(item))
        {
            return failureResult(request, "Script does not grant modify permission");
        }

        if (action == "update_object_script")
        {
            const std::string source = request["source"].asString();
            const bool running = request.has("running") ? request["running"].asBoolean() : true;
            if (source.size() > MCP_MAX_SCRIPT_SOURCE_BYTES)
            {
                return failureResult(request, "Script source exceeds the 256 KiB MCP limit");
            }
            const std::string url = object->getRegion()->getCapability("UpdateScriptTask");
            if (url.empty())
            {
                return failureResult(request, "Region does not provide UpdateScriptTask");
            }
            LLResourceUploadInfo::ptr_t upload_info(std::make_shared<LLScriptAssetUpload>(
                object_id, item_id, LLScriptAssetUpload::MONO, running, LLUUID::null, source,
                [request, object_id, running](LLUUID script_item_id, LLUUID, LLUUID new_asset_id, LLSD response)
                {
                    const bool compiled = response["compiled"].asBoolean();
                    LLSD result = scriptResult(request, compiled, compiled ? "applied" : "compile_failed",
                        compiled ? "Script source compiled and was saved" : "Simulator rejected the script during compilation");
                    result["object_id"] = object_id;
                    result["item_id"] = script_item_id;
                    result["asset_id"] = new_asset_id;
                    result["running"] = running;
                    result["compile_response"] = response;
                    writeResult(result);
                },
                [request](LLUUID, LLUUID, LLSD response, std::string reason)
                {
                    LLSD result = failureResult(request, "Script update failed: " + reason);
                    result["upload_response"] = response;
                    writeResult(result);
                    return true;
                }));
            LLViewerAssetUpload::EnqueueInventoryUpload(url, upload_info);
            return LLSD();
        }

        if (action == "delete_object_script")
        {
            object->removeInventory(item_id);
            LLSD result = scriptResult(request, true, "applied", "Script removal sent to the simulator");
            result["object_id"] = object_id;
            result["item_id"] = item_id;
            return result;
        }

        LLMessageSystem* message = gMessageSystem;
        if (action == "set_object_script_running")
        {
            const bool running = request["running"].asBoolean();
            message->newMessageFast(_PREHASH_SetScriptRunning);
            message->nextBlockFast(_PREHASH_AgentData);
            message->addUUIDFast(_PREHASH_AgentID, gAgentID);
            message->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
            message->nextBlockFast(_PREHASH_Script);
            message->addUUIDFast(_PREHASH_ObjectID, object_id);
            message->addUUIDFast(_PREHASH_ItemID, item_id);
            message->addBOOLFast(_PREHASH_Running, running);
            message->sendReliable(object->getRegion()->getHost());
            LLSD result = scriptResult(request, true, "applied", "Script running state update sent to the simulator");
            result["object_id"] = object_id;
            result["item_id"] = item_id;
            result["running"] = running;
            return result;
        }

        message->newMessageFast(_PREHASH_ScriptReset);
        message->nextBlockFast(_PREHASH_AgentData);
        message->addUUIDFast(_PREHASH_AgentID, gAgentID);
        message->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
        message->nextBlockFast(_PREHASH_Script);
        message->addUUIDFast(_PREHASH_ObjectID, object_id);
        message->addUUIDFast(_PREHASH_ItemID, item_id);
        message->sendReliable(object->getRegion()->getHost());
        LLSD result = scriptResult(request, true, "applied", "Script reset sent to the simulator");
        result["object_id"] = object_id;
        result["item_id"] = item_id;
        return result;
    }


    if (action == "set_object_face_material")
    {
        if (!object->permModify())
        {
            return failureResult(request, "Object does not grant modify permission");
        }
        const S32 face = request["face"].asInteger();
        if (face < 0 || face >= object->getNumTEs())
        {
            return failureResult(request, "Face index is outside the object's face range");
        }

        const bool has_diffuse = request.has("diffuse_texture_id");
        const bool has_pbr = request.has("pbr_material_id");
        const bool has_color = request.has("color");
        const bool has_uv_scale = request.has("texture_scale");
        const bool has_uv_offset = request.has("texture_offset");
        const bool has_uv_rotation = request.has("texture_rotation_radians");
        if (!has_diffuse && !has_pbr && !has_color && !has_uv_scale && !has_uv_offset && !has_uv_rotation)
        {
            return failureResult(request, "No face material fields were provided");
        }

        std::vector<F32> color;
        std::vector<F32> uv_scale;
        std::vector<F32> uv_offset;
        LLUUID diffuse_id;
        LLUUID pbr_id;
        if (has_color && (!readRealArray(request["color"], 4, color) ||
            color[0] < 0.f || color[0] > 1.f || color[1] < 0.f || color[1] > 1.f ||
            color[2] < 0.f || color[2] > 1.f || color[3] < 0.f || color[3] > 1.f))
        {
            return failureResult(request, "color must contain four finite values from 0 to 1");
        }
        if (has_uv_scale && !readRealArray(request["texture_scale"], 2, uv_scale))
        {
            return failureResult(request, "texture_scale must contain two finite numbers");
        }
        if (has_uv_offset && !readRealArray(request["texture_offset"], 2, uv_offset))
        {
            return failureResult(request, "texture_offset must contain two finite numbers");
        }
        const F64 uv_rotation = request["texture_rotation_radians"].asReal();
        if (has_uv_rotation && !std::isfinite(uv_rotation))
        {
            return failureResult(request, "texture_rotation_radians must be finite");
        }
        if (has_diffuse)
        {
            diffuse_id.set(request["diffuse_texture_id"].asString());
            if (diffuse_id.isNull()) return failureResult(request, "diffuse_texture_id must be a valid UUID");
        }
        if (has_pbr)
        {
            const std::string pbr_value = request["pbr_material_id"].asString();
            pbr_id.set(pbr_value);
            if (!pbr_value.empty() && pbr_id.isNull())
            {
                return failureResult(request, "pbr_material_id must be a UUID or an empty string to clear it");
            }
        }

        if (has_diffuse)
        {
            object->setTETexture(static_cast<U8>(face), diffuse_id);
        }
        if (has_color)
        {
            object->setTEColor(static_cast<U8>(face), LLColor4(color[0], color[1], color[2], color[3]));
        }
        if (has_uv_scale) object->setTEScale(static_cast<U8>(face), uv_scale[0], uv_scale[1]);
        if (has_uv_offset) object->setTEOffset(static_cast<U8>(face), uv_offset[0], uv_offset[1]);
        if (has_uv_rotation) object->setTERotation(static_cast<U8>(face), static_cast<F32>(uv_rotation));
        if (has_pbr)
        {
            object->setRenderMaterialID(face, pbr_id, false, true);
        }
        if (has_diffuse || has_color || has_uv_scale || has_uv_offset || has_uv_rotation)
        {
            object->sendTEUpdate();
        }
        if (has_pbr)
        {
            object->sendMaterialUpdate();
        }

        LLSD result;
        result["request_id"] = request["request_id"];
        result["success"] = true;
        result["state"] = "applied";
        result["message"] = "Face material update sent to the simulator";
        result["object_id"] = object_id;
        result["face"] = face;
        result["completed_at"] = LLDate::now();
        return result;
    }

    const bool has_position = request.has("position");
    const bool has_rotation = request.has("rotation_quaternion");
    const bool has_scale = request.has("scale");
    if (!has_position && !has_rotation && !has_scale)
    {
        return failureResult(request, "No transform fields were provided");
    }
    if (has_position && !object->permMove())
    {
        return failureResult(request, "Object does not grant move permission");
    }
    if ((has_rotation || has_scale) && !object->permModify())
    {
        return failureResult(request, "Object does not grant modify permission");
    }

    LLVector3 position;
    LLVector3 scale;
    LLQuaternion rotation;
    if (has_position && !readVector3(request["position"], position))
    {
        return failureResult(request, "position must contain three finite numbers");
    }
    if (has_rotation && !readQuaternion(request["rotation_quaternion"], rotation))
    {
        return failureResult(request, "rotation_quaternion must contain four finite numbers");
    }
    if (has_scale && !readVector3(request["scale"], scale))
    {
        return failureResult(request, "scale must contain three finite numbers");
    }

    if (has_position)
    {
        if (object->isAttachment())
        {
            if (position.length() > MAX_ATTACHMENT_DIST)
            {
                return failureResult(request, "Attachment position exceeds the allowed distance");
            }
        }
        else if (!object->getRegion() || !LLWorld::getInstance()->positionRegionValidGlobal(
                     object->getRegion()->getPosGlobalFromRegion(position)))
        {
            return failureResult(request, "World-object position is outside a valid region");
        }
    }
    if (has_scale)
    {
        const F32 min_scale = LLWorld::getInstance()->getRegionMinPrimScale();
        const F32 max_scale = LLWorld::getInstance()->getRegionMaxPrimScale();
        if (scale.mV[VX] < min_scale || scale.mV[VY] < min_scale || scale.mV[VZ] < min_scale ||
            scale.mV[VX] > max_scale || scale.mV[VY] > max_scale || scale.mV[VZ] > max_scale)
        {
            return failureResult(request, "Scale is outside the region's allowed range");
        }
    }

    U32 update_type = UPD_NONE;
    if (has_position)
    {
        if (object->isAttachment()) object->setPosition(position);
        else object->setPositionEdit(position);
        update_type |= UPD_POSITION;
    }
    if (has_rotation)
    {
        object->setRotation(rotation);
        update_type |= UPD_ROTATION;
    }
    if (has_scale)
    {
        object->setScale(scale, true);
        update_type |= UPD_SCALE;
    }
    LLSelectMgr::getInstance()->sendMultipleUpdate(update_type);
    LLSelectMgr::getInstance()->updateSelectionCenter();

    LLSD result;
    result["request_id"] = request["request_id"];
    result["success"] = true;
    result["state"] = "applied";
    result["message"] = "Transform applied and sent to the simulator";
    result["object_id"] = object_id;
    result["position"] = request["position"];
    result["rotation_quaternion"] = request["rotation_quaternion"];
    result["scale"] = request["scale"];
    result["completed_at"] = LLDate::now();
    return result;
}

void FSMCPBridge::onScriptSourceLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                       void* user_data, S32 status, LLExtStat ext_status)
{
    std::unique_ptr<ScriptReadContext> context(static_cast<ScriptReadContext*>(user_data));
    if (!context)
    {
        return;
    }
    if (status != LL_ERR_NOERR)
    {
        LLSD result = failureResult(context->request,
            llformat("Script source download failed (status %d, extended %d)", status, static_cast<S32>(ext_status)));
        result["object_id"] = context->object_id;
        result["item_id"] = context->item_id;
        writeResult(result);
        return;
    }

    LLFileSystem file(asset_id, type, LLFileSystem::READ);
    const S32 size = file.getSize();
    if (size < 0 || size > MCP_MAX_SCRIPT_SOURCE_BYTES + 1)
    {
        writeResult(failureResult(context->request, "Downloaded script source has an invalid size"));
        return;
    }
    std::vector<char> buffer(static_cast<size_t>(size));
    if (size > 0 && (!file.read(reinterpret_cast<U8*>(buffer.data()), size) || file.getLastBytesRead() != size))
    {
        writeResult(failureResult(context->request, "Could not read downloaded script source from the viewer cache"));
        return;
    }
    while (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();

    LLSD result = scriptResult(context->request, true, "loaded", "Script source downloaded");
    result["object_id"] = context->object_id;
    result["item_id"] = context->item_id;
    result["asset_id"] = asset_id;
    result["name"] = context->name;
    result["source"] = std::string(buffer.begin(), buffer.end());
    writeResult(result);
}

void FSMCPBridge::writeResult(const LLSD& result)
{
    writeAtomicXML(gDirUtilp->add(getBridgeDirectory(), MCP_RESULT_FILE), result);
}

void FSMCPBridge::writeStatus(bool enabled, const std::string& message)
{
    LLSD status;
    status["protocol_version"] = 3;
    status["enabled"] = enabled;
    status["state"] = message;
    status["updated_at"] = LLDate::now();
    status["snapshot_file"] = MCP_SNAPSHOT_FILE;
    status["read_only"] = false;
    status["direct_transform_write"] = true;
    status["script_management"] = true;
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
