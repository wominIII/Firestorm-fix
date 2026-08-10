/** @file fsfloaterxtoys.h */
#ifndef FS_FSFLOATERXTOYS_H
#define FS_FSFLOATERXTOYS_H

#include "llfloater.h"

class LLButton;
class LLFloaterReg;
class LLLineEditor;

class FSFloaterXToys final : public LLFloater
{
    friend class LLFloaterReg;
public:
    explicit FSFloaterXToys(const LLSD& key);
    bool postBuild() override;
private:
    void saveSettings();
    void sendTest();
    void sendStop();
    LLLineEditor* mWebhookId{ nullptr };
    LLLineEditor* mWebhookToken{ nullptr };
};

#endif // FS_FSFLOATERXTOYS_H
