/**
* @file lltranslate.h
* @brief Human language translation class and JSON response receiver.
*
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLTRANSLATE_H
#define LL_LLTRANSLATE_H

#include "llbufferstream.h"
#include "llsingleton.h"

#include <functional>

class LLTranslationAPIHandler;
/**
 * Entry point for machine translation services.
 *
 * Basically, to translate a string, we need to know the URL
 * of a translation service, have a valid API for the service
 * and be given the target language.
 *
 * Callers specify the string to translate and the target language,
 * LLTranslate takes care of the rest.
 *
 * API keys for translation are taken from saved settings.
 */
class LLTranslate: public LLSingleton<LLTranslate>
{
    LLSINGLETON(LLTranslate);
    ~LLTranslate();
    LOG_CLASS(LLTranslate);

public :

    typedef enum e_service {
        SERVICE_AZURE,
        SERVICE_GOOGLE,
        SERVICE_DEEPL,
    } EService;

    typedef std::function<void(EService, bool, S32)> KeyVerificationResult_fn;
    typedef std::function<void(std::string, std::string)> TranslationSuccess_fn;
    typedef std::function<void(int, std::string)> TranslationFailure_fn;
    typedef std::function<void(LLSD)> ScriptDialogTranslationSuccess_fn;

    enum EOutgoingMode
    {
        OUTGOING_DISABLED = 0,
        OUTGOING_STANDARD = 1,
        OUTGOING_GPT = 2
    };

    /**
     * Translate given text.
     *
     * @param receiver   Object to pass translation result to.
     * @param from_lang  Source language. Leave empty for auto-detection.
     * @param to_lang    Target language.
     * @param mesg       Text to translate.
     */
    static void translateMessage(const std::string &from_lang, const std::string &to_lang, const std::string &mesg, TranslationSuccess_fn success, TranslationFailure_fn failure);

    // Translate arbitrary text through the configured GPT-compatible endpoint,
    // independently of the outgoing-chat translation mode.
    static void translateMessageGPT(const std::string& mesg, const std::string& to_lang,
                                    TranslationSuccess_fn success, TranslationFailure_fn failure);

    // Translate text immediately before it is sent. Context is an array of
    // maps containing "name" and "text" and is used only by GPT mode.
    static void translateOutgoingMessage(const std::string& mesg, const LLSD& context,
                                         TranslationSuccess_fn success, TranslationFailure_fn failure);
    static EOutgoingMode getOutgoingMode();

    static void translateScriptDialog(const std::string& context_key, const std::string& message,
                                      const LLSD& buttons, ScriptDialogTranslationSuccess_fn success,
                                      TranslationFailure_fn failure);
    static void setScriptDialogTranslation(const std::string& context_key,
                                           const std::string& source, const std::string& translation);
    static std::string getScriptDialogTranslation(const std::string& context_key,
                                                  const std::string& source);

    /**
     * Verify given API key of a translation service.
     *
     * @param receiver  Object to pass verification result to.
     * @param key       Key to verify.
     */
    static void verifyKey(EService service, const LLSD &key, KeyVerificationResult_fn fnc);

    /**
     * @return translation target language
     */
    static std::string getTranslateLanguage();

    /**
     * @return true if translation is configured properly.
     */
    static bool isTranslationConfigured();

    static std::string addNoTranslateTags(std::string mesg);
    static std::string removeNoTranslateTags(std::string mesg);

    void logCharsSeen(size_t count);
    void logCharsSent(size_t count);
    void logSuccess(S32 count);
    void logFailure(S32 count);
    LLSD asLLSD() const;
private:
    static void translateOutgoingGPT(const std::string& mesg, const LLSD& context,
                                     TranslationSuccess_fn success, TranslationFailure_fn failure);
    static void translateOutgoingGPTCoro(std::string mesg, LLSD context, std::string target,
                                         TranslationSuccess_fn success, TranslationFailure_fn failure);
    static void translateScriptDialogGPTCoro(std::string context_key, std::string message,
                                             LLSD buttons, ScriptDialogTranslationSuccess_fn success,
                                             TranslationFailure_fn failure);
    static LLTranslationAPIHandler& getPreferredHandler();
    static LLTranslationAPIHandler& getHandler(EService service);

    size_t mCharsSeen;
    size_t mCharsSent;
    S32 mFailureCount;
    S32 mSuccessCount;
};

#endif
