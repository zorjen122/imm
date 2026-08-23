#include "VerificationService.h"

#include "Configer.h"
#include "Logger.h"
#include "Redis.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>

namespace wimi
{
namespace
{

bool ParseBool(const char* value, bool fallback)
{
    if (value == nullptr)
        return fallback;
    const std::string normalized = value;
    if (normalized == "1" || normalized == "true" || normalized == "on")
        return true;
    if (normalized == "0" || normalized == "false" || normalized == "off")
        return false;
    return fallback;
}

std::string EnvironmentOr(const char* name, std::string fallback)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::move(fallback) : std::string(value);
}

struct EmailPayload {
    std::string data;
    std::size_t offset{0};
};

std::size_t ReadEmailPayload(char* buffer, std::size_t size, std::size_t count, void* userdata)
{
    auto* payload = static_cast<EmailPayload*>(userdata);
    const std::size_t capacity = size * count;
    const std::size_t remaining = payload->data.size() - payload->offset;
    const std::size_t copied = std::min(capacity, remaining);
    if (copied == 0)
        return 0;
    std::memcpy(buffer, payload->data.data() + payload->offset, copied);
    payload->offset += copied;
    return copied;
}

} // namespace

VerificationService::VerificationService()
{
    auto server = Configer::getNode("server");
    auto verification = server ? server["verification"] : YAML::Node();
    if (verification) {
        codeTtlSeconds_ = verification["codeTtlSeconds"].as<int>(180);
        resendCooldownSeconds_ = verification["resendCooldownSeconds"].as<int>(60);
        maxAttempts_ = verification["maxAttempts"].as<int>(5);
        exposeCodeInResponse_ = verification["exposeCodeInResponse"].as<bool>(false);

        auto email = verification["email"];
        if (email) {
            emailEnabled_ = email["enabled"].as<bool>(false);
            smtpUrl_ = email["smtpUrl"].as<std::string>(smtpUrl_);
            emailUser_ = email["username"].as<std::string>("");
            emailPassword_ = email["password"].as<std::string>("");
        }
    }

    emailEnabled_ = ParseBool(std::getenv("WIMI_VERIFY_SEND_EMAIL"), emailEnabled_);
    exposeCodeInResponse_ = ParseBool(std::getenv("WIMI_VERIFY_EXPOSE_CODE"), exposeCodeInResponse_);
    smtpUrl_ = EnvironmentOr("WIMI_VERIFY_SMTP_URL", std::move(smtpUrl_));
    emailUser_ = EnvironmentOr("WIMI_VERIFY_EMAIL_USER", std::move(emailUser_));
    emailPassword_ = EnvironmentOr("WIMI_VERIFY_EMAIL_PASS", std::move(emailPassword_));
}

std::string VerificationService::NormalizeEmail(const std::string& email)
{
    auto first = std::find_if_not(email.begin(), email.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    auto last =
        std::find_if_not(email.rbegin(), email.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
    if (first >= last)
        return {};

    std::string normalized(first, last);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}

bool VerificationService::IsValidEmail(const std::string& email)
{
    if (email.empty() || email.size() > 254 || email.find_first_of(" \t\r\n{}") != std::string::npos)
        return false;
    const auto at = email.find('@');
    if (at == std::string::npos || at == 0 || at != email.rfind('@') || at + 3 > email.size())
        return false;
    const auto dot = email.find('.', at + 2);
    return dot != std::string::npos && dot + 1 < email.size();
}

std::string VerificationService::GenerateCode() const
{
    std::random_device random;
    std::uniform_int_distribution<int> distribution(0, 999999);
    const int value = distribution(random);
    std::string code = std::to_string(value);
    code.insert(code.begin(), 6 - code.size(), '0');
    return code;
}

VerificationCodeIssue VerificationService::RequestEmailCode(const std::string& rawEmail)
{
    VerificationCodeIssue result;
    const std::string email = NormalizeEmail(rawEmail);
    if (!IsValidEmail(email)) {
        result.error = ErrorCodes::EmailNotMatch;
        return result;
    }
    if (!emailEnabled_ && !exposeCodeInResponse_) {
        logger->error("[verification] neither SMTP delivery nor development response is "
                      "enabled");
        result.error = ErrorCodes::DependencyUnavailable;
        return result;
    }

    const std::string code = GenerateCode();
    const int issued =
        db::RedisDao::GetInstance()->issueVerificationCode(email, code, codeTtlSeconds_, resendCooldownSeconds_);
    if (issued < 0) {
        result.error = ErrorCodes::DependencyUnavailable;
        return result;
    }
    result.issued = issued == 1;

    std::string activeCode = code;
    if (!result.issued && exposeCodeInResponse_)
        activeCode = db::RedisDao::GetInstance()->getVerificationCode(email);

    if (result.issued && emailEnabled_ && !SendEmail(email, code)) {
        db::RedisDao::GetInstance()->clearVerificationCode(email, code);
        result.error = ErrorCodes::DependencyUnavailable;
        return result;
    }
    if (exposeCodeInResponse_)
        result.developmentCode = std::move(activeCode);
    return result;
}

bool VerificationService::VerifyAndConsume(const std::string& rawEmail, const std::string& code)
{
    const std::string email = NormalizeEmail(rawEmail);
    if (!IsValidEmail(email) || code.size() != 6)
        return false;
    return db::RedisDao::GetInstance()->consumeVerificationCode(email, code, maxAttempts_);
}

bool VerificationService::SendEmail(const std::string& recipient, const std::string& code) const
{
    if (smtpUrl_.empty() || emailUser_.empty() || emailPassword_.empty()) {
        logger->error("[verification] SMTP is enabled but its URL or credentials are "
                      "missing");
        return false;
    }

    static std::once_flag curlInit;
    static bool curlReady = false;
    std::call_once(curlInit, []() { curlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; });
    if (!curlReady)
        return false;

    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return false;

    const std::string sender = "<" + emailUser_ + ">";
    const std::string receiver = "<" + recipient + ">";
    EmailPayload payload{"To: " + recipient + "\r\nFrom: " + emailUser_ +
                         "\r\nSubject: WIMI verification code\r\n"
                         "Content-Type: text/plain; charset=utf-8\r\n\r\n"
                         "Your WIMI verification code is " +
                         code + ". It expires in " + std::to_string(codeTtlSeconds_) + " seconds.\r\n"};
    curl_slist* recipients = nullptr;
    recipients = curl_slist_append(recipients, receiver.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, smtpUrl_.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, emailUser_.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, emailPassword_.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, sender.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadEmailPayload);
    curl_easy_setopt(curl, CURLOPT_READDATA, &payload);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode status = curl_easy_perform(curl);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    if (status != CURLE_OK) {
        logger->error("[verification] SMTP delivery failed: {}", curl_easy_strerror(status));
        return false;
    }
    return true;
}

} // namespace wimi
