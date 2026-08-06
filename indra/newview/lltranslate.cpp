/**
* @file lltranslate.cpp
* @brief Functions for translating text via Google Translate.
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

#include "llviewerprecompiledheaders.h"

#include "lltranslate.h"

#include <curl/curl.h>

#include "llbufferstream.h"
#include "lltrans.h"
#include "llui.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llcoros.h"
#include "llcorehttputil.h"
#include "lldir.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llurlregistry.h"
#include "stringize.h"

#include <boost/json.hpp>
#include <regex>

static const std::string AZURE_NOTRANSLATE_OPENING_TAG("<div translate=\"no\">");
static const std::string AZURE_NOTRANSLATE_CLOSING_TAG("</div>");

namespace
{
    struct ScriptDialogCachePattern
    {
        std::string key;
        std::vector<std::string> values;
    };

    std::string scriptDialogValuePlaceholder(size_t index)
    {
        return llformat("[[FS_VALUE_%u]]", static_cast<U32>(index));
    }

    ScriptDialogCachePattern makeScriptDialogCachePattern(const std::string& source)
    {
        static const std::regex dynamic_number(R"((?:[0-9]+\.[0-9]+|[0-9]+))");
        ScriptDialogCachePattern pattern;
        size_t previous = 0;
        for (std::sregex_iterator it(source.begin(), source.end(), dynamic_number), end; it != end; ++it)
        {
            pattern.key.append(source, previous, static_cast<size_t>(it->position()) - previous);
            pattern.values.push_back(it->str());
            pattern.key += scriptDialogValuePlaceholder(pattern.values.size() - 1);
            previous = static_cast<size_t>(it->position() + it->length());
        }
        pattern.key.append(source, previous, std::string::npos);
        return pattern;
    }

    std::string applyScriptDialogCachePattern(std::string text,
                                              const ScriptDialogCachePattern& pattern)
    {
        for (size_t i = 0; i < pattern.values.size(); ++i)
        {
            LLStringUtil::replaceString(text, scriptDialogValuePlaceholder(i), pattern.values[i]);
        }
        return text;
    }

    bool makeScriptDialogTranslationTemplate(const ScriptDialogCachePattern& pattern,
                                             const std::string& translation,
                                             std::string& translation_template)
    {
        if (pattern.values.empty())
        {
            translation_template = translation;
            return false;
        }

        bool has_all_placeholders = true;
        for (size_t i = 0; i < pattern.values.size(); ++i)
        {
            if (translation.find(scriptDialogValuePlaceholder(i)) == std::string::npos)
            {
                has_all_placeholders = false;
                break;
            }
        }
        if (has_all_placeholders)
        {
            translation_template = translation;
            return true;
        }

        static const std::regex dynamic_number(R"((?:[0-9]+\.[0-9]+|[0-9]+))");
        std::vector<std::smatch> matches;
        for (std::sregex_iterator it(translation.begin(), translation.end(), dynamic_number), end;
             it != end; ++it)
        {
            matches.push_back(*it);
        }
        if (matches.size() != pattern.values.size())
        {
            translation_template = translation;
            return false;
        }
        for (size_t i = 0; i < matches.size(); ++i)
        {
            if (matches[i].str() != pattern.values[i])
            {
                translation_template = translation;
                return false;
            }
        }

        translation_template.clear();
        size_t previous = 0;
        for (size_t i = 0; i < matches.size(); ++i)
        {
            const size_t position = static_cast<size_t>(matches[i].position());
            translation_template.append(translation, previous, position - previous);
            translation_template += scriptDialogValuePlaceholder(i);
            previous = position + static_cast<size_t>(matches[i].length());
        }
        translation_template.append(translation, previous, std::string::npos);
        return true;
    }

    LLSD& scriptDialogTranslationCache()
    {
        static LLSD cache;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            llifstream file(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS,
                                                           "script_dialog_translations.xml"));
            if (!file.is_open() || LLSDSerialize::fromXML(cache, file) == LLSDParser::PARSE_FAILURE || !cache.isMap())
            {
                cache = LLSD::emptyMap();
            }
        }
        return cache;
    }

    void saveScriptDialogTranslationCache()
    {
        llofstream file(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS,
                                                       "script_dialog_translations.xml"));
        if (file.is_open())
        {
            LLSDSerialize::toPrettyXML(scriptDialogTranslationCache(), file);
        }
    }

    void cacheScriptDialogTranslation(const std::string& context_key, const std::string& source,
                                      const std::string& translation, bool manual)
    {
        if (source.empty() || translation.empty()) return;
        const ScriptDialogCachePattern pattern = makeScriptDialogCachePattern(source);
        std::string translation_template;
        const bool reusable_template = makeScriptDialogTranslationTemplate(pattern, translation,
                                                                            translation_template);
        if (!reusable_template)
        {
            translation_template = applyScriptDialogCachePattern(translation_template, pattern);
        }
        const std::string& cache_key = (!manual && reusable_template) ? pattern.key : source;
        if (manual)
        {
            translation_template = applyScriptDialogCachePattern(translation, pattern);
        }
        LLSD& entry = scriptDialogTranslationCache()[context_key][cache_key];
        if (entry["manual"].asBoolean() && !manual) return;
        entry["text"] = translation_template;
        entry["manual"] = manual;
        saveScriptDialogTranslationCache();
    }

    struct StandardDialogTranslationState
    {
        std::string context_key;
        std::string message;
        LLSD buttons;
        LLSD result;
        S32 remaining{0};
        LLTranslate::ScriptDialogTranslationSuccess_fn success;
        LLTranslate::TranslationFailure_fn failure;
        bool failed{false};
    };

    std::string formatScriptDialogContext(const std::string& message, const LLSD& buttons)
    {
        std::string menu = "Message:\n" + makeScriptDialogCachePattern(message).key + "\nButtons:\n";
        for (S32 i = 0; i < static_cast<S32>(buttons.size()); ++i)
        {
            menu += llformat("%d. %s\n", i + 1,
                             makeScriptDialogCachePattern(buttons[i].asString()).key.c_str());
        }
        return menu;
    }
}

/**
* Handler of an HTTP machine translation service.
*
* Derived classes know the service URL
* and how to parse the translation result.
*/
class LLTranslationAPIHandler
{
public:
    typedef std::pair<std::string, std::string> LanguagePair_t;

    /**
    * Get URL for translation of the given string.
    *
    * Sending HTTP GET request to the URL will initiate translation.
    *
    * @param[out] url        Place holder for the result.
    * @param      from_lang  Source language. Leave empty for auto-detection.
    * @param      to_lang    Target language.
    * @param      text       Text to translate.
    */
    virtual std::string getTranslateURL(
        const std::string &from_lang,
        const std::string &to_lang,
        const std::string &text) const = 0;

    /**
    * Get URL to verify the given API key.
    *
    * Sending request to the URL verifies the key.
    * Positive HTTP response (code 200) means that the key is valid.
    *
    * @param[out] url  Place holder for the URL.
    * @param[in]  key  Key to verify.
    */
    virtual std::string getKeyVerificationURL(
        const LLSD &key) const = 0;

    /**
    * Check API verification response.
    *
    * @param[out] bool  true if valid.
    * @param[in]  response
    * @param[in]  status
    */
    virtual bool checkVerificationResponse(
        const LLSD &response,
        int status) const = 0;

    /**
    * Parse translation response.
    *
    * @param[in,out] status        HTTP status. May be modified while parsing.
    * @param         body          Response text.
    * @param[out]    translation   Translated text.
    * @param[out]    detected_lang Detected source language. May be empty.
    * @param[out]    err_msg       Error message (in case of error).
    */
    virtual bool parseResponse(
        const LLSD& http_response,
        int& status,
        const std::string& body,
        std::string& translation,
        std::string& detected_lang,
        std::string& err_msg) const = 0;

    /**
    * @return if the handler is configured to function properly
    */
    virtual bool isConfigured() const = 0;

    virtual LLTranslate::EService getCurrentService() = 0;

    virtual void verifyKey(const LLSD &key, LLTranslate::KeyVerificationResult_fn fnc) = 0;
    virtual void translateMessage(LanguagePair_t fromTo, std::string msg, LLTranslate::TranslationSuccess_fn success, LLTranslate::TranslationFailure_fn failure);


    virtual ~LLTranslationAPIHandler() {}

    void verifyKeyCoro(LLTranslate::EService service, LLSD key, LLTranslate::KeyVerificationResult_fn fnc);
    void translateMessageCoro(LanguagePair_t fromTo, std::string msg, LLTranslate::TranslationSuccess_fn success, LLTranslate::TranslationFailure_fn failure);

    virtual void initHttpHeader(LLCore::HttpHeaders::ptr_t headers, const std::string& user_agent) const = 0;
    virtual void initHttpHeader(LLCore::HttpHeaders::ptr_t headers, const std::string& user_agent, const LLSD &key) const = 0;
    virtual LLSD sendMessageAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
                                       LLCore::HttpRequest::ptr_t request,
                                       LLCore::HttpOptions::ptr_t options,
                                       LLCore::HttpHeaders::ptr_t headers,
                                       const std::string & url,
                                       const std::string & msg,
                                       const std::string& from_lang,
                                       const std::string& to_lang) const = 0;
    virtual LLSD verifyAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
        LLCore::HttpRequest::ptr_t request,
        LLCore::HttpOptions::ptr_t options,
        LLCore::HttpHeaders::ptr_t headers,
        const std::string & url) const = 0;
};

void LLTranslationAPIHandler::translateMessage(LanguagePair_t fromTo, std::string msg, LLTranslate::TranslationSuccess_fn success, LLTranslate::TranslationFailure_fn failure)
{
    LLCoros::instance().launch("Translation", boost::bind(&LLTranslationAPIHandler::translateMessageCoro,
        this, fromTo, msg, success, failure));

}

void LLTranslationAPIHandler::verifyKeyCoro(LLTranslate::EService service, LLSD key, LLTranslate::KeyVerificationResult_fn fnc)
{
    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t
        httpAdapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("verifyKeyCoro", httpPolicy);
    LLCore::HttpRequest::ptr_t httpRequest = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t httpOpts = std::make_shared<LLCore::HttpOptions>();
    LLCore::HttpHeaders::ptr_t httpHeaders = std::make_shared<LLCore::HttpHeaders>();


    std::string user_agent = stringize(
        LLVersionInfo::instance().getChannel(), ' ',
        LLVersionInfo::instance().getMajor(), '.',
        LLVersionInfo::instance().getMinor(), '.',
        LLVersionInfo::instance().getPatch(), " (",
        LLVersionInfo::instance().getBuild(), ')');

    initHttpHeader(httpHeaders, user_agent, key);

    httpOpts->setFollowRedirects(true);
    httpOpts->setSSLVerifyPeer(false);

    std::string url = this->getKeyVerificationURL(key);
    if (url.empty())
    {
        LL_INFOS("Translate") << "No translation URL" << LL_ENDL;
        return;
    }

    std::string::size_type delim_pos = url.find("://");
    if (delim_pos == std::string::npos)
    {
        LL_INFOS("Translate") << "URL is missing a scheme" << LL_ENDL;
        return;
    }

    LLSD result = verifyAndSuspend(httpAdapter, httpRequest, httpOpts, httpHeaders, url);

    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

    bool bOk = true;
    int parseResult = status.getType();
    if (!checkVerificationResponse(httpResults, parseResult))
    {
        bOk = false;
    }

    if (fnc != nullptr)
    {
        fnc(service, bOk, parseResult);
    }
}

void LLTranslationAPIHandler::translateMessageCoro(LanguagePair_t fromTo, std::string msg,
    LLTranslate::TranslationSuccess_fn success, LLTranslate::TranslationFailure_fn failure)
{
    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t
        httpAdapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("translateMessageCoro", httpPolicy);
    LLCore::HttpRequest::ptr_t httpRequest = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t httpOpts = std::make_shared<LLCore::HttpOptions>();
    LLCore::HttpHeaders::ptr_t httpHeaders = std::make_shared<LLCore::HttpHeaders>();


    std::string user_agent = stringize(
        LLVersionInfo::instance().getChannel(), ' ',
        LLVersionInfo::instance().getMajor(), '.',
        LLVersionInfo::instance().getMinor(), '.',
        LLVersionInfo::instance().getPatch(), " (",
        LLVersionInfo::instance().getBuild(), ')');

    initHttpHeader(httpHeaders, user_agent);
    httpOpts->setSSLVerifyPeer(false);

    std::string url = this->getTranslateURL(fromTo.first, fromTo.second, msg);
    if (url.empty())
    {
        LL_INFOS("Translate") << "No translation URL" << LL_ENDL;
        return;
    }

    LLSD result = sendMessageAndSuspend(httpAdapter, httpRequest, httpOpts, httpHeaders, url, msg, fromTo.first, fromTo.second);

    if (LLApp::isQuitting())
    {
        return;
    }

    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

    std::string translation, err_msg;
    std::string detected_lang(fromTo.second);

    int parseResult = status.getType();
    const LLSD::Binary &rawBody = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
    std::string body(rawBody.begin(), rawBody.end());

    bool res = false;

    try
    {
        res = parseResponse(httpResults, parseResult, body, translation, detected_lang, err_msg);
    }
    catch (std::out_of_range&)
    {
        LL_WARNS() << "Out of range exception on string " << body << LL_ENDL;
    }
    catch (...)
    {
        LOG_UNHANDLED_EXCEPTION( "Exception on string " + body );
    }

    if (res)
    {
        // Fix up the response
        LLStringUtil::replaceString(translation, "&lt;", "<");
        LLStringUtil::replaceString(translation, "&gt;", ">");
        LLStringUtil::replaceString(translation, "&quot;", "\"");
        LLStringUtil::replaceString(translation, "&#39;", "'");
        LLStringUtil::replaceString(translation, "&amp;", "&");
        LLStringUtil::replaceString(translation, "&apos;", "'");

        if (success != nullptr)
            success(translation, detected_lang);
    }
    else
    {
        if (err_msg.empty() && httpResults.has("error_body"))
        {
            err_msg = httpResults["error_body"].asString();
        }

        if (err_msg.empty())
        {
            err_msg = LLTrans::getString("TranslationResponseParseError");
        }

        LL_WARNS() << "Translation request failed: " << err_msg << LL_ENDL;
        if (failure != nullptr)
            failure(status, err_msg);
    }
}

//=========================================================================
/// Google Translate v2 API handler.
class LLGoogleTranslationHandler : public LLTranslationAPIHandler
{
    LOG_CLASS(LLGoogleTranslationHandler);

public:
    std::string getTranslateURL(
        const std::string &from_lang,
        const std::string &to_lang,
        const std::string &text) const override;
    std::string getKeyVerificationURL(
        const LLSD &key) const override;
    bool checkVerificationResponse(
        const LLSD &response,
        int status) const override;
    bool parseResponse(
        const LLSD& http_response,
        int& status,
        const std::string& body,
        std::string& translation,
        std::string& detected_lang,
        std::string& err_msg) const override;
    bool isConfigured() const override;

    LLTranslate::EService getCurrentService() override { return LLTranslate::EService::SERVICE_GOOGLE; }

    void verifyKey(const LLSD &key, LLTranslate::KeyVerificationResult_fn fnc) override;

    void initHttpHeader(LLCore::HttpHeaders::ptr_t headers, const std::string& user_agent) const override;
    void initHttpHeader(LLCore::HttpHeaders::ptr_t headers, const std::string& user_agent, const LLSD &key) const override;
    LLSD sendMessageAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
        LLCore::HttpRequest::ptr_t request,
        LLCore::HttpOptions::ptr_t options,
        LLCore::HttpHeaders::ptr_t headers,
        const std::string & url,
        const std::string & msg,
        const std::string& from_lang,
        const std::string& to_lang) const override;

    LLSD verifyAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
        LLCore::HttpRequest::ptr_t request,
        LLCore::HttpOptions::ptr_t options,
        LLCore::HttpHeaders::ptr_t headers,
        const std::string & url) const override;

private:
    static void parseErrorResponse(
        const boost::json::value& root,
        int& status,
        std::string& err_msg);
    static bool parseTranslation(
        const boost::json::value& root,
        std::string& translation,
        std::string& detected_lang);
    static std::string getAPIKey();
};

//-------------------------------------------------------------------------
// virtual
std::string LLGoogleTranslationHandler::getTranslateURL(
    const std::string &from_lang,
    const std::string &to_lang,
    const std::string &text) const
{
    std::string url = std::string("https://www.googleapis.com/language/translate/v2?key=")
        + getAPIKey() + "&q=" + LLURI::escape(text) + "&target=" + to_lang;
    if (!from_lang.empty())
    {
        url += "&source=" + from_lang;
    }
    return url;
}

// virtual
std::string LLGoogleTranslationHandler::getKeyVerificationURL(
    const LLSD& key) const
{
    std::string url = std::string("https://www.googleapis.com/language/translate/v2/languages?key=")
        + key.asString() +"&target=en";
    return url;
}

//virtual
bool LLGoogleTranslationHandler::checkVerificationResponse(
    const LLSD &response,
    int status) const
{
    return status == HTTP_OK;
}

// virtual
bool LLGoogleTranslationHandler::parseResponse(
    const LLSD& http_response,
    int& status,
    const std::string& body,
    std::string& translation,
    std::string& detected_lang,
    std::string& err_msg) const
{
    const std::string& text = !body.empty() ? body : http_response["error_body"].asStringRef();

    boost::system::error_code ec;
    boost::json::value root = boost::json::parse(text, ec);
    if (ec.failed())
    {
        err_msg = ec.what();
        return false;
    }

    if (root.is_object())
    {
        // Request succeeded, extract translation from the XML body.
        if (parseTranslation(root, translation, detected_lang))
            return true;

        // Request failed. Extract error message from the XML body.
        parseErrorResponse(root, status, err_msg);
    }

    return false;
}

// virtual
bool LLGoogleTranslationHandler::isConfigured() const
{
    return !getAPIKey().empty();
}

// static
void LLGoogleTranslationHandler::parseErrorResponse(
    const boost::json::value& root,
    int& status,
    std::string& err_msg)
{
    boost::system::error_code ec;
    auto message = root.find_pointer("/data/message", ec);
    auto code = root.find_pointer("/data/code", ec);
    if (!message || !code)
    {
        return;
    }

    auto message_val = boost::json::try_value_to<std::string>(*message);
    auto code_val = boost::json::try_value_to<int>(*code);
    if (!message_val || !code_val)
    {
        return;
    }

    err_msg = message_val.value();
    status = code_val.value();
}

// static
bool LLGoogleTranslationHandler::parseTranslation(
    const boost::json::value& root,
    std::string& translation,
    std::string& detected_lang)
{
    boost::system::error_code ec;
    auto translated_text = root.find_pointer("/data/translations/0/translatedText", ec);
    if (!translated_text) return false;

    auto text_val = boost::json::try_value_to<std::string>(*translated_text);
    if (!text_val)
    {
        LL_WARNS() << "Failed to parse translation" << text_val.error() << LL_ENDL;
        return false;
    }

    translation = text_val.value();

    auto language = root.find_pointer("/data/translations/0/detectedSourceLanguage", ec);
    if (language)
    {
        auto lang_val = boost::json::try_value_to<std::string>(*language);
        detected_lang = lang_val ? lang_val.value() : "";
    }

    return true;
}

// static
std::string LLGoogleTranslationHandler::getAPIKey()
{
    static LLCachedControl<std::string> google_key(gSavedSettings, "GoogleTranslateAPIKey");
    return google_key;
}

/*virtual*/
void LLGoogleTranslationHandler::verifyKey(const LLSD &key, LLTranslate::KeyVerificationResult_fn fnc)
{
    LLCoros::instance().launch("Google /Verify Key", boost::bind(&LLTranslationAPIHandler::verifyKeyCoro,
        this, LLTranslate::SERVICE_GOOGLE, key, fnc));
}

/*virtual*/
void LLGoogleTranslationHandler::initHttpHeader(LLCore::HttpHeaders::ptr_t headers, const std::string& user_agent) const
{
    headers->append(HTTP_OUT_HEADER_ACCEPT, HTTP_CONTENT_JSON);
    headers->append(HTTP_OUT_HEADER_USER_AGENT, user_agent);
}

/*virtual*/
void LLGoogleTranslationHandler::initHttpHeader(
    LLCore::HttpHeaders::ptr_t headers,
    const std::string& user_agent,
    const LLSD &key) const
{
    initHttpHeader(headers, user_agent);
}

LLSD LLGoogleTranslationHandler::sendMessageAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
    LLCore::HttpRequest::ptr_t request,
    LLCore::HttpOptions::ptr_t options,
    LLCore::HttpHeaders::ptr_t headers,
    const std::string & url,
    const std::string & msg,
    const std::string& from_lang,
    const std::string& to_lang) const
{
    return adapter->getRawAndSuspend(request, url, options, headers);
}

LLSD LLGoogleTranslationHandler::verifyAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
    LLCore::HttpRequest::ptr_t request,
    LLCore::HttpOptions::ptr_t options,
    LLCore::HttpHeaders::ptr_t headers,
    const std::string & url) const
{
    return adapter->getAndSuspend(request, url, options, headers);
}

//=========================================================================
/// Microsoft Translator v2 API handler.
class LLAzureTranslationHandler : public LLTranslationAPIHandler
{
    LOG_CLASS(LLAzureTranslationHandler);

public:
    std::string getTranslateURL(
        const std::string &from_lang,
        const std::string &to_lang,
        const std::string &text) const override;
    std::string getKeyVerificationURL(
        const LLSD &key) const override;
    bool checkVerificationResponse(
        const LLSD &response,
        int status) const override;
    bool parseResponse(
        const LLSD& http_response,
        int& status,
        const std::string& body,
        std::string& translation,
        std::string& detected_lang,
        std::string& err_msg) const override;
    bool isConfigured() const override;

    LLTranslate::EService getCurrentService() override { return LLTranslate::EService::SERVICE_AZURE; }

    void verifyKey(const LLSD &key, LLTranslate::KeyVerificationResult_fn fnc) override;

    void initHttpHeader(LLCore::HttpHeaders::ptr_t headers, const std::string& user_agent) const override;
    void initHttpHeader(LLCore::HttpHeaders::ptr_t headers, const std::string& user_agent, const LLSD &key) const override;
    LLSD sendMessageAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
        LLCore::HttpRequest::ptr_t request,
        LLCore::HttpOptions::ptr_t options,
        LLCore::HttpHeaders::ptr_t headers,
        const std::string & url,
        const std::string & msg,
        const std::string& from_lang,
        const std::string& to_lang) const override;

    LLSD verifyAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
        LLCore::HttpRequest::ptr_t request,
        LLCore::HttpOptions::ptr_t options,
        LLCore::HttpHeaders::ptr_t headers,
        const std::string & url) const override;
private:
    static std::string parseErrorResponse(
        const std::string& body);
    static LLSD getAPIKey();
    static std::string getAPILanguageCode(const std::string& lang);

};

//-------------------------------------------------------------------------
// virtual
std::string LLAzureTranslationHandler::getTranslateURL(
    const std::string &from_lang,
    const std::string &to_lang,
    const std::string &text) const
{
    std::string url;
    LLSD key = getAPIKey();
    if (key.isMap())
    {
        std::string endpoint = key["endpoint"].asString();

        if (*endpoint.rbegin() != '/')
        {
            endpoint += "/";
        }
        url = endpoint + std::string("translate?api-version=3.0&to=")
            + getAPILanguageCode(to_lang);
    }
    return url;
}


// virtual
std::string LLAzureTranslationHandler::getKeyVerificationURL(
    const LLSD& key) const
{
    std::string url;
    if (key.isMap())
    {
        std::string endpoint = key["endpoint"].asString();
        if (*endpoint.rbegin() != '/')
        {
            endpoint += "/";
        }
        url = endpoint + std::string("translate?api-version=3.0&to=en");
    }
    return url;
}

//virtual
bool LLAzureTranslationHandler::checkVerificationResponse(
    const LLSD &response,
    int status) const
{
    if (status == HTTP_UNAUTHORIZED)
    {
        LL_DEBUGS("Translate") << "Key unathorised" << LL_ENDL;
        return false;
    }

    if (status == HTTP_NOT_FOUND)
    {
        LL_DEBUGS("Translate") << "Either endpoint doesn't have requested resource" << LL_ENDL;
        return false;
    }

    if (status != HTTP_BAD_REQUEST)
    {
        LL_DEBUGS("Translate") << "Unexpected error code" << LL_ENDL;
        return false;
    }

    if (!response.has("error_body"))
    {
        LL_DEBUGS("Translate") << "Unexpected response, no error returned" << LL_ENDL;
        return false;
    }

    // Expected: "{\"error\":{\"code\":400000,\"message\":\"One of the request inputs is not valid.\"}}"
    // But for now just verify response is a valid json

    boost::system::error_code ec;
    boost::json::value root = boost::json::parse(response["error_body"].asString(), ec);
    if (ec.failed())
    {
        LL_DEBUGS("Translate") << "Failed to parse error_body:" << ec.what() << LL_ENDL;
        return false;
    }

    return true;
}

// virtual
bool LLAzureTranslationHandler::parseResponse(
    const LLSD& http_response,
    int& status,
    const std::string& body,
    std::string& translation,
    std::string& detected_lang,
    std::string& err_msg) const
{
    if (status != HTTP_OK)
    {
        if (http_response.has("error_body"))
            err_msg = parseErrorResponse(http_response["error_body"].asString());
        return false;
    }

    //Example:
    // "[{\"detectedLanguage\":{\"language\":\"en\",\"score\":1.0},\"translations\":[{\"text\":\"Hello, what is your name?\",\"to\":\"en\"}]}]"

    boost::system::error_code ec;
    boost::json::value root = boost::json::parse(body, ec);
    if (ec.failed())
    {
        err_msg = ec.what();
        return false;
    }
    auto language = root.find_pointer("/0/detectedLanguage/language", ec);
    if (!language) return false;

    auto translated_text = root.find_pointer("/0/translations/0/text", ec);
    if (!translated_text) return false;

    auto lang_val = boost::json::try_value_to<std::string>(*language);
    auto text_val = boost::json::try_value_to<std::string>(*translated_text);
    if (!lang_val || !text_val)
    {
        LL_WARNS() << "Failed to parse translation" << lang_val.error() << text_val.error() << LL_ENDL;
        return false;
    }

    detected_lang = lang_val.value();
    translation = text_val.value();

    return true;
}

// virtual
bool LLAzureTranslationHandler::isConfigured() const
{
    return getAPIKey().isMap();
}

//static
std::string LLAzureTranslationHandler::parseErrorResponse(
    const std::string& body)
{
    // Expected: "{\"error\":{\"code\":400000,\"message\":\"One of the request inputs is not valid.\"}}"
    // But for now just verify response is a valid json with an error

    boost::system::error_code ec;
    boost::json::value root = boost::json::parse(body, ec);
    if (ec.failed())
    {
        return {};
    }

    auto err_msg = root.find_pointer("/error/message", ec);
    if (!err_msg)
    {
        return {};
    }

    auto err_msg_val = boost::json::try_value_to<std::string>(*err_msg);
    if (!err_msg_val)
    {
        return {};
    }
    return err_msg_val.value();
}

// static
LLSD LLAzureTranslationHandler::getAPIKey()
{
    static LLCachedControl<LLSD> azure_key(gSavedSettings, "AzureTranslateAPIKey");
    return azure_key;
}

// static
std::string LLAzureTranslationHandler::getAPILanguageCode(const std::string& lang)
{
    return lang == "zh" ? "zh-CHT" : lang; // treat Chinese as Traditional Chinese
}

/*virtual*/
void LLAzureTranslationHandler::verifyKey(const LLSD &key, LLTranslate::KeyVerificationResult_fn fnc)
{
    LLCoros::instance().launch("Azure /Verify Key", boost::bind(&LLTranslationAPIHandler::verifyKeyCoro,
        this, LLTranslate::SERVICE_AZURE, key, fnc));
}
/*virtual*/
void LLAzureTranslationHandler::initHttpHeader(
    LLCore::HttpHeaders::ptr_t headers,
    const std::string& user_agent) const
{
    initHttpHeader(headers, user_agent, getAPIKey());
}

/*virtual*/
void LLAzureTranslationHandler::initHttpHeader(
    LLCore::HttpHeaders::ptr_t headers,
    const std::string& user_agent,
    const LLSD &key) const
{
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, HTTP_CONTENT_JSON);
    headers->append(HTTP_OUT_HEADER_USER_AGENT, user_agent);

    if (key.has("id"))
    {
        // Token based autorization
        headers->append("Ocp-Apim-Subscription-Key", key["id"].asString());
    }
    if (key.has("region"))
    {
        // ex: "westeurope"
        headers->append("Ocp-Apim-Subscription-Region", key["region"].asString());
    }
}

LLSD LLAzureTranslationHandler::sendMessageAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
    LLCore::HttpRequest::ptr_t request,
    LLCore::HttpOptions::ptr_t options,
    LLCore::HttpHeaders::ptr_t headers,
    const std::string & url,
    const std::string & msg,
    const std::string& from_lang,
    const std::string& to_lang) const
{
    LLCore::BufferArray::ptr_t rawbody(new LLCore::BufferArray);
    LLCore::BufferArrayStream outs(rawbody.get());

    static const std::string allowed_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz "
                                             "0123456789"
                                             "-._~";

    outs << "[{\"text\":\"";
    outs << LLURI::escape(msg, allowed_chars);
    outs << "\"}]";

    return adapter->postRawAndSuspend(request, url, rawbody, options, headers);
}

LLSD LLAzureTranslationHandler::verifyAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
    LLCore::HttpRequest::ptr_t request,
    LLCore::HttpOptions::ptr_t options,
    LLCore::HttpHeaders::ptr_t headers,
    const std::string & url) const
{
    LLCore::BufferArray::ptr_t rawbody(new LLCore::BufferArray);
    LLCore::BufferArrayStream outs(rawbody.get());
    outs << "[{\"intentionally_invalid_400\"}]";

    return adapter->postRawAndSuspend(request, url, rawbody, options, headers);
}

//=========================================================================
/// DeepL Translator API handler.
class LLDeepLTranslationHandler: public LLTranslationAPIHandler
{
    LOG_CLASS(LLDeepLTranslationHandler);

public:
    std::string getTranslateURL(
        const std::string& from_lang,
        const std::string& to_lang,
        const std::string& text) const override;
    std::string getKeyVerificationURL(
        const LLSD& key) const override;
    bool checkVerificationResponse(
        const LLSD& response,
        int status) const override;
    bool parseResponse(
        const LLSD& http_response,
        int& status,
        const std::string& body,
        std::string& translation,
        std::string& detected_lang,
        std::string& err_msg) const override;
    bool isConfigured() const override;

    LLTranslate::EService getCurrentService() override
    {
        return LLTranslate::EService::SERVICE_DEEPL;
    }

    void verifyKey(const LLSD& key, LLTranslate::KeyVerificationResult_fn fnc) override;

    void initHttpHeader(LLCore::HttpHeaders::ptr_t headers, const std::string& user_agent) const override;
    void initHttpHeader(LLCore::HttpHeaders::ptr_t headers, const std::string& user_agent, const LLSD& key) const override;
    LLSD sendMessageAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
                               LLCore::HttpRequest::ptr_t request,
                               LLCore::HttpOptions::ptr_t options,
                               LLCore::HttpHeaders::ptr_t headers,
                               const std::string& url,
                               const std::string& msg,
                               const std::string& from_lang,
                               const std::string& to_lang) const override;

    LLSD verifyAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
                          LLCore::HttpRequest::ptr_t request,
                          LLCore::HttpOptions::ptr_t options,
                          LLCore::HttpHeaders::ptr_t headers,
                          const std::string& url) const override;
private:
    static std::string parseErrorResponse(
        const std::string& body);
    static LLSD getAPIKey();
    static std::string getAPILanguageCode(const std::string& lang);
};

//-------------------------------------------------------------------------
// virtual
std::string LLDeepLTranslationHandler::getTranslateURL(
    const std::string& from_lang,
    const std::string& to_lang,
    const std::string& text) const
{
    std::string url;
    LLSD key = getAPIKey();
    if (key.isMap())
    {
        url = key["domain"].asString();

        if (*url.rbegin() != '/')
        {
            url += "/";
        }
        url += std::string("v2/translate");
    }
    return url;
}


// virtual
std::string LLDeepLTranslationHandler::getKeyVerificationURL(
    const LLSD& key) const
{
    std::string url;
    if (key.isMap())
    {
        url = key["domain"].asString();

        if (*url.rbegin() != '/')
        {
            url += "/";
        }
        url += std::string("v2/translate");
    }
    return url;
}

//virtual
bool LLDeepLTranslationHandler::checkVerificationResponse(
    const LLSD& response,
    int status) const
{
    // Might need to parse body to make sure we got
    // a valid response and not a message
    return status == HTTP_OK;
}

// virtual
bool LLDeepLTranslationHandler::parseResponse(
    const LLSD& http_response,
    int& status,
    const std::string& body,
    std::string& translation,
    std::string& detected_lang,
    std::string& err_msg) const
{
    if (status != HTTP_OK)
    {
        if (http_response.has("error_body"))
            err_msg = parseErrorResponse(http_response["error_body"].asString());
        return false;
    }

    //Example:
    // "{\"translations\":[{\"detected_source_language\":\"EN\",\"text\":\"test\"}]}"

    boost::system::error_code ec;
    boost::json::value root = boost::json::parse(body, ec);
    if (ec.failed())
    {
        err_msg = ec.message();
        return false;
    }

    auto detected_langp = root.find_pointer("/translations/0/detected_source_language", ec);
    if (!detected_langp || ec.failed()) // empty response? should not happen
    {
        err_msg = ec.message();
        return false;
    }

    // Request succeeded, extract translation from the response.
    auto text_valp = root.find_pointer("/translations/0/text", ec);
    if (!text_valp || ec.failed())
    {
        err_msg = ec.message();
        return false;
    }

    auto lang_result = boost::json::try_value_to<std::string>(*detected_langp);
    auto text_result = boost::json::try_value_to<std::string>(*text_valp);
    if (!lang_result || !text_result)
    {
        return false;
    }

    detected_lang = lang_result.value();
    LLStringUtil::toLower(detected_lang);
    translation = text_result.value();

    return true;
}

// virtual
bool LLDeepLTranslationHandler::isConfigured() const
{
    return getAPIKey().isMap();
}

//static
std::string LLDeepLTranslationHandler::parseErrorResponse(
    const std::string& body)
{
    // Example: "{\"message\":\"One of the request inputs is not valid.\"}"
    boost::system::error_code ec;
    boost::json::value root = boost::json::parse(body, ec);
    if (ec.failed())
    {
        return {};
    }

    auto message_ptr = root.find_pointer("/message", ec);
    if (!message_ptr || ec.failed())
    {
        return {};
    }

    auto message_val = boost::json::try_value_to<std::string>(*message_ptr);
    if (!message_val)
        return {};

    return message_val.value();
}

// static
LLSD LLDeepLTranslationHandler::getAPIKey()
{
    static LLCachedControl<LLSD> deepl_key(gSavedSettings, "DeepLTranslateAPIKey");
    return deepl_key;
}

// static
std::string LLDeepLTranslationHandler::getAPILanguageCode(const std::string& lang)
{
    return lang == "zh" ? "zh-CHT" : lang; // treat Chinese as Traditional Chinese
}

/*virtual*/
void LLDeepLTranslationHandler::verifyKey(const LLSD& key, LLTranslate::KeyVerificationResult_fn fnc)
{
    LLCoros::instance().launch("DeepL /Verify Key", boost::bind(&LLTranslationAPIHandler::verifyKeyCoro,
                                                                this, LLTranslate::SERVICE_DEEPL, key, fnc));
}
/*virtual*/
void LLDeepLTranslationHandler::initHttpHeader(
    LLCore::HttpHeaders::ptr_t headers,
    const std::string& user_agent) const
{
    initHttpHeader(headers, user_agent, getAPIKey());
}

/*virtual*/
void LLDeepLTranslationHandler::initHttpHeader(
    LLCore::HttpHeaders::ptr_t headers,
    const std::string& user_agent,
    const LLSD& key) const
{
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/x-www-form-urlencoded");
    headers->append(HTTP_OUT_HEADER_USER_AGENT, user_agent);

    if (key.has("id"))
    {
        std::string authkey = "DeepL-Auth-Key " + key["id"].asString();
        headers->append(HTTP_OUT_HEADER_AUTHORIZATION, authkey);
    }
}

LLSD LLDeepLTranslationHandler::sendMessageAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
                                                      LLCore::HttpRequest::ptr_t request,
                                                      LLCore::HttpOptions::ptr_t options,
                                                      LLCore::HttpHeaders::ptr_t headers,
                                                      const std::string& url,
                                                      const std::string& msg,
                                                      const std::string& from_lang,
                                                      const std::string& to_lang) const
{
    LLCore::BufferArray::ptr_t rawbody(new LLCore::BufferArray);
    LLCore::BufferArrayStream outs(rawbody.get());
    outs << "text=";
    std::string escaped_string = LLURI::escape(msg);
    outs << escaped_string;
    outs << "&target_lang=";
    std::string lang = to_lang;
    LLStringUtil::toUpper(lang);
    outs << lang;

    return adapter->postRawAndSuspend(request, url, rawbody, options, headers);
}

LLSD LLDeepLTranslationHandler::verifyAndSuspend(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter,
                                                 LLCore::HttpRequest::ptr_t request,
                                                 LLCore::HttpOptions::ptr_t options,
                                                 LLCore::HttpHeaders::ptr_t headers,
                                                 const std::string& url) const
{
    LLCore::BufferArray::ptr_t rawbody(new LLCore::BufferArray);
    LLCore::BufferArrayStream outs(rawbody.get());
    outs << "text=&target_lang=EN";

    return adapter->postRawAndSuspend(request, url, rawbody, options, headers);
}

//=========================================================================
LLTranslate::LLTranslate():
    mCharsSeen(0),
    mCharsSent(0),
    mFailureCount(0),
    mSuccessCount(0)
{
}

LLTranslate::~LLTranslate()
{
}

/*static*/
void LLTranslate::translateMessage(const std::string &from_lang, const std::string &to_lang,
    const std::string &mesg, TranslationSuccess_fn success, TranslationFailure_fn failure)
{
    LLTranslationAPIHandler& handler = getPreferredHandler();

    handler.translateMessage(LLTranslationAPIHandler::LanguagePair_t(from_lang, to_lang), addNoTranslateTags(mesg), success, failure);
}

LLTranslate::EIncomingMode LLTranslate::getIncomingMode()
{
    return gSavedSettings.getS32("FSIncomingTranslateMode") == INCOMING_GPT
        ? INCOMING_GPT
        : INCOMING_STANDARD;
}

void LLTranslate::translateIncomingMessage(const std::string& from_lang, const std::string& to_lang,
                                           const std::string& mesg, TranslationSuccess_fn success,
                                           TranslationFailure_fn failure)
{
    if (getIncomingMode() == INCOMING_GPT)
    {
        translateMessageGPT(mesg, to_lang, success, failure);
    }
    else
    {
        translateMessage(from_lang, to_lang, mesg, success, failure);
    }
}

LLTranslate::EOutgoingMode LLTranslate::getOutgoingMode()
{
    S32 mode = gSavedSettings.getS32("FSOutgoingTranslateMode");
    return (mode >= OUTGOING_DISABLED && mode <= OUTGOING_GPT)
        ? static_cast<EOutgoingMode>(mode)
        : OUTGOING_DISABLED;
}

void LLTranslate::translateOutgoingMessage(const std::string& mesg, const LLSD& context,
                                           TranslationSuccess_fn success, TranslationFailure_fn failure)
{
    switch (getOutgoingMode())
    {
        case OUTGOING_STANDARD:
            translateMessage(std::string(), gSavedSettings.getString("FSOutgoingTranslateLanguage"),
                             mesg, success, failure);
            break;
        case OUTGOING_GPT:
            translateOutgoingGPT(mesg, context, success, failure);
            break;
        default:
            success(mesg, std::string());
            break;
    }
}

void LLTranslate::translateOutgoingGPT(const std::string& mesg, const LLSD& context,
                                       TranslationSuccess_fn success, TranslationFailure_fn failure)
{
    const std::string target = gSavedSettings.getString("FSOutgoingTranslateLanguage");
    const S32 context_count = llclamp(gSavedSettings.getS32("FSOutgoingGPTContextCount"), 0, 30);
    LLCoros::instance().launch("OutgoingGPTTranslation",
        boost::bind(&LLTranslate::translateOutgoingGPTCoro, mesg, context, context_count,
                    target, success, failure));
}

void LLTranslate::translateMessageGPT(const std::string& mesg, const std::string& to_lang,
                                      TranslationSuccess_fn success, TranslationFailure_fn failure)
{
    LLCoros::instance().launch("GPTTextTranslation",
        boost::bind(&LLTranslate::translateOutgoingGPTCoro, mesg, LLSD::emptyArray(),
                    0, to_lang, success, failure));
}

void LLTranslate::translateOutgoingGPTCoro(std::string mesg, LLSD context, S32 context_count,
                                           std::string target,
                                           TranslationSuccess_fn success, TranslationFailure_fn failure)
{
    std::string url = gSavedSettings.getString("FSOutgoingGPTBaseURL");
    std::string key = gSavedSettings.getString("FSOutgoingGPTAPIKey");
    std::string model = gSavedSettings.getString("FSOutgoingGPTModel");
    LLStringUtil::trim(url);
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.size() < 17 || url.substr(url.size() - 17) != "/chat/completions")
    {
        const std::string::size_type scheme = url.find("://");
        const std::string::size_type path = scheme == std::string::npos
            ? std::string::npos
            : url.find('/', scheme + 3);
        if (path == std::string::npos)
        {
            url += "/v1";
        }
        url += "/chat/completions";
    }
    if (url.empty() || key.empty() || model.empty())
    {
        failure(0, "GPT translation is not configured");
        return;
    }

    std::string prompt = gSavedSettings.getString("FSOutgoingGPTPrompt");
    LLStringUtil::replaceString(prompt, "{target_language}", target);

    LLSD body;
    body["model"] = model;
    body["temperature"] = 0.2;
    LLSD messages = LLSD::emptyArray();
    messages.append(LLSD().with("role", "system").with("content", prompt));

    const S32 wanted = llclamp(context_count, 0, 30);
    const S32 first = llmax(0, static_cast<S32>(context.size()) - wanted);
    for (S32 i = first; i < static_cast<S32>(context.size()); ++i)
    {
        std::string line = context[i]["name"].asString();
        if (!line.empty()) line += ": ";
        line += context[i]["text"].asString();
        messages.append(LLSD().with("role", "user").with("content", "[Context] " + line));
    }
    messages.append(LLSD().with("role", "user").with("content",
        "[Message to translate into " + target + "]\n" + mesg));
    body["messages"] = messages;

    LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("OutgoingGPTTranslation", policy);
    auto request = std::make_shared<LLCore::HttpRequest>();
    auto headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append("Accept", "application/json");
    headers->append("Content-Type", "application/json");
    headers->append("Authorization", "Bearer " + key);

    LLSD result = adapter->postJsonAndSuspend(request, url, body, headers);
    if (LLApp::isQuitting()) return;

    LLSD http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
    if (!status)
    {
        failure(status.getType(), status.toString());
        return;
    }

    std::string translation;
    if (result.has("choices") && result["choices"].size() > 0)
    {
        translation = result["choices"][0]["message"]["content"].asString();
    }
    LLStringUtil::trim(translation);
    if (translation.empty())
    {
        failure(status.getType(), "GPT translation returned no text");
        return;
    }
    success(translation, std::string());
}

std::string LLTranslate::getScriptDialogTranslation(const std::string& context_key,
                                                    const std::string& source)
{
    const LLSD& exact_entry = scriptDialogTranslationCache()[context_key][source];
    const std::string exact = exact_entry["text"].asString();
    if (!exact.empty()) return exact;

    const ScriptDialogCachePattern pattern = makeScriptDialogCachePattern(source);
    if (pattern.values.empty()) return std::string();
    const LLSD& template_entry = scriptDialogTranslationCache()[context_key][pattern.key];
    return applyScriptDialogCachePattern(template_entry["text"].asString(), pattern);
}

void LLTranslate::setScriptDialogTranslation(const std::string& context_key,
                                             const std::string& source,
                                             const std::string& translation)
{
    cacheScriptDialogTranslation(context_key, source, translation, true);
}

void LLTranslate::clearScriptDialogTranslations()
{
    scriptDialogTranslationCache() = LLSD::emptyMap();
    saveScriptDialogTranslationCache();
    LL_INFOS("ScriptDialogTranslate") << "Cleared script dialog translation cache" << LL_ENDL;
}

void LLTranslate::translateScriptDialog(const std::string& context_key, const std::string& message,
                                        const LLSD& buttons, ScriptDialogTranslationSuccess_fn success,
                                        TranslationFailure_fn failure)
{
    const S32 mode = gSavedSettings.getS32("FSScriptDialogTranslateMode");
    LLSD cached;
    cached["message"] = getScriptDialogTranslation(context_key, message);
    cached["buttons"] = LLSD::emptyArray();
    bool complete = message.empty() || !cached["message"].asString().empty();
    for (LLSD::array_const_iterator it = buttons.beginArray(); it != buttons.endArray(); ++it)
    {
        const std::string translated = getScriptDialogTranslation(context_key, it->asString());
        cached["buttons"].append(translated);
        complete = complete && !translated.empty();
    }
    if (mode == 0)
    {
        return;
    }
    if (complete)
    {
        success(cached);
        return;
    }
    if (mode == 2)
    {
        LLCoros::instance().launch("ScriptDialogGPTTranslation",
            boost::bind(&LLTranslate::translateScriptDialogGPTCoro, context_key, message,
                        buttons, success, failure));
        return;
    }

    std::shared_ptr<StandardDialogTranslationState> state = std::make_shared<StandardDialogTranslationState>();
    state->context_key = context_key;
    state->message = message;
    state->buttons = buttons;
    state->result["message"] = cached["message"];
    state->result["buttons"] = cached["buttons"];
    state->success = success;
    state->failure = failure;
    state->remaining = (message.empty() || !cached["message"].asString().empty() ? 0 : 1);
    for (S32 i = 0; i < static_cast<S32>(buttons.size()); ++i)
    {
        if (state->result["buttons"][i].asString().empty()) ++state->remaining;
    }

    auto finish_one = [state]()
    {
        if (--state->remaining == 0 && !state->failed) state->success(state->result);
    };
    auto fail_once = [state](int status, std::string error)
    {
        if (!state->failed)
        {
            state->failed = true;
            std::string url = gSavedSettings.getString("FSOutgoingGPTBaseURL");
            const std::string key = gSavedSettings.getString("FSOutgoingGPTAPIKey");
            const std::string model = gSavedSettings.getString("FSOutgoingGPTModel");
            LLStringUtil::trim(url);
            if (!url.empty() && !key.empty() && !model.empty())
            {
                LL_WARNS("ScriptDialogTranslate")
                    << "Normal translation failed (" << status << ": " << error
                    << "); falling back to context AI" << LL_ENDL;
                LLCoros::instance().launch("ScriptDialogGPTFallback",
                    boost::bind(&LLTranslate::translateScriptDialogGPTCoro,
                                state->context_key, state->message, state->buttons,
                                state->success, state->failure));
            }
            else
            {
                state->failure(status, error);
            }
        }
    };
    const std::string target = gSavedSettings.getString("FSScriptDialogTranslateLanguage");
    if (!message.empty() && cached["message"].asString().empty())
    {
        translateMessage(std::string(), target, message,
            [state, finish_one](std::string text, std::string)
            {
                cacheScriptDialogTranslation(state->context_key, state->message, text, false);
                state->result["message"] = getScriptDialogTranslation(state->context_key, state->message);
                finish_one();
            }, fail_once);
    }
    for (S32 i = 0; i < static_cast<S32>(buttons.size()); ++i)
    {
        if (!cached["buttons"][i].asString().empty()) continue;
        const std::string source = buttons[i].asString();
        translateMessage(std::string(), target, source,
            [state, finish_one, source, i](std::string text, std::string)
            {
                cacheScriptDialogTranslation(state->context_key, source, text, false);
                state->result["buttons"][i] = getScriptDialogTranslation(state->context_key, source);
                finish_one();
            }, fail_once);
    }
}

void LLTranslate::translateScriptDialogGPTCoro(std::string context_key, std::string message,
                                               LLSD buttons, ScriptDialogTranslationSuccess_fn success,
                                               TranslationFailure_fn failure)
{
    std::string url = gSavedSettings.getString("FSOutgoingGPTBaseURL");
    const std::string key = gSavedSettings.getString("FSOutgoingGPTAPIKey");
    const std::string model = gSavedSettings.getString("FSOutgoingGPTModel");
    const std::string target = gSavedSettings.getString("FSScriptDialogTranslateLanguage");
    LLStringUtil::trim(url);
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.size() < 17 || url.substr(url.size() - 17) != "/chat/completions")
    {
        const std::string::size_type scheme = url.find("://");
        if (scheme != std::string::npos && url.find('/', scheme + 3) == std::string::npos) url += "/v1";
        url += "/chat/completions";
    }
    if (url.empty() || key.empty() || model.empty())
    {
        failure(0, "Script dialog GPT translation is not configured");
        return;
    }

    std::string prompt = gSavedSettings.getString("FSScriptDialogGPTPrompt");
    LLStringUtil::replaceString(prompt, "{target_language}", target);
    const std::string menu = formatScriptDialogContext(message, buttons);
    LLSD body;
    body["model"] = model;
    body["temperature"] = 0.2;
    body["messages"] = LLSD::emptyArray();
    body["messages"].append(LLSD().with("role", "system").with("content", prompt));
    body["messages"].append(LLSD().with("role", "user").with("content",
        "[Complete menu context - read the entire body and every button before translating]\n" + menu));
    body["messages"].append(LLSD().with("role", "user").with("content",
        "[Translation task]\nTranslate the complete menu above. Use its full context to interpret every short "
        "or ambiguous button label. Return only the required JSON, with one translated button for each "
        "original button in exactly the same order. Preserve every [[FS_VALUE_n]] placeholder exactly."));

    LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("ScriptDialogGPTTranslation", policy);
    auto request = std::make_shared<LLCore::HttpRequest>();
    auto headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append("Accept", "application/json");
    headers->append("Content-Type", "application/json");
    headers->append("Authorization", "Bearer " + key);
    LLSD response = adapter->postJsonAndSuspend(request, url, body, headers);
    LLSD http_results = response[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
    if (!status)
    {
        failure(status.getType(), status.toString());
        return;
    }

    try
    {
        std::string content = response["choices"][0]["message"]["content"].asString();
        const size_t first = content.find('{');
        const size_t last = content.rfind('}');
        if (first == std::string::npos || last == std::string::npos) throw std::runtime_error("No JSON object");
        boost::json::object parsed = boost::json::parse(content.substr(first, last - first + 1)).as_object();
        const std::string translated_message = boost::json::value_to<std::string>(parsed.at("message"));
        boost::json::array translated_buttons = parsed.at("buttons").as_array();
        if (translated_buttons.size() != buttons.size()) throw std::runtime_error("Button count mismatch");
        cacheScriptDialogTranslation(context_key, message, translated_message, false);
        LLSD result;
        result["message"] = getScriptDialogTranslation(context_key, message);
        result["buttons"] = LLSD::emptyArray();
        for (S32 i = 0; i < static_cast<S32>(buttons.size()); ++i)
        {
            const std::string translated = boost::json::value_to<std::string>(translated_buttons[i]);
            cacheScriptDialogTranslation(context_key, buttons[i].asString(), translated, false);
            result["buttons"].append(getScriptDialogTranslation(context_key, buttons[i].asString()));
        }
        success(result);
    }
    catch (const std::exception& e)
    {
        LL_WARNS("ScriptDialogTranslate")
            << "Structured AI menu translation was invalid (" << e.what()
            << "); retrying each menu entry separately" << LL_ENDL;
        translateScriptDialogGPTIndividually(context_key, message, buttons, success, failure);
    }
}

void LLTranslate::translateScriptDialogGPTIndividually(std::string context_key, std::string message,
                                                        LLSD buttons, ScriptDialogTranslationSuccess_fn success,
                                                        TranslationFailure_fn failure)
{
    std::shared_ptr<StandardDialogTranslationState> state = std::make_shared<StandardDialogTranslationState>();
    state->context_key = context_key;
    state->message = message;
    state->buttons = buttons;
    state->result["message"] = getScriptDialogTranslation(context_key, message);
    state->result["buttons"] = LLSD::emptyArray();
    state->success = success;
    state->failure = failure;
    state->remaining = message.empty() ? 0 : 1;
    for (S32 i = 0; i < static_cast<S32>(buttons.size()); ++i)
    {
        state->result["buttons"].append(LLSD());
        ++state->remaining;
    }

    if (state->remaining == 0)
    {
        success(state->result);
        return;
    }

    auto finish_one = [state]()
    {
        if (--state->remaining == 0 && !state->failed)
        {
            state->success(state->result);
        }
    };
    auto fail_once = [state](int status, std::string error)
    {
        if (!state->failed)
        {
            state->failed = true;
            state->failure(status, error);
        }
    };
    const std::string target = gSavedSettings.getString("FSScriptDialogTranslateLanguage");
    LLSD full_menu_context = LLSD::emptyArray();
    full_menu_context.append(LLSD().with("text",
        "[Complete script menu - read the entire body and every button before translating this entry]\n" +
        formatScriptDialogContext(message, buttons)));

    if (!message.empty())
    {
        LLCoros::instance().launch("ScriptDialogGPTBodyRetry",
            boost::bind(&LLTranslate::translateOutgoingGPTCoro,
                makeScriptDialogCachePattern(message).key, full_menu_context, 1, target,
            [state, finish_one](std::string text, std::string)
            {
                cacheScriptDialogTranslation(state->context_key, state->message, text, false);
                state->result["message"] = getScriptDialogTranslation(state->context_key, state->message);
                finish_one();
            }, fail_once));
    }
    for (S32 i = 0; i < static_cast<S32>(buttons.size()); ++i)
    {
        const std::string source = buttons[i].asString();
        LLCoros::instance().launch("ScriptDialogGPTButtonRetry",
            boost::bind(&LLTranslate::translateOutgoingGPTCoro,
                makeScriptDialogCachePattern(source).key, full_menu_context, 1, target,
            [state, finish_one, source, i](std::string text, std::string)
            {
                cacheScriptDialogTranslation(state->context_key, source, text, false);
                state->result["buttons"][i] = getScriptDialogTranslation(state->context_key, source);
                finish_one();
            }, fail_once));
    }
}

std::string LLTranslate::addNoTranslateTags(std::string mesg)
{
    if (getPreferredHandler().getCurrentService() == SERVICE_GOOGLE)
    {
        return mesg;
    }

    if (getPreferredHandler().getCurrentService() == SERVICE_DEEPL)
    {
        return mesg;
    }

    if (getPreferredHandler().getCurrentService() == SERVICE_AZURE)
    {
        // https://learn.microsoft.com/en-us/azure/cognitive-services/translator/prevent-translation
        std::string upd_msg(mesg);
        LLUrlMatch match;
        S32 dif = 0;
        //surround all links (including SLURLs) with 'no-translate' tags to prevent unnecessary translation
        while (LLUrlRegistry::instance().findUrl(mesg, match))
        {
            upd_msg.insert(dif + match.getStart(), AZURE_NOTRANSLATE_OPENING_TAG);
            upd_msg.insert(dif + AZURE_NOTRANSLATE_OPENING_TAG.size() + match.getEnd() + 1, AZURE_NOTRANSLATE_CLOSING_TAG);
            mesg.erase(match.getStart(), match.getEnd() - match.getStart());
            dif += match.getEnd() - match.getStart() + static_cast<S32>(AZURE_NOTRANSLATE_OPENING_TAG.size() + AZURE_NOTRANSLATE_CLOSING_TAG.size());
        }
        return upd_msg;
    }
    return mesg;
}

std::string LLTranslate::removeNoTranslateTags(std::string mesg)
{
    if (getPreferredHandler().getCurrentService() == SERVICE_GOOGLE)
    {
        return mesg;
    }
    if (getPreferredHandler().getCurrentService() == SERVICE_DEEPL)
    {
        return mesg;
    }

    if (getPreferredHandler().getCurrentService() == SERVICE_AZURE)
    {
        std::string upd_msg(mesg);
        LLUrlMatch match;
        auto opening_tag_size = AZURE_NOTRANSLATE_OPENING_TAG.size();
        auto closing_tag_size = AZURE_NOTRANSLATE_CLOSING_TAG.size();
        size_t dif = 0;
        //remove 'no-translate' tags we added to the links before
        while (LLUrlRegistry::instance().findUrl(mesg, match))
        {
            if (upd_msg.substr(dif + match.getStart() - opening_tag_size, opening_tag_size) == AZURE_NOTRANSLATE_OPENING_TAG)
            {
                upd_msg.erase(dif + match.getStart() - opening_tag_size, opening_tag_size);
                dif -= opening_tag_size;

                if (upd_msg.substr(dif + match.getEnd() + 1, closing_tag_size) == AZURE_NOTRANSLATE_CLOSING_TAG)
                {
                    upd_msg.replace(dif + match.getEnd() + 1, closing_tag_size, " ");
                    dif -= closing_tag_size - 1;
                }
            }
            mesg.erase(match.getStart(), match.getUrl().size());
            dif += match.getUrl().size();
        }
        return upd_msg;
    }

    return mesg;
}

/*static*/
void LLTranslate::verifyKey(EService service, const LLSD &key, KeyVerificationResult_fn fnc)
{
    LLTranslationAPIHandler& handler = getHandler(service);

    handler.verifyKey(key, fnc);
}


//static
std::string LLTranslate::getTranslateLanguage()
{
    std::string language = gSavedSettings.getString("TranslateLanguage");
    if (language.empty() || language == "default")
    {
        language = LLUI::getLanguage();
    }
    language = language.substr(0,2);
    return language;
}

// static
bool LLTranslate::isTranslationConfigured()
{
    return getPreferredHandler().isConfigured();
}

bool LLTranslate::isGPTTranslationConfigured()
{
    std::string url = gSavedSettings.getString("FSOutgoingGPTBaseURL");
    std::string key = gSavedSettings.getString("FSOutgoingGPTAPIKey");
    std::string model = gSavedSettings.getString("FSOutgoingGPTModel");
    LLStringUtil::trim(url);
    LLStringUtil::trim(key);
    LLStringUtil::trim(model);
    return !url.empty() && !key.empty() && !model.empty();
}

void LLTranslate::logCharsSeen(size_t count)
{
    mCharsSeen += count;
}

void LLTranslate::logCharsSent(size_t count)
{
    mCharsSent += count;
}

void LLTranslate::logSuccess(S32 count)
{
    mSuccessCount += count;
}

void LLTranslate::logFailure(S32 count)
{
    mFailureCount += count;
}

LLSD LLTranslate::asLLSD() const
{
    LLSD res;
    bool on = gSavedSettings.getBOOL("TranslateChat");
    res["on"] = on;
    res["chars_seen"] = (S32) mCharsSeen;
    if (on)
    {
        res["chars_sent"] = (S32) mCharsSent;
        res["success_count"] = mSuccessCount;
        res["failure_count"] = mFailureCount;
        res["language"] = getTranslateLanguage();
        res["service"] = gSavedSettings.getString("TranslationService");
    }
    return res;
}

// static
LLTranslationAPIHandler& LLTranslate::getPreferredHandler()
{
    EService service = SERVICE_AZURE;

    std::string service_str = gSavedSettings.getString("TranslationService");
    if (service_str == "google")
    {
        service = SERVICE_GOOGLE;
    }
    if (service_str == "azure")
    {
        service = SERVICE_AZURE;
    }
    if (service_str == "deepl")
    {
        service = SERVICE_DEEPL;
    }

    return getHandler(service);
}

// static
LLTranslationAPIHandler& LLTranslate::getHandler(EService service)
{
    static LLGoogleTranslationHandler google;
    static LLAzureTranslationHandler azure;
    static LLDeepLTranslationHandler deepl;

    switch (service)
    {
        case SERVICE_AZURE:
            return azure;
        case SERVICE_GOOGLE:
            return google;
        case SERVICE_DEEPL:
            return deepl;
    }

    return azure;
}
