#include "log.h"
#include "log_manager.h"

namespace NYT {
namespace NLogging {

////////////////////////////////////////////////////////////////////////////////

TLogger::TLogger()
    : LogManager_(nullptr)
    , Category_(nullptr)
{ }

TLogger::TLogger(const char* categoryName)
    : LogManager_(TLogManager::Get())
    , Category_(LogManager_->GetCategory(categoryName))
{ }

const TLoggingCategory* TLogger::GetCategory() const
{
    return Category_;
}

bool TLogger::IsEnabled(ELogLevel level) const
{
    if (!Category_) {
        return false;
    }

    if (Category_->CurrentVersion != Category_->ActualVersion->load(std::memory_order_relaxed)) {
        LogManager_->UpdateCategory(Category_);
    }

    return level >= Category_->MinLevel;
}

void TLogger::Write(TLogEvent&& event) const
{
    if (!Context_.empty()) {
        event.Message = GetMessageWithContext(event.Message, Context_);
    }

    LogManager_->Enqueue(std::move(event));
}

TLogger& TLogger::AddRawTag(const TString& tag)
{
    if (!Context_.empty()) {
        Context_ += ", ";
    }
    Context_ += tag;
    return *this;
}

const TString& TLogger::GetContext() const
{
    return Context_;
}

TString TLogger::GetMessageWithContext(const TString& originalMessage, const TString& context)
{
    auto endIndex = originalMessage.find('\n');
    if (endIndex == TString::npos) {
        endIndex = originalMessage.length();
    }
    if (endIndex > 0 && originalMessage[endIndex - 1] == ')') {
        return
            originalMessage.substr(0, endIndex - 1) +
            ", " + context +
            originalMessage.substr(endIndex - 1);
    } else {
        return
            originalMessage.substr(0, endIndex) +
            " (" + context + ")" +
            originalMessage.substr(endIndex);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT
