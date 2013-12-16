#include "stdafx.h"
#include "table_commands.h"
#include "config.h"
#include "table_mount_cache.h"

#include <core/concurrency/async_stream.h>
#include <core/concurrency/fiber.h>

#include <core/yson/parser.h>
#include <core/yson/consumer.h>

#include <core/ytree/fluent.h>

#include <ytlib/formats/parser.h>

#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/memory_reader.h>
#include <ytlib/chunk_client/memory_writer.h>

#include <ytlib/table_client/table_reader.h>
#include <ytlib/table_client/table_writer.h>
#include <ytlib/table_client/table_consumer.h>
#include <ytlib/table_client/table_producer.h>

#include <ytlib/new_table_client/chunk_reader.h>
#include <ytlib/new_table_client/chunk_writer.h>
#include <ytlib/new_table_client/name_table.h>
#include <ytlib/new_table_client/reader.h>
#include <ytlib/new_table_client/row.h>

#include <ytlib/query_client/plan_fragment.h>
#include <ytlib/query_client/executor.h>

#include <ytlib/tablet_client/public.h>
#include <ytlib/tablet_client/tablet_service_proxy.h>
#include <ytlib/tablet_client/protocol.h>

#include <ytlib/hive/cell_directory.h>

#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/new_table_client/chunk_writer.h>
#include <ytlib/new_table_client/name_table.h>
#include <ytlib/new_table_client/row.h>

#include <ytlib/tablet_client/tablet_service_proxy.h>

namespace NYT {
namespace NDriver {

using namespace NYson;
using namespace NYTree;
using namespace NFormats;
using namespace NChunkClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NQueryClient;
using namespace NConcurrency;
using namespace NTransactionClient;
using namespace NHive;
using namespace NVersionedTableClient;

////////////////////////////////////////////////////////////////////////////////

void TReadCommand::DoExecute()
{
    // COMPAT(babenko): remove Request->TableReader
    auto config = UpdateYsonSerializable(
        Context->GetConfig()->TableReader,
        Request->TableReader);
    config = UpdateYsonSerializable(
        config,
        Request->GetOptions());

    auto reader = New<TAsyncTableReader>(
        config,
        Context->GetMasterChannel(),
        GetTransaction(EAllowNullTransaction::Yes, EPingTransaction::Yes),
        Context->GetBlockCache(),
        Request->Path);

    auto output = Context->Request().OutputStream;

    // TODO(babenko): provide custom allocation tag
    TBlobOutput buffer;
    i64 bufferLimit = Context->GetConfig()->ReadBufferSize;

    auto format = Context->GetOutputFormat();
    auto consumer = CreateConsumerForFormat(format, EDataType::Tabular, &buffer);

    reader->Open();

    auto fetchNextItem = [&] () -> bool {
        if (!reader->FetchNextItem()) {
            auto result = WaitFor(reader->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        }
        return reader->IsValid();
    };


    if (!fetchNextItem()) {
        return;
    }

    BuildYsonMapFluently(Context->Request().ResponseParametersConsumer)
        .Item("start_row_index").Value(reader->GetTableRowIndex());

    auto flushBuffer = [&] () {
        if (!output->Write(buffer.Begin(), buffer.Size())) {
            auto result = WaitFor(output->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        }
        buffer.Clear();
    };

    while (true) {
        ProduceRow(~consumer, reader->GetRow());

        if (buffer.Size() > bufferLimit) {
            flushBuffer();
        }

        if (!fetchNextItem()) {
            break;
        }
    }

    if (buffer.Size() > 0) {
        flushBuffer();
    }
}

//////////////////////////////////////////////////////////////////////////////////

void TWriteCommand::DoExecute()
{
    // COMPAT(babenko): remove Request->TableWriter
    auto config = UpdateYsonSerializable(
        Context->GetConfig()->TableWriter,
        Request->TableWriter);
    config = UpdateYsonSerializable(
        config,
        Request->GetOptions());

    auto writer = CreateAsyncTableWriter(
        config,
        Context->GetMasterChannel(),
        GetTransaction(EAllowNullTransaction::Yes, EPingTransaction::Yes),
        Context->GetTransactionManager(),
        Request->Path,
        Request->Path.Attributes().Find<TKeyColumns>("sorted_by"));

    writer->Open();

    TTableConsumer consumer(writer);

    auto format = Context->GetInputFormat();
    auto parser = CreateParserForFormat(format, EDataType::Tabular, &consumer);

    struct TWriteBufferTag { };
    auto buffer = TSharedRef::Allocate<TWriteBufferTag>(config->BlockSize);

    auto input = Context->Request().InputStream;

    while (true) {
        if (!input->Read(buffer.Begin(), buffer.Size())) {
            auto result = WaitFor(input->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        }

        size_t length = input->GetReadLength();
        if (length == 0)
            break;

        parser->Read(TStringBuf(buffer.Begin(), length));

        if (!writer->IsReady()) {
            auto result = WaitFor(writer->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        }
    }

    parser->Finish();

    writer->Close();
}

////////////////////////////////////////////////////////////////////////////////

void TMountCommand::DoExecute()
{
    auto req = TTableYPathProxy::Mount(Request->Path.GetPath());
    if (Request->FirstTabletIndex) {
        req->set_first_tablet_index(*Request->FirstTabletIndex);
    }
    if (Request->LastTabletIndex) {
        req->set_first_tablet_index(*Request->LastTabletIndex);
    }

    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    ReplySuccess();
}

////////////////////////////////////////////////////////////////////////////////

void TUnmountCommand::DoExecute()
{
    auto req = TTableYPathProxy::Unmount(Request->Path.GetPath());
    if (Request->FirstTabletIndex) {
        req->set_first_tablet_index(*Request->FirstTabletIndex);
    }
    if (Request->LastTabletIndex) {
        req->set_first_tablet_index(*Request->LastTabletIndex);
    }

    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    ReplySuccess();
}

////////////////////////////////////////////////////////////////////////////////

void TReshardCommand::DoExecute()
{
    auto req = TTableYPathProxy::Reshard(Request->Path.GetPath());
    if (Request->FirstTabletIndex) {
        req->set_first_tablet_index(*Request->FirstTabletIndex);
    }
    if (Request->LastTabletIndex) {
        req->set_first_tablet_index(*Request->LastTabletIndex);
    }
    ToProto(req->mutable_pivot_keys(), Request->PivotKeys);

    auto rsp = WaitFor(ObjectProxy->Execute(req));
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    ReplySuccess();
}

////////////////////////////////////////////////////////////////////////////////

void TInsertCommand::DoExecute()
{
    // COMPAT(babenko): remove Request->TableWriter
    auto config = UpdateYsonSerializable(
        Context->GetConfig()->TableWriter,
        Request->TableWriter);
    config = UpdateYsonSerializable(
        config,
        Request->GetOptions());

    auto tableMountCache = Context->GetTableMountCache();
    auto mountInfoOrError = WaitFor(tableMountCache->LookupInfo(Request->Path.GetPath()));
    THROW_ERROR_EXCEPTION_IF_FAILED(mountInfoOrError);

    const auto& mountInfo = mountInfoOrError.GetValue();

    // Parse input data.

    TBuildingTableConsumer consumer(
        mountInfo->Schema,
        mountInfo->KeyColumns);
    consumer.SetTreatMissingAsNull(!Request->Update);

    auto format = Context->GetInputFormat();
    auto parser = CreateParserForFormat(format, EDataType::Tabular, &consumer);

    struct TWriteBufferTag { };
    auto buffer = TSharedRef::Allocate<TWriteBufferTag>(config->BlockSize);

    auto input = Context->Request().InputStream;

    while (true) {
        if (!input->Read(buffer.Begin(), buffer.Size())) {
            auto result = WaitFor(input->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        }

        size_t length = input->GetReadLength();
        if (length == 0)
            break;

        parser->Read(TStringBuf(buffer.Begin(), length));
    }

    parser->Finish();

    // Write data into the tablets.

    struct TWriteBuffer
        : public TIntrinsicRefCounted
    {
        TProtocolWriter Writer;
    };

    typedef TIntrusivePtr<TWriteBuffer> TWriteBufferPtr;

    yhash_map<const TTabletInfo*, TWriteBufferPtr> tabletToBuffer;

    auto getBuffer = [&] (const TTabletInfo& tabletInfo) -> TWriteBufferPtr {
        auto it = tabletToBuffer.find(&tabletInfo);
        if (it == tabletToBuffer.end()) {
            auto buffer = New<TWriteBuffer>();
            YCHECK(tabletToBuffer.insert(std::make_pair(&tabletInfo, buffer)).second);
            return buffer;
        } else {
            return it->second;
        }
    };

    auto transactionManager = Context->GetTransactionManager();
    TTransactionStartOptions startOptions;
    startOptions.Type = ETransactionType::Tablet;
    auto transactionOrError = WaitFor(transactionManager->Start(startOptions));
    THROW_ERROR_EXCEPTION_IF_FAILED(transactionOrError);
    auto transaction = transactionOrError.GetValue();

    for (const auto& row : consumer.GetRows()) {
        const auto& tabletInfo = mountInfo->GetTablet(row);
        if (tabletInfo.State != ETabletState::Mounted) {
            THROW_ERROR_EXCEPTION("Tablet %s is not mounted",
                ~ToString(tabletInfo.TabletId));
        }

        transaction->AddParticipant(tabletInfo.CellId);

        auto buffer = getBuffer(tabletInfo);
        buffer->Writer.WriteCommand(EProtocolCommand::WriteRow);
        buffer->Writer.WriteUnversionedRow(row);
    }

    auto cellDirectory = Context->GetCellDirectory();
    
    for (const auto& pair : tabletToBuffer) {
        const auto& tabletInfo = *pair.first;
        const auto& buffer = pair.second;
    
        auto channel = cellDirectory->GetChannelOrThrow(tabletInfo.CellId);

        TTabletServiceProxy tabletProxy(channel);
        auto writeReq = tabletProxy.Write();
        ToProto(writeReq->mutable_transaction_id(), transaction->GetId());
        ToProto(writeReq->mutable_tablet_id(), tabletInfo.TabletId);
        writeReq->set_encoded_request(buffer->Writer.Finish());

        auto writeRsp = WaitFor(writeReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(*writeRsp);
    }

    auto commitResult = WaitFor(transaction->Commit());
    THROW_ERROR_EXCEPTION_IF_FAILED(commitResult);
}

////////////////////////////////////////////////////////////////////////////////

void TSelectCommand::DoExecute()
{
    using namespace NChunkClient;
    using namespace NVersionedTableClient;

    auto fragment = TPlanFragment::Prepare(
        Request->Query,
        Context->GetQueryCallbacksProvider()->GetPrepareCallbacks());

    auto coordinator = CreateCoordinatedEvaluator(
        GetCurrentInvoker(),
        Context->GetQueryCallbacksProvider()->GetCoordinateCallbacks());

    auto memoryWriter = New<TMemoryWriter>();
    auto chunkWriter = New<TChunkWriter>(
        New<NVersionedTableClient::TChunkWriterConfig>(),
        New<TEncodingWriterOptions>(),
        memoryWriter);

    {
        auto error = WaitFor(coordinator->Execute(fragment, chunkWriter));
        THROW_ERROR_EXCEPTION_IF_FAILED(error);
    }

    auto memoryReader = New<TMemoryReader>(
        std::move(memoryWriter->GetChunkMeta()),
        std::move(memoryWriter->GetBlocks()));
    auto chunkReader = CreateChunkReader(
        New<TChunkReaderConfig>(),
        memoryReader);

    TBlobOutput buffer;
    auto format = Context->GetOutputFormat();
    auto consumer = CreateConsumerForFormat(format, EDataType::Tabular, &buffer);

    auto nameTable = New<TNameTable>();
    {
        auto error = WaitFor(chunkReader->Open(
            nameTable,
            TTableSchema(),
            true));
        THROW_ERROR_EXCEPTION_IF_FAILED(error);
    }

    const int RowsBufferSize = 1000;
    std::vector<NVersionedTableClient::TVersionedRow> rows;
    rows.reserve(RowsBufferSize);
    
    while (true) {
        bool hasData = chunkReader->Read(&rows);
        for (auto row : rows) {
            consumer->OnListItem();
            consumer->OnBeginMap();
            for (int i = 0; i < row.GetValueCount(); ++i) {
                const auto& value = row[i];
                if (value.Type == EValueType::Null)
                    continue;
                consumer->OnKeyedItem(nameTable->GetName(value.Id));
                switch (value.Type) {
                    case EValueType::Integer:
                        consumer->OnIntegerScalar(value.Data.Integer);
                        break;
                    case EValueType::Double:
                        consumer->OnDoubleScalar(value.Data.Double);
                        break;
                    case EValueType::String:
                        consumer->OnStringScalar(TStringBuf(value.Data.String, value.Length));
                        break;
                    case EValueType::Any:
                        consumer->OnRaw(TStringBuf(value.Data.String, value.Length), EYsonType::Node);
                        break;
                    default:
                        YUNREACHABLE();
                }
            }
            consumer->OnEndMap();
        }
        if (!hasData) {
            break;
        }
        if (rows.size() < rows.capacity()) {
            auto result = WaitFor(chunkReader->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        }
        rows.clear();
    }

    auto output = Context->Request().OutputStream;
    if (!output->Write(buffer.Begin(), buffer.Size())) {
        auto result = WaitFor(output->GetReadyEvent());
        THROW_ERROR_EXCEPTION_IF_FAILED(result);
    }
    buffer.Clear();

    ReplySuccess();
}

////////////////////////////////////////////////////////////////////////////////

void TLookupCommand::DoExecute()
{
    auto tableMountCache = Context->GetTableMountCache();
    auto mountInfoOrError = WaitFor(tableMountCache->LookupInfo(Request->Path.GetPath()));
    THROW_ERROR_EXCEPTION_IF_FAILED(mountInfoOrError);

    const auto& mountInfo = mountInfoOrError.GetValue();
    const auto& tabletInfo = mountInfo->GetTablet(Request->Key);
    if (tabletInfo.State != ETabletState::Mounted) {
        THROW_ERROR_EXCEPTION("Tablet %s is not mounted",
            ~ToString(tabletInfo.TabletId));
    }

    auto nameTable = TNameTable::FromSchema(mountInfo->Schema);

    auto cellDirectory = Context->GetCellDirectory();
    auto channel = cellDirectory->GetChannelOrThrow(tabletInfo.CellId);

    TTabletServiceProxy proxy(channel);
    auto req = proxy.Read();

    ToProto(req->mutable_tablet_id(), tabletInfo.TabletId);
    req->set_timestamp(Request->Timestamp);

    TProtocolWriter writer;
    writer.WriteCommand(EProtocolCommand::LookupRow);
    writer.WriteUnversionedRow(Request->Key);
    writer.WriteColumnFilter(
        Request->Columns
        ? TColumnFilter(*Request->Columns)
        : TColumnFilter());
    req->set_encoded_request(writer.Finish());

    auto rsp = WaitFor(req->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(*rsp);

    TProtocolReader reader(rsp->encoded_response());
    std::vector<TVersionedRow> rowset;
    reader.ReadVersionedRowset(&rowset);

    if (!rowset.empty()) {
        YCHECK(rowset.size() <= 1);
        auto row = rowset[0];

        TBlobOutput buffer;
        auto format = Context->GetOutputFormat();
        auto consumer = CreateConsumerForFormat(format, EDataType::Tabular, &buffer);
        
        consumer->OnListItem();
        consumer->OnBeginMap();
        for (int index = 0; index < row.GetValueCount(); ++index) {
            const auto& value = row[index];
            if (value.Type == EValueType::Null)
                continue;
            consumer->OnKeyedItem(nameTable->GetName(value.Id));
            switch (value.Type) {
                case EValueType::Integer:
                    consumer->OnIntegerScalar(value.Data.Integer);
                    break;
                case EValueType::Double:
                    consumer->OnDoubleScalar(value.Data.Double);
                    break;
                case EValueType::String:
                    consumer->OnStringScalar(TStringBuf(value.Data.String, value.Length));
                    break;
                case EValueType::Any:
                    consumer->OnRaw(TStringBuf(value.Data.String, value.Length), EYsonType::Node);
                    break;
                default:
                    YUNREACHABLE();
            }
        }
        consumer->OnEndMap();

        auto output = Context->Request().OutputStream;
        if (!output->Write(buffer.Begin(), buffer.Size())) {
            auto result = WaitFor(output->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        }
    }

    ReplySuccess();
}

////////////////////////////////////////////////////////////////////////////////

void TDeleteCommand::DoExecute()
{
    auto tableMountCache = Context->GetTableMountCache();
    auto mountInfoOrError = WaitFor(tableMountCache->LookupInfo(Request->Path.GetPath()));
    THROW_ERROR_EXCEPTION_IF_FAILED(mountInfoOrError);

    const auto& mountInfo = mountInfoOrError.GetValue();
    const auto& tabletInfo = mountInfo->GetTablet(Request->Key);
    if (tabletInfo.State != ETabletState::Mounted) {
        THROW_ERROR_EXCEPTION("Tablet %s is not mounted",
            ~ToString(tabletInfo.TabletId));
    }

    auto transactionManager = Context->GetTransactionManager();
    TTransactionStartOptions startOptions;
    startOptions.Type = ETransactionType::Tablet;
    auto transactionOrError = WaitFor(transactionManager->Start(startOptions));
    THROW_ERROR_EXCEPTION_IF_FAILED(transactionOrError);
    auto transaction = transactionOrError.GetValue();

    transaction->AddParticipant(tabletInfo.CellId);

    auto cellDirectory = Context->GetCellDirectory();
    auto channel = cellDirectory->GetChannelOrThrow(tabletInfo.CellId);

    TTabletServiceProxy proxy(channel);
    auto writeReq = proxy.Write();
    ToProto(writeReq->mutable_transaction_id(), transaction->GetId());
    ToProto(writeReq->mutable_tablet_id(), tabletInfo.TabletId);

    TProtocolWriter writer;
    writer.WriteCommand(EProtocolCommand::DeleteRow);
    writer.WriteUnversionedRow(Request->Key);
    writeReq->set_encoded_request(writer.Finish());

    auto writeRsp = WaitFor(writeReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(*writeRsp);

    auto commitResult = WaitFor(transaction->Commit());
    THROW_ERROR_EXCEPTION_IF_FAILED(commitResult);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
