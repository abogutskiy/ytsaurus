#pragma once

#include "proxy_input.h"

#include <mapreduce/yt/interface/io.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TLenvalTableReader
{
public:
    explicit TLenvalTableReader(::TIntrusivePtr<TProxyInput> input);
    virtual ~TLenvalTableReader();

protected:
    bool IsValid() const;
    void Next();
    ui32 GetTableIndex() const;
    ui64 GetRowIndex() const;
    void NextKey();

    void CheckValidity() const;

    bool Retry();

    template <class T>
    bool ReadInteger(T* result, bool acceptEndOfStream = false)
    {
        size_t count = Input_->Load(result, sizeof(T));
        if (acceptEndOfStream && count == 0) {
            Finished_ = true;
            Valid_ = false;
            return false;
        }
        if (count != sizeof(T)) {
            ythrow yexception() << "Premature end of stream";
        }
        return true;
    }

    virtual void SkipRow() = 0;

protected:
    ::TIntrusivePtr<TProxyInput> Input_;

    bool Valid_ = true;
    bool Finished_ = false;
    ui32 TableIndex_ = 0;
    TMaybe<ui64> RowIndex_;
    TMaybe<ui32> RangeIndex_;
    bool AtStart_ = true;
    bool RowTaken_ = true;
    ui32 Length_ = 0;

private:
    bool PrepareRetry();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
