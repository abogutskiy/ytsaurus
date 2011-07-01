#include "log.h"

#include "../misc/pattern_formatter.h"
#include "../misc/config.h"

namespace NYT {
namespace NLog {

////////////////////////////////////////////////////////////////////////////////

// TODO: review this and that
static const char* const SystemPattern = "$(datetime) $(level) $(category) $(message)";

static const char* const DefaultStdErrWriterName = "StdErr";
static const ELogLevel DefaultStdErrMinLevel= ELogLevel::Info;
static const char* const DefaultStdErrPattern = "$(datetime) $(level) $(category) $(message)";

static const char* const DefaultFileWriterName = "LogFile";
static const char* const DefaultFileName = "default.log";
static const ELogLevel DefaultFileMinLevel = ELogLevel::Debug;
static const char* const DefaultFilePattern =
    "$(datetime) $(level) $(category) $(message)$(tab)$(file?) $(line?) $(function?) $(thread?)";

static const char* const AllCategoriesName = "*";

static TLogger Logger(SystemLoggingCategory);

////////////////////////////////////////////////////////////////////////////////

class TLogManager::TConfig
    : public virtual TRefCountedBase
{
public:
    typedef TIntrusivePtr<TConfig> TPtr;

    typedef yhash_map<Stroka, ILogWriter::TPtr> TWriterMap;

    TWriterMap Writers;

    struct TRule {
        TRule()
            : AllCategories(false)
            , MinLevel(ELogLevel::Minimum)
            , MaxLevel(ELogLevel::Maximum)
        { }

        bool AllCategories;
        yhash_set<Stroka> Categories;

        ELogLevel MinLevel;
        ELogLevel MaxLevel;

        yvector<Stroka> Writers;

        bool IsApplicable(Stroka category) const
        {
            return (AllCategories || Categories.find(category) != Categories.end());
        }

        bool IsApplicable(const TLogEvent& event) const
        {
            ELogLevel level = event.GetLevel();
            return (IsApplicable(event.GetCategory()) &&
                    MinLevel <= level && level <= MaxLevel);
        }
    };

    typedef yvector<TRule> TRules;
    TRules Rules;

    void ConfigureWriters(const TJsonObject* root);
    void ConfigureRules(const TJsonObject* root);

    TConfig();
    TConfig(const TJsonObject* root);
};

void TLogManager::TConfig::ConfigureWriters(const TJsonObject* root)
{
    const TJsonArray* writers = static_cast<const TJsonArray*>(root);
    for (int i = 0; i < writers->Length(); ++i) {
        Stroka name, type, pattern;
        const TJsonObject* item = writers->Item(i);
        TryRead(item, L"Name", &name);
        TryRead(item, L"Type", &type);
        TryRead(item, L"Pattern", &pattern);
        if (type == "File") {
            Stroka fileName;
            NYT::TryRead(item, L"FileName", &fileName);
            Writers[name] = new TFileLogWriter(fileName, pattern);
        } else if (type == "StdErr") {
            Writers[name] = new TStdErrLogWriter(pattern);
        } else if (type == "StdOut") {
            Writers[name] = new TStdOutLogWriter(pattern);
        } else {
            ythrow yexception() <<
                Sprintf("%s is unknown type of writer", ~type);
        }
    }
}

void TLogManager::TConfig::ConfigureRules(const TJsonObject* root)
{
    const TJsonArray* rules = static_cast<const TJsonArray*>(root);
    for (int i = 0; i < rules->Length(); ++i) {
        TRule rule;
        const TJsonObject* item = rules->Item(i);
        yvector<Stroka> categories;
        TryRead(item, L"Categories", &categories);
        if (categories.size() == 1 && categories[0] == AllCategoriesName) {
            rule.AllCategories = true;
        } else {
            rule.AllCategories = false;
            rule.Categories.insert(categories.begin(), categories.end());
        }

        ReadEnum<ELogLevel>(item, L"MinLevel", &rule.MinLevel, ELogLevel::Minimum);
        ReadEnum<ELogLevel>(item, L"MaxLevel", &rule.MaxLevel, ELogLevel::Maximum);
        TryRead(item, L"Writers", &rule.Writers);
        Rules.push_back(rule);
    }
}

TLogManager::TConfig::TConfig()
{
    Writers.insert(MakePair(
        DefaultStdErrWriterName,
        new TStdErrLogWriter(SystemPattern)));

    TRule stdErrRule;
    stdErrRule.AllCategories = true;
    stdErrRule.MinLevel = DefaultStdErrMinLevel;
    stdErrRule.Writers.push_back(DefaultStdErrWriterName);
    Rules.push_back(stdErrRule);

    Writers.insert(MakePair(
        DefaultFileWriterName,
        new TFileLogWriter(DefaultFileName, DefaultFilePattern)));

    TRule fileRule;
    fileRule.AllCategories = true;
    fileRule.MinLevel = DefaultFileMinLevel;
    fileRule.Writers.push_back(DefaultFileWriterName);
    Rules.push_back(fileRule);
}

TLogManager::TConfig::TConfig(const TJsonObject* root)
{
    ConfigureWriters(root->Value(L"Writers"));
    ConfigureRules(root->Value(L"Rules"));
}

////////////////////////////////////////////////////////////////////////////////

TLogManager::TLogManager()
    : ConfigVersion(0)
    , Queue(new TActionQueue(false))
{
    ConfigureSystem();
    ConfigureDefault();
}

TLogManager* TLogManager::Get()
{
    return Singleton<TLogManager>();
}

void TLogManager::Flush()
{
    TActionQueue::TPtr queue = Queue;
    if (~queue != NULL) {
        FromMethod(&TLogManager::DoFlush, this)
            ->AsyncVia(~queue)
            ->Do()
            ->Get();
    }
}

void TLogManager::Shutdown()
{
    Flush();
    
    TActionQueue::TPtr queue = Queue;
    if (~queue != NULL) {
        queue->Shutdown();
        Queue = NULL;
    }
}

TVoid TLogManager::DoFlush()
{
    for (TConfig::TWriterMap::iterator it = Configuration->Writers.begin();
         it != Configuration->Writers.end();
         ++it)
    {
        it->second->Flush();
    }
    return TVoid();
}

void TLogManager::Write(const TLogEvent& event)
{
    TActionQueue::TPtr queue = Queue;
    if (~queue != NULL) {
        queue->Invoke(FromMethod(&TLogManager::DoWrite, this, event));

        // TODO: use system-wide exit function
        if (event.GetLevel() == ELogLevel::Fatal) {
            Shutdown();
            exit(1);
        }
    }
}

void TLogManager::DoWrite(const TLogEvent& event)
{
    yvector<ILogWriter::TPtr> writers = GetWriters(event);
    for (yvector<ILogWriter::TPtr>::iterator it = writers.begin();
         it != writers.end();
         ++it)
    {
        (*it)->Write(event);
    }
}

yvector<ILogWriter::TPtr> TLogManager::GetWriters(const TLogEvent& event)
{
    if (event.GetCategory() == SystemLoggingCategory)
        return SystemWriters;
    else
        return GetConfiguredWriters(event);
}

yvector<ILogWriter::TPtr> TLogManager::GetConfiguredWriters(const TLogEvent& event)
{
    Stroka category = event.GetCategory();
    ELogLevel level = event.GetLevel();

    yhash_set<Stroka> writerIds;
    for (TConfig::TRules::iterator it = Configuration->Rules.begin();
        it != Configuration->Rules.end(); ++it) {
        if (it->IsApplicable(event)) {
            writerIds.insert(it->Writers.begin(), it->Writers.end());
        }
    }

    yvector<ILogWriter::TPtr> writers;
    for (yhash_set<Stroka>::const_iterator it = writerIds.begin();
         it != writerIds.end();
         ++it)
    {
        writers.push_back(Configuration->Writers[*it]);
    }

    return writers;
}

int TLogManager::GetConfigVersion()
{
    TGuard<TSpinLock> guard(&SpinLock);
    return ConfigVersion;
}

void TLogManager::GetLoggerConfig(
    Stroka category,
    ELogLevel* minLevel,
    int* configVersion)
{
    TGuard<TSpinLock> guard(&SpinLock);
    *minLevel = GetMinLevel(category);
    *configVersion = ConfigVersion;
}

NYT::NLog::ELogLevel TLogManager::GetMinLevel(Stroka category)
{
    ELogLevel level = ELogLevel::Maximum;

    for(TConfig::TRules::iterator it = Configuration->Rules.begin();
        it != Configuration->Rules.end();
        ++it)
    {
        if (it->IsApplicable(category)) {
            level = Min(level, it->MinLevel);
        }
    }
    return level;
}

void TLogManager::Configure(TJsonObject* root)
{
    TGuard<TSpinLock> guard(&SpinLock);

    TConfig::TPtr newConfig;
    try {
        newConfig = new TConfig(root);
    } catch (const yexception& e) {
        LOG_ERROR("Error configuring logging: %s", e.what())
        return;
    }

    Configuration = newConfig;
    AtomicIncrement(ConfigVersion);
}

void TLogManager::Configure(Stroka fileName, Stroka rootPath)
{
    TIFStream configStream(fileName);
    TJsonReader reader(CODES_UTF8, &configStream);
    TJsonObject* root = reader.ReadAll();
    TJsonObject* subTree = GetSubTree(root, rootPath);
    Configure(subTree);
}

void TLogManager::ConfigureSystem()
{
    SystemWriters.push_back(new TStdErrLogWriter(SystemPattern));
}

void TLogManager::ConfigureDefault()
{
    TGuard<TSpinLock> guard(&SpinLock);

    Configuration = new TConfig();
    AtomicIncrement(ConfigVersion);
}

////////////////////////////////////////////////////////////////////////////////

TLogger::TLogger(Stroka category)
    : Category(category)
    , ConfigVersion(0)
{ }

Stroka TLogger::GetCategory() const
{
    return Category;
}

void TLogger::Write(const TLogEvent& event)
{
    TLogManager::Get()->Write(event);
}

bool TLogger::IsEnabled(ELogLevel level)
{
    TLogManager* manager = TLogManager::Get();
    if (manager->GetConfigVersion() != ConfigVersion) {
        UpdateConfig();
    }
    return level >= MinLevel;
}

void TLogger::UpdateConfig()
{
    TLogManager* manager = TLogManager::Get();
    manager->GetLoggerConfig(
        Category,
        &MinLevel,
        &ConfigVersion);
}

////////////////////////////////////////////////////////////////////////////////

TPrefixLogger::TPrefixLogger(TLogger& baseLogger, const Stroka& prefix)
    : BaseLogger(baseLogger)
    , Prefix(prefix)
{ }

Stroka TPrefixLogger::GetCategory() const
{
    return BaseLogger.GetCategory();
}

void TPrefixLogger::Write(const TLogEvent& event)
{
    // TODO: optimize?
    TLogEvent prefixedEvent(
        event.GetCategory(),
        event.GetLevel(),
        Prefix + event.GetMessage());

    const TLogEvent::TProperties& properties = event.GetProperties();
    for (TLogEvent::TProperties::const_iterator it = properties.begin();
         it != properties.end();
         ++it)
    {
        prefixedEvent.AddProperty(it->First(), it->Second());
    }

    BaseLogger.Write(prefixedEvent);
}

bool TPrefixLogger::IsEnabled(ELogLevel level)
{
    return BaseLogger.IsEnabled(level);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NLog
} // namespace NYT
