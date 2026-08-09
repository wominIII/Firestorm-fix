/**
 * @file fsfloateriaassistant.h
 * @brief AI-assisted diagnostics for avatar attachments and selected objects.
 */

#ifndef FS_FSFLOATERIAIASSISTANT_H
#define FS_FSFLOATERIAIASSISTANT_H

#include "llfloater.h"

#include <functional>

class LLButton;
class LLTextEditor;

class FSAIAssistantService
{
public:
    using success_callback_t = std::function<void(const std::string&)>;
    using failure_callback_t = std::function<void(S32, const std::string&)>;

    static LLSD collectSnapshot();
    static std::string formatSnapshot(const LLSD& snapshot);
    static bool isConfigured();
    static void analyze(const LLSD& snapshot, const std::string& question,
                        success_callback_t success, failure_callback_t failure);

private:
    static void analyzeCoro(LLSD snapshot, std::string question,
                            success_callback_t success, failure_callback_t failure);
};

class FSFloaterAIAssistant final : public LLFloater
{
    friend class LLFloaterReg;

public:
    explicit FSFloaterAIAssistant(const LLSD& key);
    ~FSFloaterAIAssistant() final = default;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void refreshSnapshot();
    void analyzeSnapshot();
    void focusSelectedObject();
    void openBuildTools();
    void setBusy(bool busy);
    void showAnalysis(const std::string& text);
    void showError(S32 status, const std::string& message);

    LLSD mSnapshot;
    LLTextEditor* mSnapshotEditor{ nullptr };
    LLTextEditor* mQuestionEditor{ nullptr };
    LLTextEditor* mAnalysisEditor{ nullptr };
    LLButton* mAnalyzeButton{ nullptr };
};

#endif // FS_FSFLOATERIAIASSISTANT_H
