#pragma once

#include "public.h"
#include "symbols.h"

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

struct TSchemedDsvTable
{
    TLookupTable Stops;
    TEscapeTable Escapes;

    TSchemedDsvTable(const TSchemedDsvFormatConfigPtr& config)
    {
        std::vector<char> stopSymbols;
        stopSymbols.push_back(config->RecordSeparator);
        stopSymbols.push_back(config->FieldSeparator);
        stopSymbols.push_back(config->EscapingSymbol);
        stopSymbols.push_back('\0');
        stopSymbols.push_back('\r');

        Stops.Fill(
            stopSymbols.data(),
            stopSymbols.data() + stopSymbols.size());
    }
};

} // namespace NFormats
} // namespace NYT

