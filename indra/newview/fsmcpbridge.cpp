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
#include "llinventorymodel.h"
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
#include "roles_constants.h"

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
constexpr S32 MCP_MAX_INVENTORY_RESULTS = 1000;
constexpr S32 MCP_MAX_BATCH_ENTRIES = 100;

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
        gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE) &&
        gAgent.allowOperation(PERM_MODIFY, item->getPermissions(), GP_OBJECT_MANIPULATE);
}

bool canModifyScript(const LLViewerInventoryItem* item)
{
    return item && gAgent.allowOperation(PERM_MODIFY, item->getPermissions(), GP_OBJECT_MANIPULATE);
}

bool isObjectInSingleSelectedLinkset(LLViewerObject* object)
{
    if (!object) return false;
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    if (!selection || selection->getRootObjectCount() != 1) return false;
    LLViewerObject* selected_root = selection->getFirstRootObject(true);
    return selected_root && object->getRootEdit() == selected_root->getRootEdit();
}

LLSD agentInventoryPermissions(const LLViewerInventoryItem* item)
{
    LLSD result;
    if (!item)
    {
        result["known"] = false;
        return result;
    }
    const LLPermissions& permissions = item->getPermissions();
    result["known"] = true;
    result["copy"] = gAgent.allowOperation(PERM_COPY, permissions, GP_OBJECT_MANIPULATE);
    result["modify"] = gAgent.allowOperation(PERM_MODIFY, permissions, GP_OBJECT_MANIPULATE);
    result["transfer"] = permissions.allowOperationBy(PERM_TRANSFER, gAgentID, gAgent.getGroupID());
    result["owner_id"] = permissions.getOwner();
    result["creator_id"] = permissions.getCreator();
    result["group_id"] = permissions.getGroup();
    return result;
}

LLSD agentInventoryItemToLLSD(const LLViewerInventoryItem* item)
{
    LLSD result;
    if (!item) return result;
    result["kind"] = "item";
    result["id"] = item->getUUID();
    result["parent_id"] = item->getParentUUID();
    result["name"] = item->getName();
    result["description"] = item->getDescription();
    result["asset_type"] = LLAssetType::lookup(item->getType());
    result["actual_asset_type"] = LLAssetType::lookup(item->getActualType());
    result["inventory_type"] = LLInventoryType::lookup(item->getInventoryType());
    result["is_link"] = item->getIsLinkType();
    result["linked_id"] = item->getLinkedUUID();
    result["flags"] = static_cast<S32>(item->getFlags());
    result["creation_date"] = LLDate(static_cast<F64>(item->getCreationDate()));
    result["permissions"] = agentInventoryPermissions(item);
    return result;
}

LLSD agentInventoryCategoryToLLSD(const LLViewerInventoryCategory* category)
{
    LLSD result;
    if (!category) return result;
    result["kind"] = "folder";
    result["id"] = category->getUUID();
    result["parent_id"] = category->getParentUUID();
    result["name"] = category->getName();
    result["preferred_type"] = LLFolderType::lookup(category->getPreferredType());
    result["version"] = category->getVersion();
    result["descendent_count"] = category->getDescendentCount();
    result["complete"] = gInventory.isCategoryComplete(category->getUUID());
    result["protected_system_folder"] = category->getPreferredType() != LLFolderType::FT_NONE;
    return result;
}

LLSD inventoryPath(const LLUUID& object_id)
{
    LLSD path = LLSD::emptyArray();
    LLUUID current = object_id;
    S32 guard = 0;
    while (current.notNull() && guard++ < 256)
    {
        const LLInventoryObject* object = gInventory.getObject(current);
        if (!object) break;
        LLSD part;
        part["id"] = object->getUUID();
        part["name"] = object->getName();
        path.insert(0, part);
        current = object->getParentUUID();
    }
    return path;
}

bool isAgentInventoryObject(const LLUUID& id)
{
    const LLUUID root = gInventory.getRootFolderID();
    return id == root || gInventory.isObjectDescendentOf(id, root);
}

bool validInventoryName(std::string& name)
{
    LLStringUtil::trim(name);
    return !name.empty() && name.size() <= 63 && name.find('\n') == std::string::npos &&
        name.find('\r') == std::string::npos;
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
    snapshot["mcp_bridge"]["protocol_version"] = 4;
    snapshot["mcp_bridge"]["read_only"] = false;
    snapshot["mcp_bridge"]["direct_transform_write"] = true;
    snapshot["mcp_bridge"]["script_management"] = true;
    snapshot["mcp_bridge"]["inventory_management"] = true;
    snapshot["mcp_bridge"]["detailed_object_reads"] = true;
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
    if (request["protocol_version"].asInteger() < 2 || request["protocol_version"].asInteger() > 4)
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
        action != "update_object_script" && action != "patch_object_script" &&
        action != "delete_object_script" && action != "set_object_script_running" &&
        action != "reset_object_script" && action != "inspect_object" &&
        action != "inventory_get" && action != "inventory_list" &&
        action != "inventory_search" && action != "inventory_create_folder" &&
        action != "inventory_rename" && action != "inventory_move" &&
        action != "inventory_trash")
    {
        return failureResult(request, "Unsupported MCP action");
    }

    const bool inventory_action = action.rfind("inventory_", 0) == 0;
    if (inventory_action)
    {
        if (!gAgent.isInitialized() || gInventory.getRootFolderID().isNull())
        {
            return failureResult(request, "Agent inventory is unavailable before login completes");
        }

        if (action == "inventory_get")
        {
            const LLUUID entry_id(request["entry_id"].asString());
            if (!isAgentInventoryObject(entry_id)) return failureResult(request, "Inventory entry is not in the agent inventory");
            LLSD result = scriptResult(request, true, "loaded", "Inventory entry loaded");
            if (const LLViewerInventoryItem* item = gInventory.getItem(entry_id)) result["entry"] = agentInventoryItemToLLSD(item);
            else if (const LLViewerInventoryCategory* category = gInventory.getCategory(entry_id)) result["entry"] = agentInventoryCategoryToLLSD(category);
            else return failureResult(request, "Inventory entry was not found in the loaded inventory");
            result["path"] = inventoryPath(entry_id);
            return result;
        }

        if (action == "inventory_list" || action == "inventory_search")
        {
            LLUUID folder_id(request["folder_id"].asString());
            if (folder_id.isNull()) folder_id = gInventory.getRootFolderID();
            const LLViewerInventoryCategory* folder = gInventory.getCategory(folder_id);
            if (!folder || !isAgentInventoryObject(folder_id)) return failureResult(request, "Inventory folder was not found in the agent inventory");

            const bool recursive = action == "inventory_search" || request["recursive"].asBoolean();
            const bool include_trash = request["include_trash"].asBoolean();
            const S32 limit = llclamp(request.has("limit") ? request["limit"].asInteger() : 200, 1, MCP_MAX_INVENTORY_RESULTS);
            const bool complete = gInventory.isCategoryComplete(folder_id);
            if (!complete) gInventory.fetchDescendentsOf(folder_id);

            LLInventoryModel::cat_array_t categories;
            LLInventoryModel::item_array_t items;
            if (recursive)
            {
                gInventory.collectDescendents(folder_id, categories, items, include_trash);
            }
            else
            {
                LLInventoryModel::cat_array_t* direct_categories = nullptr;
                LLInventoryModel::item_array_t* direct_items = nullptr;
                gInventory.getDirectDescendentsOf(folder_id, direct_categories, direct_items);
                if (direct_categories) categories = *direct_categories;
                if (direct_items) items = *direct_items;
            }

            std::string query = request["query"].asString();
            LLStringUtil::trim(query);
            LLStringUtil::toLower(query);
            LLSD entries = LLSD::emptyArray();
            bool truncated = false;
            auto matches = [&query](const std::string& name, const std::string& description)
            {
                if (query.empty()) return true;
                std::string searchable = name + "\n" + description;
                LLStringUtil::toLower(searchable);
                return searchable.find(query) != std::string::npos;
            };
            for (const LLPointer<LLViewerInventoryCategory>& category : categories)
            {
                if (category.isNull() || !matches(category->getName(), std::string())) continue;
                if (static_cast<S32>(entries.size()) >= limit) { truncated = true; break; }
                LLSD entry = agentInventoryCategoryToLLSD(category);
                if (recursive) entry["path"] = inventoryPath(category->getUUID());
                entries.append(entry);
            }
            if (!truncated)
            {
                for (const LLPointer<LLViewerInventoryItem>& item : items)
                {
                    if (item.isNull() || !matches(item->getName(), item->getDescription())) continue;
                    if (static_cast<S32>(entries.size()) >= limit) { truncated = true; break; }
                    LLSD entry = agentInventoryItemToLLSD(item);
                    if (recursive) entry["path"] = inventoryPath(item->getUUID());
                    entries.append(entry);
                }
            }
            LLSD result = scriptResult(request, true, complete ? "loaded" : "fetch_requested",
                complete ? "Inventory entries loaded" : "Folder was incomplete; a server fetch was requested, so retry for complete results");
            result["folder"] = agentInventoryCategoryToLLSD(folder);
            result["recursive"] = recursive;
            result["entries"] = entries;
            result["returned"] = static_cast<S32>(entries.size());
            result["truncated"] = truncated;
            result["complete"] = complete;
            return result;
        }

        if (action == "inventory_create_folder")
        {
            LLUUID parent_id(request["parent_id"].asString());
            if (parent_id.isNull()) parent_id = gInventory.getRootFolderID();
            if (!gInventory.getCategory(parent_id) || !isAgentInventoryObject(parent_id))
                return failureResult(request, "Destination parent folder is not in the agent inventory");
            std::string name = request["name"].asString();
            if (!validInventoryName(name)) return failureResult(request, "Folder name must contain 1 to 63 bytes and no line breaks");
            gInventory.createNewCategory(parent_id, LLFolderType::FT_NONE, name,
                [request, parent_id, name](const LLUUID& new_category_id)
                {
                    if (new_category_id.isNull())
                    {
                        writeResult(failureResult(request, "Inventory service did not create the folder"));
                        return;
                    }
                    LLSD result = scriptResult(request, true, "applied", "Inventory folder created");
                    result["folder_id"] = new_category_id;
                    result["parent_id"] = parent_id;
                    result["name"] = name;
                    writeResult(result);
                });
            return LLSD();
        }

        if (action == "inventory_rename")
        {
            const LLUUID entry_id(request["entry_id"].asString());
            std::string name = request["name"].asString();
            if (!validInventoryName(name)) return failureResult(request, "Inventory name must contain 1 to 63 bytes and no line breaks");
            if (!isAgentInventoryObject(entry_id)) return failureResult(request, "Inventory entry is not in the agent inventory");
            if (LLViewerInventoryItem* item = gInventory.getItem(entry_id))
            {
                LLPointer<LLViewerInventoryItem> updated = new LLViewerInventoryItem(item);
                updated->rename(name);
                updated->updateServer(false);
                gInventory.updateItem(updated);
                gInventory.notifyObservers();
            }
            else if (LLViewerInventoryCategory* category = gInventory.getCategory(entry_id))
            {
                if (category->getPreferredType() != LLFolderType::FT_NONE)
                    return failureResult(request, "Protected system folders cannot be renamed through MCP");
                rename_category(&gInventory, entry_id, name);
            }
            else return failureResult(request, "Inventory entry was not found");
            LLSD result = scriptResult(request, true, "applied", "Inventory entry renamed");
            result["entry_id"] = entry_id;
            result["name"] = name;
            return result;
        }

        if (action == "inventory_move" || action == "inventory_trash")
        {
            const LLSD& ids = request["entry_ids"];
            if (!ids.isArray() || ids.size() == 0 || ids.size() > MCP_MAX_BATCH_ENTRIES)
                return failureResult(request, "entry_ids must contain 1 to 100 inventory UUIDs");
            LLUUID destination_id = action == "inventory_trash"
                ? gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH)
                : LLUUID(request["destination_folder_id"].asString());
            if (!gInventory.getCategory(destination_id) || !isAgentInventoryObject(destination_id))
                return failureResult(request, "Destination folder is not in the agent inventory");

            LLSD moved = LLSD::emptyArray();
            LLSD errors = LLSD::emptyArray();
            for (LLSD::array_const_iterator it = ids.beginArray(); it != ids.endArray(); ++it)
            {
                const LLUUID entry_id(it->asString());
                std::string error;
                if (!isAgentInventoryObject(entry_id)) error = "not in agent inventory";
                else if (LLViewerInventoryItem* item = gInventory.getItem(entry_id))
                {
                    gInventory.changeItemParent(item, destination_id, action == "inventory_trash");
                }
                else if (LLViewerInventoryCategory* category = gInventory.getCategory(entry_id))
                {
                    if (category->getPreferredType() != LLFolderType::FT_NONE) error = "protected system folder";
                    else if (entry_id == destination_id || gInventory.isObjectDescendentOf(destination_id, entry_id))
                        error = "destination would create a folder cycle";
                    else gInventory.changeCategoryParent(category, destination_id, action == "inventory_trash");
                }
                else error = "entry not found";

                if (error.empty()) moved.append(entry_id);
                else
                {
                    LLSD failed;
                    failed["entry_id"] = entry_id;
                    failed["error"] = error;
                    errors.append(failed);
                }
            }
            LLSD result = scriptResult(request, errors.size() == 0,
                errors.size() == 0 ? "applied" : "partial",
                action == "inventory_trash" ? "Inventory entries moved to Trash" : "Inventory entries moved");
            result["destination_folder_id"] = destination_id;
            result["moved"] = moved;
            result["errors"] = errors;
            return result;
        }
    }

    const LLUUID object_id(request["object_id"].asString());
    LLViewerObject* object = gObjectList.findObject(object_id);
    if (object_id.isNull() || !object)
    {
        return failureResult(request, "Target object is not currently present in the Viewer object list");
    }

    if (action == "inspect_object")
    {
        const bool include_inventory = request["include_inventory"].asBoolean();
        const bool include_linkset = request.has("include_linkset") ? request["include_linkset"].asBoolean() : true;
        if (include_inventory && !object->getInventoryRoot()) object->requestInventory();
        LLSD result = scriptResult(request, true, "loaded", "Object details loaded");
        result["object"] = FSAIAssistantService::collectObjectSnapshot(object, std::string(), std::string(), include_inventory);
        result["linkset"] = LLSD::emptyArray();
        if (include_linkset)
        {
            LLViewerObject* root = object->getRootEdit();
            if (root)
            {
                result["linkset"].append(FSAIAssistantService::collectObjectSnapshot(root, std::string(), std::string(), false));
                for (const LLPointer<LLViewerObject>& child : root->getChildren())
                {
                    if (child.notNull()) result["linkset"].append(
                        FSAIAssistantService::collectObjectSnapshot(child.get(), std::string(), std::string(), false));
                }
            }
        }
        result["inventory_fetch_requested"] = include_inventory && !object->getInventoryRoot();
        return result;
    }

    if (!isObjectInSingleSelectedLinkset(object))
    {
        return failureResult(request, "Target must belong to the single linkset selected in Firestorm");
    }

    const bool script_action = action == "create_object_script" || action == "read_object_script" ||
        action == "update_object_script" || action == "patch_object_script" || action == "delete_object_script" ||
        action == "set_object_script_running" || action == "reset_object_script";
    if (script_action)
    {
        if (!object->getRegion())
        {
            return failureResult(request, "Object is not in an active simulator region");
        }

        if (action == "create_object_script")
        {
            if (!object->permModify())
            {
                return failureResult(request, "Object does not grant permission to add inventory");
            }
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

        if (action == "read_object_script" || action == "patch_object_script")
        {
            if (!canReadScript(item))
            {
                return failureResult(request,
                    "Viewer-standard script source access requires both copy and modify permission, including applicable group powers");
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
                        compiled ? "Script source compiled and was updated in place; item UUID preserved" : "Simulator rejected the script during compilation");
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
            if (!object->permModify())
            {
                return failureResult(request, "Object does not grant permission to remove inventory");
            }
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

    if (!object->isRootEdit())
    {
        return failureResult(request, "Transform writes require the selected linkset root object");
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

    std::string source(buffer.begin(), buffer.end());
    if (context->request["action"].asString() == "patch_object_script")
    {
        const LLSD& edits = context->request["edits"];
        if (!edits.isArray() || edits.size() == 0 || edits.size() > 100)
        {
            writeResult(failureResult(context->request, "edits must contain 1 to 100 exact text replacements"));
            return;
        }

        S32 applied = 0;
        for (LLSD::array_const_iterator it = edits.beginArray(); it != edits.endArray(); ++it)
        {
            const std::string old_text = (*it)["old_text"].asString();
            const std::string new_text = (*it)["new_text"].asString();
            const bool replace_all = (*it)["replace_all"].asBoolean();
            if (old_text.empty())
            {
                writeResult(failureResult(context->request, "old_text cannot be empty"));
                return;
            }

            S32 matches = 0;
            std::string::size_type search = 0;
            while ((search = source.find(old_text, search)) != std::string::npos)
            {
                ++matches;
                search += old_text.size();
            }
            const S32 expected = (*it).has("expected_matches")
                ? (*it)["expected_matches"].asInteger() : (replace_all ? matches : 1);
            if (matches != expected || (!replace_all && matches != 1))
            {
                writeResult(failureResult(context->request,
                    llformat("Patch precondition failed: expected %d exact match(es), found %d", expected, matches)));
                return;
            }

            if (replace_all)
            {
                std::string::size_type position = 0;
                while ((position = source.find(old_text, position)) != std::string::npos)
                {
                    source.replace(position, old_text.size(), new_text);
                    position += new_text.size();
                    ++applied;
                }
            }
            else
            {
                source.replace(source.find(old_text), old_text.size(), new_text);
                ++applied;
            }
            if (source.size() > MCP_MAX_SCRIPT_SOURCE_BYTES)
            {
                writeResult(failureResult(context->request, "Patched script source exceeds the 256 KiB MCP limit"));
                return;
            }
        }

        LLViewerObject* object = gObjectList.findObject(context->object_id);
        LLViewerInventoryItem* item = findTaskScript(object, context->item_id);
        if (!object || !item || !canModifyScript(item) || !isObjectInSingleSelectedLinkset(object))
        {
            writeResult(failureResult(context->request,
                "Script or selected linkset changed while its source was being loaded"));
            return;
        }
        const std::string url = object->getRegion() ? object->getRegion()->getCapability("UpdateScriptTask") : std::string();
        if (url.empty())
        {
            writeResult(failureResult(context->request, "Region does not provide UpdateScriptTask"));
            return;
        }
        const bool running = context->request.has("running") ? context->request["running"].asBoolean() : true;
        const LLSD request = context->request;
        const LLUUID object_id = context->object_id;
        LLResourceUploadInfo::ptr_t upload_info(std::make_shared<LLScriptAssetUpload>(
            object_id, context->item_id, LLScriptAssetUpload::MONO, running, LLUUID::null, source,
            [request, object_id, running, applied](LLUUID script_item_id, LLUUID, LLUUID new_asset_id, LLSD response)
            {
                const bool compiled = response["compiled"].asBoolean();
                LLSD result = scriptResult(request, compiled, compiled ? "applied" : "compile_failed",
                    compiled ? "Script patched in place; item UUID preserved" : "Simulator rejected the patched script during compilation");
                result["object_id"] = object_id;
                result["item_id"] = script_item_id;
                result["asset_id"] = new_asset_id;
                result["running"] = running;
                result["replacements_applied"] = applied;
                result["compile_response"] = response;
                writeResult(result);
            },
            [request](LLUUID, LLUUID, LLSD response, std::string reason)
            {
                LLSD result = failureResult(request, "In-place script patch failed: " + reason);
                result["upload_response"] = response;
                writeResult(result);
                return true;
            }));
        LLViewerAssetUpload::EnqueueInventoryUpload(url, upload_info);
        return;
    }

    LLSD result = scriptResult(context->request, true, "loaded", "Script source downloaded");
    result["object_id"] = context->object_id;
    result["item_id"] = context->item_id;
    result["asset_id"] = asset_id;
    result["name"] = context->name;
    result["source"] = source;
    writeResult(result);
}

void FSMCPBridge::writeResult(const LLSD& result)
{
    writeAtomicXML(gDirUtilp->add(getBridgeDirectory(), MCP_RESULT_FILE), result);
}

void FSMCPBridge::writeStatus(bool enabled, const std::string& message)
{
    LLSD status;
    status["protocol_version"] = 4;
    status["enabled"] = enabled;
    status["state"] = message;
    status["updated_at"] = LLDate::now();
    status["snapshot_file"] = MCP_SNAPSHOT_FILE;
    status["read_only"] = false;
    status["direct_transform_write"] = true;
    status["script_management"] = true;
    status["script_patch_in_place"] = true;
    status["inventory_management"] = true;
    status["detailed_object_reads"] = true;
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
