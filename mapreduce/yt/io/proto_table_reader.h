#pragma once

#include <mapreduce/yt/interface/io.h>

namespace NYT {

class TProxyInput;
class TNodeTableReader;

////////////////////////////////////////////////////////////////////////////////

class TProtoTableReader
    : public IProtoReaderImpl
{
public:
    explicit TProtoTableReader(THolder<TProxyInput> input);
    ~TProtoTableReader();

    virtual void ReadRow(Message* row) override;
    virtual void SkipRow() override;
    virtual bool IsValid() const override;
    virtual void Next() override;
    virtual ui32 GetTableIndex() const override;
    virtual ui64 GetRowIndex() const override;
    virtual void NextKey() override;

private:
    THolder<TNodeTableReader> NodeReader_; // proto over yson
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
