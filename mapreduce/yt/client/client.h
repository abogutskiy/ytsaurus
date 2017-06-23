#pragma once

#include <mapreduce/yt/interface/client.h>
#include <mapreduce/yt/http/requests.h>
#include <mapreduce/yt/io/client_reader.h>
#include <mapreduce/yt/io/client_writer.h>

namespace NYT {

namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

class TClientBase
    : virtual public IClientBase
{
public:
    explicit TClientBase(
        const TAuth& auth,
        const TTransactionId& transactionId);

    ITransactionPtr StartTransaction(
        const TStartTransactionOptions& options) override;

    // cypress

    TNodeId Create(
        const TYPath& path,
        ENodeType type,
        const TCreateOptions& options) override;

    void Remove(
        const TYPath& path,
        const TRemoveOptions& options) override;

    bool Exists(const TYPath& path) override;

    TNode Get(
        const TYPath& path,
        const TGetOptions& options) override;

    void Set(
        const TYPath& path,
        const TNode& value) override;

    TNode::TList List(
        const TYPath& path,
        const TListOptions& options) override;

    TNodeId Copy(
        const TYPath& sourcePath,
        const TYPath& destinationPath,
        const TCopyOptions& options) override;

    TNodeId Move(
        const TYPath& sourcePath,
        const TYPath& destinationPath,
        const TMoveOptions& options) override;

    TNodeId Link(
        const TYPath& targetPath,
        const TYPath& linkPath,
        const TLinkOptions& options) override;

    void Concatenate(
        const yvector<TYPath>& sourcePaths,
        const TYPath& destinationPath,
        const TConcatenateOptions& options) override;

    IFileReaderPtr CreateFileReader(
        const TRichYPath& path,
        const TFileReaderOptions& options) override;

    IFileWriterPtr CreateFileWriter(
        const TRichYPath& path,
        const TFileWriterOptions& options) override;

    TRawTableReaderPtr CreateRawReader(
        const TRichYPath& path,
        EDataStreamFormat format,
        const TTableReaderOptions& options,
        const TString& formatConfig) override;

    TRawTableWriterPtr CreateRawWriter(
        const TRichYPath& path,
        EDataStreamFormat format,
        const TTableWriterOptions& options,
        const TString& formatConfig) override;

    // operations

    TOperationId DoMap(
        const TMapOperationSpec& spec,
        IJob* mapper,
        const TOperationOptions& options) override;

    TOperationId DoReduce(
        const TReduceOperationSpec& spec,
        IJob* reducer,
        const TOperationOptions& options) override;

    TOperationId DoJoinReduce(
        const TJoinReduceOperationSpec& spec,
        IJob* reducer,
        const TOperationOptions& options) override;

    TOperationId DoMapReduce(
        const TMapReduceOperationSpec& spec,
        IJob* mapper,
        IJob* reduceCombiner,
        IJob* reducer,
        const TMultiFormatDesc& outputMapperDesc,
        const TMultiFormatDesc& inputReduceCombinerDesc,
        const TMultiFormatDesc& outputReduceCombinerDesc,
        const TMultiFormatDesc& inputReducerDesc,
        const TOperationOptions& options) override;

    TOperationId Sort(
        const TSortOperationSpec& spec,
        const TOperationOptions& options) override;

    TOperationId Merge(
        const TMergeOperationSpec& spec,
        const TOperationOptions& options) override;

    TOperationId Erase(
        const TEraseOperationSpec& spec,
        const TOperationOptions& options) override;

    EOperationStatus CheckOperation(const TOperationId& operationId) override;

    void AbortOperation(const TOperationId& operationId) override;

    void WaitForOperation(const TOperationId& operationId) override;

    void AlterTable(
        const TYPath& path,
        const TAlterTableOptions& options) override;

protected:
    const TAuth Auth_;
    TTransactionId TransactionId_;

private:
    ::TIntrusivePtr<TClientReader> CreateClientReader(
        const TRichYPath& path,
        EDataStreamFormat format,
        const TTableReaderOptions& options,
        const TString& formatConfig = TString());

    THolder<TClientWriter> CreateClientWriter(
        const TRichYPath& path,
        EDataStreamFormat format,
        const TTableWriterOptions& options,
        const TString& formatConfig = TString());

    ::TIntrusivePtr<INodeReaderImpl> CreateNodeReader(
        const TRichYPath& path, const TTableReaderOptions& options) override;

    ::TIntrusivePtr<IYaMRReaderImpl> CreateYaMRReader(
        const TRichYPath& path, const TTableReaderOptions& options) override;

    ::TIntrusivePtr<IProtoReaderImpl> CreateProtoReader(
        const TRichYPath& path,
        const TTableReaderOptions& options,
        const Message* prototype) override;

    ::TIntrusivePtr<INodeWriterImpl> CreateNodeWriter(
        const TRichYPath& path, const TTableWriterOptions& options) override;

    ::TIntrusivePtr<IYaMRWriterImpl> CreateYaMRWriter(
        const TRichYPath& path, const TTableWriterOptions& options) override;

    ::TIntrusivePtr<IProtoWriterImpl> CreateProtoWriter(
        const TRichYPath& path,
        const TTableWriterOptions& options,
        const Message* prototype) override;

    // Raw table writer buffer size
    static const size_t BUFFER_SIZE;
};

////////////////////////////////////////////////////////////////////////////////

class TTransaction
    : public ITransaction
    , public TClientBase
{
public:
    TTransaction(
        const TAuth& auth,
        const TTransactionId& transactionId,
        bool isOwning,
        const TStartTransactionOptions& options);

    const TTransactionId& GetId() const override;

    ILockPtr Lock(
        const TYPath& path,
        ELockMode mode,
        const TLockOptions& options) override;

    void Commit() override;

    void Abort() override;

private:
    THolder<TPingableTransaction> PingableTx_;
};

////////////////////////////////////////////////////////////////////////////////

class TClient
    : public IClient
    , public TClientBase
{
public:
    TClient(
        const TAuth& auth,
        const TTransactionId& globalId);

    ITransactionPtr AttachTransaction(
        const TTransactionId& transactionId) override;

    void MountTable(
        const TYPath& path,
        const TMountTableOptions& options) override;

    void UnmountTable(
        const TYPath& path,
        const TUnmountTableOptions& options) override;

    void RemountTable(
        const TYPath& path,
        const TRemountTableOptions& options) override;

    void ReshardTable(
        const TYPath& path,
        const yvector<TKey>& keys,
        const TReshardTableOptions& options) override;

    void ReshardTable(
        const TYPath& path,
        i32 tabletCount,
        const TReshardTableOptions& options) override;

    void InsertRows(
        const TYPath& path,
        const TNode::TList& rows,
        const TInsertRowsOptions& options) override;

    void DeleteRows(
        const TYPath& path,
        const TNode::TList& keys,
        const TDeleteRowsOptions& options) override;

    TNode::TList LookupRows(
        const TYPath& path,
        const TNode::TList& keys,
        const TLookupRowsOptions& options) override;

    TNode::TList SelectRows(
        const TString& query,
        const TSelectRowsOptions& options) override;

    void EnableTableReplica(const TReplicaId& replicaid) override;

    void DisableTableReplica(const TReplicaId& replicaid) override;

    ui64 GenerateTimestamp() override;

    void ExecuteBatch(const TBatchRequest& request, const TExecuteBatchOptions& options) override;

private:
    template <class TOptions>
    void SetTabletParams(
        THttpHeader& header,
        const TYPath& path,
        const TOptions& options);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NYT
