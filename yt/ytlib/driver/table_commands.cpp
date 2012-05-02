#include "stdafx.h"
#include "table_commands.h"
#include "config.h"

#include <ytlib/ytree/yson_parser.h>
#include <ytlib/ytree/tree_visitor.h>

#include <ytlib/table_client/table_reader.h>
#include <ytlib/table_client/table_writer.h>
#include <ytlib/table_client/yson_table_input.h>
#include <ytlib/table_client/yson_row_consumer.h>

namespace NYT {
namespace NDriver {

using namespace NYTree;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

TCommandDescriptor TReadCommand::GetDescriptor()
{
    return TCommandDescriptor(EDataType::Null, EDataType::Table);
}

void TReadCommand::DoExecute(TReadRequestPtr request)
{
    auto stream = Host->GetOutputStream();

    auto reader = New<TTableReader>(
        ~Host->GetConfig()->TableReader,
        ~Host->GetMasterChannel(),
        ~Host->GetTransaction(request),
        ~Host->GetBlockCache(),
        request->Path);

    TYsonTableInput input(
        reader, 
        stream,
        Host->GetConfig()->OutputFormat);

    while (input.ReadRow())
    { }
}

////////////////////////////////////////////////////////////////////////////////

TCommandDescriptor TWriteCommand::GetDescriptor()
{
    return TCommandDescriptor(EDataType::Table, EDataType::Null);
}

void TWriteCommand::DoExecute(TWriteRequestPtr request)
{
    TTableWriter::TOptions options;
    options.Sorted = request->Sorted;

    auto writer = New<TTableWriter>(
        ~Host->GetConfig()->TableWriter,
        options,
        ~Host->GetMasterChannel(),
        ~Host->GetTransaction(request),
        ~Host->GetTransactionManager(),
        request->Path);

    writer->Open();
    TRowConsumer consumer(~writer);

    if (request->Value) {
        auto value = request->Value;
        switch (value->GetType()) {
            case ENodeType::List: {
                FOREACH (const auto& child, value->AsList()->GetChildren()) {
                    VisitTree(~child, &consumer);
                }
                break;
            }

            case ENodeType::Map: {
                VisitTree(~value, &consumer);
                break;
            }

            default:
                YUNREACHABLE();
        }
    } else {
        auto stream = Host->GetInputStream();
        ParseYson(stream, &consumer, EYsonType::ListFragment);
    }

    writer->Close();
    Host->ReplySuccess();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
