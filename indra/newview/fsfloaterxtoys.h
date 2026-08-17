/** @file fsfloaterxtoys.h */
#ifndef FS_FSFLOATERXTOYS_H
#define FS_FSFLOATERXTOYS_H

#include "llfloater.h"

class LLButton;
class LLFloaterReg;
class LLLineEditor;
class LLScrollListCtrl;

class FSFloaterXToys final : public LLFloater
{
    friend class LLFloaterReg;
public:
    explicit FSFloaterXToys(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
private:
    void saveSettings();
    void refreshSounds();
    void saveSoundSelection();
    void previewSelectedSound();
    void sendTest();
    void sendStop();
    LLLineEditor* mWebhookId{ nullptr };
    LLLineEditor* mWebhookToken{ nullptr };
    LLScrollListCtrl* mSoundList{ nullptr };
    LLUUID mPreviewAudioSourceId;
};

#endif // FS_FSFLOATERXTOYS_H
