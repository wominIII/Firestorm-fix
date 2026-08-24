/**
 * @file fsfloaterwearablefavorites.cpp
 * @brief Class for the favorite wearables floater
 *
 * $LicenseInfo:firstyear=2018&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2018 Ansariel Hiller @ Second Life
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsfloaterwearablefavorites.h"
#include "fscommon.h"
#include "llappearancemgr.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llfiltereditor.h"
#include "llgesturemgr.h"
#include "llinventoryfunctions.h"
#include "llinventoryobserver.h"
#include "llmenugl.h"
#include "llmenubutton.h"
#include "lltoggleablemenu.h"
#include "llviewercontrol.h"        // for gSavedSettings
#include "llviewermenu.h"           // for gMenuHolder
#include "rlvactions.h"
#include "rlvlocks.h"

// Hello Kokua! Have fun yoinking! ;)

#define FS_WEARABLE_FAVORITES_FOLDER "#Wearable Favorites"

static LLDefaultChildRegistry::Register<FSWearableFavoritesItemsList> r("fs_wearable_favorites_items_list");

static bool isQuickShelfCargo(EDragAndDropType cargo_type, void* cargo_data)
{
    if (cargo_type == DAD_BODYPART || cargo_type == DAD_CLOTHING ||
        cargo_type == DAD_OBJECT || cargo_type == DAD_GESTURE)
    {
        return true;
    }

    if (cargo_type == DAD_LINK && cargo_data)
    {
        LLInventoryItem* dropped_item = static_cast<LLInventoryItem*>(cargo_data);
        LLViewerInventoryItem* linked_item = gInventory.getItem(dropped_item->getLinkedUUID());
        if (linked_item)
        {
            const LLAssetType::EType type = linked_item->getType();
            return type == LLAssetType::AT_BODYPART || type == LLAssetType::AT_CLOTHING ||
                   type == LLAssetType::AT_OBJECT || type == LLAssetType::AT_GESTURE;
        }
    }

    return false;
}

FSWearableFavoritesItemsList::FSWearableFavoritesItemsList(const Params& p)
:   LLWearableItemsList(p)
{
}

bool FSWearableFavoritesItemsList::handleDragAndDrop(S32 x, S32 y, MASK mask,
                                                  bool drop,
                                                  EDragAndDropType cargo_type,
                                                  void* cargo_data,
                                                  EAcceptance* accept,
                                                  std::string& tooltip_msg)
{
    // Scroll folder view if needed.  Never accepts a drag or drop.
    *accept = ACCEPT_NO;
    autoScroll(x, y);

    if (isQuickShelfCargo(cargo_type, cargo_data))
    {
        if (drop)
        {
            LLInventoryItem* item = (LLInventoryItem*)cargo_data;
            if (!mDADSignal.empty())
            {
                mDADSignal(item->getUUID());
            }
        }
        else
        {
            *accept = ACCEPT_YES_SINGLE;
        }
    }

    return true;
}

LLUUID FSFloaterWearableFavorites::sFolderID = LLUUID();

FSFloaterWearableFavorites::FSFloaterWearableFavorites(const LLSD& key)
    : LLFloater(key),
    mItemsList(nullptr),
    mInitialized(false),
    mDADCallbackConnection(),
    mAutoHideCheck(nullptr),
    mHandlingClick(false),
    mShelfCollapsed(false),
    mCollapsePending(false),
    mExpandedWidth(360),
    mExpandedHeight(340)
{
    mCategoriesObserver = new LLInventoryCategoriesObserver();
}

FSFloaterWearableFavorites::~FSFloaterWearableFavorites()
{
    if (gInventory.containsObserver(mCategoriesObserver))
    {
        gInventory.removeObserver(mCategoriesObserver);
    }
    delete mCategoriesObserver;

    if (mDADCallbackConnection.connected())
    {
        mDADCallbackConnection.disconnect();
    }

    if (mOptionsMenuHandle.get())
    {
        mOptionsMenuHandle.get()->die();
    }
}

//virtual
bool FSFloaterWearableFavorites::postBuild()
{
    mItemsList = getChild<FSWearableFavoritesItemsList>("favorites_list");
    mItemsList->setNoFilteredItemsMsg(getString("search_no_items"));
    mItemsList->setCommitCallback(boost::bind(&FSFloaterWearableFavorites::onItemClicked, this));
    mDADCallbackConnection = mItemsList->setDADCallback(boost::bind(&FSFloaterWearableFavorites::onItemDAD, this, _1));

    mRemoveItemBtn = getChild<LLButton>("remove_btn");
    mRemoveItemBtn->setCommitCallback(boost::bind(&FSFloaterWearableFavorites::handleRemove, this));

    mFilterEditor = getChild<LLFilterEditor>("wearable_filter_input");
    mFilterEditor->setCommitCallback(boost::bind(&FSFloaterWearableFavorites::onFilterEdit, this, _2));

    // Create menus.
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;

    registrar.add("FavWearables.Action",                boost::bind(&FSFloaterWearableFavorites::onOptionsMenuItemClicked, this, _2));
    enable_registrar.add("FavWearables.CheckAction",    boost::bind(&FSFloaterWearableFavorites::onOptionsMenuItemChecked, this, _2));

    mOptionsButton = getChild<LLMenuButton>("options_btn");
    mAutoHideCheck = getChild<LLCheckBoxCtrl>("auto_hide_check");
    mExpandedWidth = getRect().getWidth();
    mExpandedHeight = getRect().getHeight();

    if (LLToggleableMenu* options_menu = LLUICtrlFactory::getInstance()->createFromFile<LLToggleableMenu>("menu_fs_wearable_favorites.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance()); options_menu)
    {
        mOptionsMenuHandle = options_menu->getHandle();
        mOptionsButton->setMenu(options_menu, LLMenuButton::MP_BOTTOM_LEFT);
    }

    return true;
}

//virtual
void FSFloaterWearableFavorites::onOpen(const LLSD& /*info*/)
{
    if (!mInitialized)
    {
        if (sFolderID.isNull())
        {
            initCategory(boost::bind(&FSFloaterWearableFavorites::initialize, this));
        }
        else
        {
            initialize();
        }
    }

    setShelfCollapsed(true);
}

void FSFloaterWearableFavorites::initialize()
{
    LLViewerInventoryCategory* category = gInventory.getCategory(sFolderID);
    if (!category)
    {
        return;
    }

    const LLUUID cof = gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT);
    LLViewerInventoryCategory* category_cof = gInventory.getCategory(cof);
    if (!category_cof)
    {
        return;
    }

    gInventory.addObserver(mCategoriesObserver);
    mCategoriesObserver->addCategory(sFolderID, boost::bind(&FSFloaterWearableFavorites::updateList, this, sFolderID));
    mCategoriesObserver->addCategory(cof, boost::bind(&FSFloaterWearableFavorites::updateList, this, sFolderID));
    category->fetch();

    mItemsList->setSortOrder((LLWearableItemsList::ESortOrder)gSavedSettings.getU32("FSWearableFavoritesSortOrder"));
    updateList(sFolderID);

    mInitialized = true;
}

//virtual
void FSFloaterWearableFavorites::draw()
{
    // Floaters can be repositioned by screen-fit/layout passes after a display
    // mode, UI scale or toolbar change.  Keep the shelf bottom-centred even
    // when its expanded/collapsed state itself did not change.
    setShelfCollapsed(mShelfCollapsed);

    if (mCollapsePending && mCollapseTimer.getElapsedTimeF32() >= 0.45f)
    {
        mCollapsePending = false;
        if (mAutoHideCheck && mAutoHideCheck->getValue().asBoolean())
        {
            setShelfCollapsed(true);
        }
    }

    LLFloater::draw();

    mRemoveItemBtn->setEnabled(mLastActivatedItem.notNull());
}

bool FSFloaterWearableFavorites::handleHover(S32 x, S32 y, MASK mask)
{
    mCollapsePending = false;
    if (mShelfCollapsed)
    {
        setShelfCollapsed(false);
    }
    return LLFloater::handleHover(x, y, mask);
}

bool FSFloaterWearableFavorites::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                                   EDragAndDropType cargo_type, void* cargo_data,
                                                   EAcceptance* accept, std::string& tooltip_msg)
{
    if (!isQuickShelfCargo(cargo_type, cargo_data))
    {
        return LLFloater::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
    }

    // Dragging from inventory makes the drawer lose hover and collapse. Treat
    // the home indicator itself as a drop target and expand as soon as cargo
    // reaches it, so users never need to hit the inner list precisely.
    mCollapsePending = false;
    if (mShelfCollapsed)
    {
        setShelfCollapsed(false);
    }

    *accept = ACCEPT_YES_SINGLE;
    if (drop && cargo_data)
    {
        LLInventoryItem* item = static_cast<LLInventoryItem*>(cargo_data);
        onItemDAD(item->getUUID());
    }
    return true;
}

void FSFloaterWearableFavorites::onMouseLeave(S32 x, S32 y, MASK mask)
{
    LLFloater::onMouseLeave(x, y, mask);
    if (mAutoHideCheck && mAutoHideCheck->getValue().asBoolean() && !mShelfCollapsed)
    {
        mCollapsePending = true;
        mCollapseTimer.reset();
    }
}

void FSFloaterWearableFavorites::setShelfCollapsed(bool collapsed)
{
    const LLRect parent_rect = getParent() ? getParent()->getRect() : getRect();
    const S32 width = collapsed ? 132 : mExpandedWidth;
    // UI scaling can make a fixed 340px floater taller than the usable view.
    // Leave a generous top margin so this drawer never displaces top bars.
    const S32 maximum_expanded_height = llmax(180, parent_rect.getHeight() - 110);
    const S32 height = collapsed ? 12 : llmin(mExpandedHeight, maximum_expanded_height);
    const S32 left = llmax(0, (parent_rect.getWidth() - width) / 2);
    const S32 bottom = 8;
    LLRect rect(left, bottom + height, left + width, bottom);

    if (collapsed != mShelfCollapsed)
    {
        getChildView("drawer_panel")->setVisible(!collapsed);
        getChildView("home_indicator")->setVisible(collapsed);
    }

    if (getRect() != rect)
    {
        setShape(rect, true);
    }
    mShelfCollapsed = collapsed;
}

//virtual
bool FSFloaterWearableFavorites::handleKeyHere(KEY key, MASK mask)
{
    if (FSCommon::isFilterEditorKeyCombo(key, mask))
    {
        mFilterEditor->setFocus(true);
        return true;
    }

    return LLFloater::handleKeyHere(key, mask);
}

// static
std::optional<LLUUID> FSFloaterWearableFavorites::getWearableFavoritesFolderID()
{
    if (LLUUID fs_root_cat_id = gInventory.findCategoryByName(ROOT_FIRESTORM_FOLDER); !fs_root_cat_id.isNull())
    {
        LLInventoryModel::item_array_t* items;
        LLInventoryModel::cat_array_t* cats;
        gInventory.getDirectDescendentsOf(fs_root_cat_id, cats, items);
        if (cats)
        {
            for (const auto& cat : *cats)
            {
                if (cat->getName() == FS_WEARABLE_FAVORITES_FOLDER)
                {
                    return cat->getUUID();
                }
            }
        }
    }

    return std::nullopt;
}

// static
void FSFloaterWearableFavorites::initCategory(inventory_func_type callback)
{
    if (!gInventory.isInventoryUsable())
    {
        LL_WARNS() << "Cannot initialize Favorite Wearables inventory folder - inventory is not usable!" << LL_ENDL;
        return;
    }

    if (auto fs_favs_id = getWearableFavoritesFolderID(); fs_favs_id.has_value())
    {
        sFolderID = fs_favs_id.value();
        callback(sFolderID);
    }
    else
    {
        LLUUID fs_root_cat_id = gInventory.findCategoryByName(ROOT_FIRESTORM_FOLDER);
        if (fs_root_cat_id.isNull())
        {
            gInventory.createNewCategory(gInventory.getRootFolderID(), LLFolderType::FT_NONE, ROOT_FIRESTORM_FOLDER, [callback](const LLUUID& new_cat_id)
            {
                gInventory.createNewCategory(new_cat_id, LLFolderType::FT_NONE, FS_WEARABLE_FAVORITES_FOLDER, [callback](const LLUUID& new_cat_id)
                {
                    FSFloaterWearableFavorites::sFolderID = new_cat_id;
                    callback(new_cat_id);
                });
            });
        }
        else
        {
            gInventory.createNewCategory(fs_root_cat_id, LLFolderType::FT_NONE, FS_WEARABLE_FAVORITES_FOLDER, [callback](const LLUUID& new_cat_id)
            {
                FSFloaterWearableFavorites::sFolderID = new_cat_id;
                callback(new_cat_id);
            });
        }
    }
}

//static
LLUUID FSFloaterWearableFavorites::getFavoritesFolder()
{
    if (!sFolderID.isNull())
    {
        if (auto fs_favs_id = getWearableFavoritesFolderID(); fs_favs_id.has_value())
        {
            sFolderID = fs_favs_id.value();
        }
    }

    return sFolderID;
}

void FSFloaterWearableFavorites::updateList(const LLUUID& folder_id)
{
    mItemsList->updateList(folder_id);

    if (gInventory.isCategoryComplete(folder_id))
    {
        mItemsList->setNoItemsCommentText(getString("empty_list")); // Have to reset it here because LLWearableItemsList::updateList might override it
    }
}

void FSFloaterWearableFavorites::onItemDAD(const LLUUID& item_id)
{
    LLViewerInventoryItem* dropped_item = gInventory.getItem(item_id);
    const LLUUID target_id = dropped_item ? dropped_item->getLinkedUUID() : item_id;

    if (sFolderID.notNull())
    {
        link_inventory_object(sFolderID, target_id, LLPointer<LLInventoryCallback>(nullptr));
        return;
    }

    // The first opening creates the backing category asynchronously. The old
    // code accepted a drop before its callback was connected and then lost it.
    initCategory([target_id](const LLUUID& folder_id)
    {
        if (folder_id.notNull())
        {
            link_inventory_object(folder_id, target_id, LLPointer<LLInventoryCallback>(nullptr));
        }
    });
}

void FSFloaterWearableFavorites::handleRemove()
{
    if (mLastActivatedItem.notNull())
    {
        remove_inventory_item(mLastActivatedItem, LLPointer<LLInventoryCallback>(nullptr));
        mLastActivatedItem.setNull();
    }
}

void FSFloaterWearableFavorites::onFilterEdit(const std::string& search_string)
{
    mItemsList->setFilterSubString(search_string, true);
    mItemsList->setNoItemsCommentText(getString("empty_list"));
    mItemsList->rearrange();
}

void FSFloaterWearableFavorites::onOptionsMenuItemClicked(const LLSD& userdata)
{
    const std::string action = userdata.asString();

    if (action == "sort_by_name")
    {
        mItemsList->setSortOrder(LLWearableItemsList::E_SORT_BY_NAME);
        gSavedSettings.setU32("FSWearableFavoritesSortOrder", LLWearableItemsList::E_SORT_BY_NAME);
    }
    else if (action == "sort_by_most_recent")
    {
        mItemsList->setSortOrder(LLWearableItemsList::E_SORT_BY_MOST_RECENT);
        gSavedSettings.setU32("FSWearableFavoritesSortOrder", LLWearableItemsList::E_SORT_BY_MOST_RECENT);
    }
    else if (action == "sort_by_type_name")
    {
        mItemsList->setSortOrder(LLWearableItemsList::E_SORT_BY_TYPE_NAME);
        gSavedSettings.setU32("FSWearableFavoritesSortOrder", LLWearableItemsList::E_SORT_BY_TYPE_NAME);
    }
}

bool FSFloaterWearableFavorites::onOptionsMenuItemChecked(const LLSD& userdata)
{
    const std::string action = userdata.asString();

    if (action == "sort_by_name")
    {
        return mItemsList->getSortOrder() == LLWearableItemsList::E_SORT_BY_NAME;
    }
    else if (action == "sort_by_most_recent")
    {
        return mItemsList->getSortOrder() == LLWearableItemsList::E_SORT_BY_MOST_RECENT;
    }
    else if (action == "sort_by_type_name")
    {
        return mItemsList->getSortOrder() == LLWearableItemsList::E_SORT_BY_TYPE_NAME;
    }

    return false;
}

void FSFloaterWearableFavorites::onItemClicked()
{
    if (mHandlingClick)
    {
        return;
    }

    const LLUUID selected_item_id = mItemsList->getSelectedUUID();
    if (selected_item_id.isNull())
    {
        return;
    }

    mHandlingClick = true;
    mLastActivatedItem = selected_item_id;
    toggleItem(selected_item_id);
    // Clear selection so clicking the same shortcut again emits another commit.
    mItemsList->resetSelection();
    mHandlingClick = false;
}

void FSFloaterWearableFavorites::toggleItem(const LLUUID& link_id)
{
    LLViewerInventoryItem* link_item = gInventory.getItem(link_id);
    LLViewerInventoryItem* item = link_item ? gInventory.getItem(link_item->getLinkedUUID()) : nullptr;
    if (!item)
    {
        return;
    }

    const LLUUID item_id = item->getUUID();
    const LLAssetType::EType asset_type = item->getType();

    if (asset_type == LLAssetType::AT_GESTURE)
    {
        if (LLGestureMgr::instance().isGestureActive(item_id))
        {
            LLGestureMgr::instance().deactivateGesture(item_id);
        }
        else
        {
            LLGestureMgr::instance().activateGesture(item_id);
        }
        return;
    }

    uuid_vec_t ids;
    ids.push_back(item_id);

    if (get_is_item_worn(item_id))
    {
        if ((asset_type == LLAssetType::AT_CLOTHING && (!RlvActions::isRlvEnabled() || gRlvWearableLocks.canRemove(item))) ||
            (asset_type == LLAssetType::AT_OBJECT && (!RlvActions::isRlvEnabled() || gRlvAttachmentLocks.canDetach(item))))
        {
            LLAppearanceMgr::instance().removeItemsFromAvatar(ids);
        }
    }
    else
    {
        if (asset_type == LLAssetType::AT_BODYPART && (!RlvActions::isRlvEnabled() || (gRlvWearableLocks.canWear(item) & RLV_WEAR_REPLACE) == RLV_WEAR_REPLACE))
        {
            wear_multiple(ids, true);
        }
        else if (asset_type == LLAssetType::AT_CLOTHING && LLAppearanceMgr::instance().canAddWearables(ids) && (!RlvActions::isRlvEnabled() || (gRlvWearableLocks.canWear(item) & RLV_WEAR_ADD) == RLV_WEAR_ADD))
        {
            wear_multiple(ids, false);
        }
        else if (asset_type == LLAssetType::AT_OBJECT && LLAppearanceMgr::instance().canAddWearables(ids) && (!RlvActions::isRlvEnabled() || (gRlvAttachmentLocks.canAttach(item) & RLV_WEAR_ADD) == RLV_WEAR_ADD))
        {
            wear_multiple(ids, false);
        }
    }
}
