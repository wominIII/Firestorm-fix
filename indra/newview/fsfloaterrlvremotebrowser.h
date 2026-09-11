/** @file fsfloaterrlvremotebrowser.h */
#ifndef FS_FSFLOATERRLVREMOTEBROWSER_H
#define FS_FSFLOATERRLVREMOTEBROWSER_H

#include "llfloater.h"

class LLScrollListCtrl;
class LLTextBox;

class FSFloaterRLVRemoteBrowser final : public LLFloater
{
    friend class LLFloaterReg;
public:
    explicit FSFloaterRLVRemoteBrowser(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    static void refreshIfOpen();

private:
    void refreshAll();
    void refreshAvatars();
    void refreshFolders();
    void copyControllerScript();
    void requestSelectedAvatar();
    void openSelectedFolder();
    void openParentFolder();
    void applySelectedFolder(const std::string& action);
    void releaseTarget();

    LLScrollListCtrl* mAvatarList{ nullptr };
    LLScrollListCtrl* mFolderList{ nullptr };
    LLTextBox* mStatusText{ nullptr };
    LLTextBox* mPathText{ nullptr };
};

#endif // FS_FSFLOATERRLVREMOTEBROWSER_H
