#include "stdafx.h"
#include "log.h"
#include "log_manager.h"

#include <ytlib/misc/pattern_formatter.h>
#include <ytlib/misc/configurable.h>
#include <ytlib/ytree/ytree.h>

#include <util/folder/dirut.h>

namespace NYT {
namespace NLog {

////////////////////////////////////////////////////////////////////////////////

TLogger::TLogger(const Stroka& category)
    : Category(category)
    , ConfigVersion(0)
    , LogManager(NULL)
{ }

Stroka TLogger::GetCategory() const
{
    return Category;
}

void TLogger::Write(const TLogEvent& event)
{
    GetLogManager()->Write(event);
}

bool TLogger::IsEnabled(ELogLevel level) const
{
    if (GetLogManager()->GetConfigVersion() != ConfigVersion) {
        const_cast<TLogger*>(this)->UpdateConfig();
    }

    return level >= MinLevel;
}

void TLogger::UpdateConfig()
{
    GetLogManager()->GetLoggerConfig(
        Category,
        &MinLevel,
        &ConfigVersion);
}

TLogManager* TLogger::GetLogManager() const
{
    if (!LogManager) {
        LogManager = TLogManager::Get();
    }
    return LogManager;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NLog
} // namespace NYT
