#pragma once

#include "public.h"
#include "config.h"

#include <ytlib/ytree/parser.h>
#include <ytlib/ytree/yson_consumer.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

class TDsvParser
    : public NYTree::IParser
{
public:
    explicit TDsvParser(NYTree::IYsonConsumer* consumer, TDsvFormatConfigPtr config = NULL);

    virtual void Read(const TStringBuf& data);
    virtual void Finish();

private:
    NYTree::IYsonConsumer* Consumer;
    TDsvFormatConfigPtr Config;

    bool NewRecordStarted;
    bool ExpectingEscapedChar;

    Stroka CurrentToken;

    char KeyStopSymbols[3];
    char ValueStopSymbols[3];

    const char* Consume(const char* begin, const char* end);
    const char* FindEndOfValue(const char* begin, const char* end);
    const char* FindEndOfKey(const char* begin, const char* end);

    void StartRecordIfNeeded();

    void ValidatePrefix(const Stroka& prefix);

    DECLARE_ENUM(EState,
        (InsidePrefix)
        (InsideKey)
        (InsideValue)
    );
    EState State;
    EState GetStartState();

};

////////////////////////////////////////////////////////////////////////////////

void ParseDsv(
    TInputStream* input,
    NYTree::IYsonConsumer* consumer,
    TDsvFormatConfigPtr config = NULL);

void ParseDsv(
    const TStringBuf& data,
    NYTree::IYsonConsumer* consumer,
    TDsvFormatConfigPtr config = NULL);

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
