/** @file fsfloaterrlvremotebrowser.cpp */
#include "llviewerprecompiledheaders.h"

#include "fsfloaterrlvremotebrowser.h"
#include "fsrlvremotebridge.h"

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llclipboard.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "llworld.h"

namespace
{
std::string statusLabel(const std::string& status)
{
    if (status == "controller_missing") return "未发现控制器：请复制脚本，放入物体后佩戴";
    if (status == "controller_ready") return "控制器已连接，请选择附近玩家";
    if (status == "pending") return "等待对方 RLV Relay 响应…";
    if (status == "authorized") return "已授权，可以浏览并应用远程 #RLV 文件夹";
    if (status == "denied") return "请求被拒绝、超时或对方没有可用 Relay";
    if (status == "action_denied") return "对方 Relay 拒绝了这次文件夹操作";
    if (status.rfind("error:", 0) == 0) return "控制器错误：" + status.substr(6);
    return status;
}

std::string wornLabel(const std::string& worn)
{
    if (worn.size() < 2) return "未知";
    if (worn[0] == '3' || worn[1] == '3') return "全部穿戴";
    if (worn[0] == '2' || worn[1] == '2') return "部分穿戴";
    if (worn[0] == '1' || worn[1] == '1') return "未穿戴";
    return "空";
}
}

FSFloaterRLVRemoteBrowser::FSFloaterRLVRemoteBrowser(const LLSD& key) : LLFloater(key) {}

bool FSFloaterRLVRemoteBrowser::postBuild()
{
    mAvatarList = getChild<LLScrollListCtrl>("avatar_list");
    mFolderList = getChild<LLScrollListCtrl>("folder_list");
    mStatusText = getChild<LLTextBox>("status_text");
    mPathText = getChild<LLTextBox>("path_text");
    getChild<LLButton>("copy_script_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::copyControllerScript, this));
    getChild<LLButton>("refresh_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::refreshAll, this));
    getChild<LLButton>("request_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::requestSelectedAvatar, this));
    getChild<LLButton>("open_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::openSelectedFolder, this));
    getChild<LLButton>("back_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::openParentFolder, this));
    getChild<LLButton>("attach_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::applySelectedFolder, this, "attach"));
    getChild<LLButton>("attach_all_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::applySelectedFolder, this, "attachall"));
    getChild<LLButton>("detach_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::applySelectedFolder, this, "detach"));
    getChild<LLButton>("detach_all_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::applySelectedFolder, this, "detachall"));
    getChild<LLButton>("release_btn")->setClickedCallback(
        boost::bind(&FSFloaterRLVRemoteBrowser::releaseTarget, this));
    mFolderList->setDoubleClickCallback(boost::bind(&FSFloaterRLVRemoteBrowser::openSelectedFolder, this));
    return true;
}

void FSFloaterRLVRemoteBrowser::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    FSRLVRemoteBridge::discoverController();
    refreshAll();
}

void FSFloaterRLVRemoteBrowser::refreshIfOpen()
{
    FSFloaterRLVRemoteBrowser* floater = LLFloaterReg::findTypedInstance<FSFloaterRLVRemoteBrowser>("fs_rlv_remote_browser");
    if (floater && floater->getVisible()) floater->refreshAll();
}

void FSFloaterRLVRemoteBrowser::refreshAll()
{
    refreshAvatars();
    refreshFolders();
    if (mPathText) mPathText->setText(FSRLVRemoteBridge::getCurrentPath().empty()
        ? "远程 RLV / #RLV" : "远程 RLV / #RLV / " + FSRLVRemoteBridge::getCurrentPath());
    const bool controller = FSRLVRemoteBridge::controllerReady();
    if (mStatusText) mStatusText->setText(statusLabel(controller
        ? FSRLVRemoteBridge::getStatus() : "controller_missing"));
    const bool authorized = FSRLVRemoteBridge::targetAuthorized();
    getChild<LLButton>("request_btn")->setEnabled(controller);
    getChild<LLButton>("open_btn")->setEnabled(authorized);
    getChild<LLButton>("back_btn")->setEnabled(authorized && !FSRLVRemoteBridge::getCurrentPath().empty());
    getChild<LLButton>("attach_btn")->setEnabled(authorized);
    getChild<LLButton>("attach_all_btn")->setEnabled(authorized);
    getChild<LLButton>("detach_btn")->setEnabled(authorized);
    getChild<LLButton>("detach_all_btn")->setEnabled(authorized);
    getChild<LLButton>("release_btn")->setEnabled(FSRLVRemoteBridge::getTargetID().notNull());
}

void FSFloaterRLVRemoteBrowser::refreshAvatars()
{
    if (!mAvatarList || !gAgent.isInitialized()) return;
    const LLUUID selected = mAvatarList->getSelectedValue().asUUID();
    mAvatarList->clearRows();
    uuid_vec_t ids;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&ids, &positions, gAgent.getPositionGlobal(), 96.f);
    for (S32 index = 0; index < static_cast<S32>(ids.size()); ++index)
    {
        if (ids[index].isNull() || ids[index] == gAgentID) continue;
        LLAvatarName avatar_name;
        const std::string name = LLAvatarNameCache::get(ids[index], &avatar_name)
            ? avatar_name.getCompleteName() : ids[index].asString();
        LLSD row;
        row["id"] = ids[index];
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = name;
        row["columns"][1]["column"] = "distance";
        row["columns"][1]["value"] = llformat("%.1f m", dist_vec(positions[index], gAgent.getPositionGlobal()));
        row["columns"][2]["column"] = "status";
        row["columns"][2]["value"] = ids[index] == FSRLVRemoteBridge::getTargetID()
            ? statusLabel(FSRLVRemoteBridge::getStatus()) : "未请求";
        LLScrollListItem* item = mAvatarList->addElement(row, ADD_BOTTOM);
        if (ids[index] == selected && item) item->setSelected(true);
    }
}

void FSFloaterRLVRemoteBrowser::refreshFolders()
{
    if (!mFolderList) return;
    const std::string selected = mFolderList->getSelectedValue().asString();
    mFolderList->clearRows();
    const LLSD& folders = FSRLVRemoteBridge::getFolders();
    for (LLSD::array_const_iterator it = folders.beginArray(); it != folders.endArray(); ++it)
    {
        LLSD row;
        row["id"] = (*it)["path"];
        row["columns"][0]["column"] = "worn";
        row["columns"][0]["value"] = wornLabel((*it)["worn"].asString());
        row["columns"][1]["column"] = "folder";
        row["columns"][1]["value"] = (*it)["name"];
        row["columns"][2]["column"] = "path";
        row["columns"][2]["value"] = (*it)["path"];
        LLScrollListItem* item = mFolderList->addElement(row, ADD_BOTTOM);
        if ((*it)["path"].asString() == selected && item) item->setSelected(true);
    }
}

void FSFloaterRLVRemoteBrowser::copyControllerScript()
{
    const LLWString script = utf8str_to_wstring(FSRLVRemoteBridge::getControllerScript());
    LLClipboard::instance().copyToClipboard(script, 0, static_cast<S32>(script.size()));
    if (mStatusText)
        mStatusText->setText(LLStringExplicit("控制器脚本已复制：粘贴到可修改物体的脚本编辑器，保存并佩戴"));
}

void FSFloaterRLVRemoteBrowser::requestSelectedAvatar()
{
    const LLUUID id = mAvatarList ? mAvatarList->getSelectedValue().asUUID() : LLUUID::null;
    if (id.notNull()) FSRLVRemoteBridge::requestAccess(id);
}

void FSFloaterRLVRemoteBrowser::openSelectedFolder()
{
    const std::string path = mFolderList ? mFolderList->getSelectedValue().asString() : std::string();
    if (!path.empty()) FSRLVRemoteBridge::requestFolder(path);
}

void FSFloaterRLVRemoteBrowser::openParentFolder()
{
    std::string path = FSRLVRemoteBridge::getCurrentPath();
    const std::string::size_type slash = path.rfind('/');
    if (slash == std::string::npos) path.clear();
    else path.erase(slash);
    FSRLVRemoteBridge::requestFolder(path);
}

void FSFloaterRLVRemoteBrowser::applySelectedFolder(const std::string& action)
{
    const std::string path = mFolderList ? mFolderList->getSelectedValue().asString() : std::string();
    if (!path.empty()) FSRLVRemoteBridge::applyFolder(path, action);
}

void FSFloaterRLVRemoteBrowser::releaseTarget()
{
    FSRLVRemoteBridge::releaseTarget();
}
