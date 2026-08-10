/** @file fsfloaterxtoys.cpp */
#include "llviewerprecompiledheaders.h"
#include "fsfloaterxtoys.h"
#include "fsxtoysbridge.h"
#include "llbutton.h"
#include "lllineeditor.h"
#include "llviewercontrol.h"

FSFloaterXToys::FSFloaterXToys(const LLSD& key) : LLFloater(key) {}

bool FSFloaterXToys::postBuild()
{
    mWebhookId = getChild<LLLineEditor>("webhook_id");
    mWebhookToken = getChild<LLLineEditor>("webhook_token");
    mWebhookId->setText(gSavedSettings.getString("FSXToysWebhookId"));
    mWebhookToken->setText(gSavedSettings.getString("FSXToysWebhookToken"));
    mWebhookId->setCommitCallback(boost::bind(&FSFloaterXToys::saveSettings, this));
    mWebhookToken->setCommitCallback(boost::bind(&FSFloaterXToys::saveSettings, this));
    getChild<LLButton>("test_btn")->setClickedCallback(boost::bind(&FSFloaterXToys::sendTest, this));
    getChild<LLButton>("stop_btn")->setClickedCallback(boost::bind(&FSFloaterXToys::sendStop, this));
    return true;
}

void FSFloaterXToys::saveSettings()
{
    gSavedSettings.setString("FSXToysWebhookId", mWebhookId->getText());
    gSavedSettings.setString("FSXToysWebhookToken", mWebhookToken->getText());
}

void FSFloaterXToys::sendTest()
{
    saveSettings();
    FSXToysBridge::sendTest();
}

void FSFloaterXToys::sendStop()
{
    saveSettings();
    FSXToysBridge::sendStop();
}
