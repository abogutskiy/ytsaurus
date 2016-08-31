#include "client_writer.h"

#include "block_writer.h"

#include <mapreduce/yt/interface/io.h>
#include <mapreduce/yt/common/helpers.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TClientWriter::TClientWriter(
    const TRichYPath& path,
    const TAuth& auth,
    const TTransactionId& transactionId,
    EDataStreamFormat format,
    const Stroka& formatConfig,
    const TTableWriterOptions& options)
    : BlockWriter_(new TBlockWriter(
        auth,
        transactionId,
        GetWriteTableCommand(),
        format,
        formatConfig,
        path,
        BUFFER_SIZE,
        options))
{ }

size_t TClientWriter::GetStreamCount() const
{
    return 1;
}

TOutputStream* TClientWriter::GetStream(size_t tableIndex) const
{
    Y_UNUSED(tableIndex);
    return BlockWriter_.Get();
}

void TClientWriter::OnRowFinished(size_t)
{
    BlockWriter_->DoFlushIfNeeded();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
