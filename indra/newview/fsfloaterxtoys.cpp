/** @file fsfloaterxtoys.cpp */
#include "llviewerprecompiledheaders.h"
#include "fsfloaterxtoys.h"
#include "fsxtoysbridge.h"
#include "llagent.h"
#include "llaudioengine.h"
#include "llbutton.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltexteditor.h"
#include "llviewercontrol.h"

#include <set>

namespace
{
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
}

FSFloaterXToys::FSFloaterXToys(const LLSD& key) : LLFloater(key) {}

bool FSFloaterXToys::postBuild()
{
    mWebhookId = getChild<LLLineEditor>("webhook_id");
    mWebhookToken = getChild<LLLineEditor>("webhook_token");
    mChatKeywords = getChild<LLTextEditor>("chat_keywords");
    mChatAllowedSenders = getChild<LLTextEditor>("chat_allowed_senders");
    mSoundList = getChild<LLScrollListCtrl>("sound_list");
    mWebhookId->setText(gSavedSettings.getString("FSXToysWebhookId"));
    mWebhookToken->setText(gSavedSettings.getString("FSXToysWebhookToken"));
    mChatKeywords->setText(gSavedSettings.getString("FSXToysChatKeywords"));
    mChatAllowedSenders->setText(gSavedSettings.getString("FSXToysChatAllowedSenders"));
    mWebhookId->setCommitCallback(boost::bind(&FSFloaterXToys::saveSettings, this));
    mWebhookToken->setCommitCallback(boost::bind(&FSFloaterXToys::saveSettings, this));
    mChatKeywords->setCommitCallback(boost::bind(&FSFloaterXToys::saveSettings, this));
    mChatAllowedSenders->setCommitCallback(boost::bind(&FSFloaterXToys::saveSettings, this));
    getChild<LLButton>("test_btn")->setClickedCallback(boost::bind(&FSFloaterXToys::sendTest, this));
    getChild<LLButton>("stop_btn")->setClickedCallback(boost::bind(&FSFloaterXToys::sendStop, this));
    getChild<LLButton>("refresh_sounds_btn")->setClickedCallback(boost::bind(&FSFloaterXToys::refreshSounds, this));
    getChild<LLButton>("preview_sound_btn")->setClickedCallback(boost::bind(&FSFloaterXToys::previewSelectedSound, this));
    mSoundList->setCommitCallback(boost::bind(&FSFloaterXToys::saveSoundSelection, this));
    refreshSounds();
    return true;
}

void FSFloaterXToys::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    refreshSounds();
}

void FSFloaterXToys::saveSettings()
{
    gSavedSettings.setString("FSXToysWebhookId", mWebhookId->getText());
    gSavedSettings.setString("FSXToysWebhookToken", mWebhookToken->getText());
    gSavedSettings.setString("FSXToysChatKeywords", mChatKeywords->getText());
    gSavedSettings.setString("FSXToysChatAllowedSenders", mChatAllowedSenders->getText());
}

void FSFloaterXToys::refreshSounds()
{
    if (!mSoundList) return;
    FSXToysBridge::refreshAvatarSoundCandidates();
    const std::set<LLUUID> selected = selectedSoundIds();
    mSoundList->clearRows();
    const LLSD candidates = FSXToysBridge::getAvatarSoundCandidates();
    for (LLSD::array_const_iterator it = candidates.beginArray(); it != candidates.endArray(); ++it)
    {
        const LLUUID sound_id((*it)["sound_id"].asString());
        if (sound_id.isNull()) continue;
        LLSD row;
        row["id"] = sound_id;
        row["columns"][0]["column"] = "enabled";
        row["columns"][0]["type"] = "checkbox";
        row["columns"][0]["value"] = selected.count(sound_id) != 0;
        row["columns"][1]["column"] = "sound_name";
        row["columns"][1]["value"] = (*it)["sound_name"];
        row["columns"][2]["column"] = "attachment_name";
        row["columns"][2]["value"] = (*it)["attachment_name"];
        row["columns"][3]["column"] = "sound_id";
        row["columns"][3]["value"] = sound_id.asString();
        mSoundList->addElement(row, ADD_BOTTOM);
    }
}

void FSFloaterXToys::saveSoundSelection()
{
    if (!mSoundList) return;
    std::string value;
    for (LLScrollListItem* row : mSoundList->getAllData())
    {
        if (!row || !row->getColumn(0)->getValue().asBoolean()) continue;
        const LLUUID sound_id(row->getValue().asString());
        if (sound_id.isNull()) continue;
        if (!value.empty()) value += ',';
        value += sound_id.asString();
    }
    gSavedSettings.setString("FSXToysSelectedSoundIds", value);
}

void FSFloaterXToys::previewSelectedSound()
{
    if (!mSoundList || !gAudiop) return;
    const LLScrollListItem* row = mSoundList->getFirstSelected();
    const LLUUID sound_id(row ? row->getValue().asString() : std::string());
    if (sound_id.isNull()) return;

    if (mPreviewAudioSourceId.notNull())
    {
        if (LLAudioSource* source = gAudiop->findAudioSource(mPreviewAudioSourceId);
            source && !source->isDone())
        {
            source->play(LLUUID::null);
        }
    }
    mPreviewAudioSourceId = LLUUID::generateNewID();
    gAudiop->triggerSound(sound_id, gAgentID, 1.f, LLAudioEngine::AUDIO_TYPE_UI,
                         LLVector3d::zero, LLUUID::null, mPreviewAudioSourceId);
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
