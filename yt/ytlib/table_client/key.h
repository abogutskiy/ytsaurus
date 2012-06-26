﻿#pragma once

#include "public.h"
#include "size_limits.h"

#include <ytlib/misc/small_vector.h>
#include <ytlib/misc/property.h>
#include <ytlib/misc/foreach.h>
#include <ytlib/misc/string.h>
#include <ytlib/misc/nullable.h>
#include <ytlib/misc/blob_output.h>
#include <ytlib/ytree/lexer.h>
#include <ytlib/table_client/table_chunk_meta.pb.h>
#include <ytlib/table_client/table_reader.pb.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EKeyPartType,
    // A special sentinel used by #GetKeySuccessor.
    ((MinSentinel)(-1))
    // Denotes a missing (null) component in a composite key.
    ((Null)(0))
    // Integer value.
    ((Integer)(1))
    // Floating-point value.
    ((Double)(2))
    // String value.
    ((String)(3))
    // Any structured value.
    ((Composite)(4))
);

////////////////////////////////////////////////////////////////////////////////

template <class TStrType>
class TKeyPart
{
    DEFINE_BYVAL_RO_PROPERTY(EKeyPartType, Type);

public:
    TKeyPart()
        : Type_(EKeyPartType::Null)
    { }

    static TKeyPart CreateNull()
    {
        TKeyPart result;
        result.Type_ = EKeyPartType::Null;
        return result;
    }

    static TKeyPart CreateComposite()
    {
        TKeyPart result;
        result.Type_ = EKeyPartType::Composite;
        return result;
    }

    static TKeyPart CreateValue(const TStrType& value)
    {
        TKeyPart result;
        result.Type_ = EKeyPartType::String;
        result.StrValue = value;
        return result;
    }

    static TKeyPart CreateValue(i64 value)
    {
        TKeyPart result;
        result.Type_ = EKeyPartType::Integer;
        result.IntValue = value;
        return result;
    }

    static TKeyPart CreateValue(double value)
    {
        TKeyPart result;
        result.Type_ = EKeyPartType::Double;
        result.DoubleValue = value;
        return result;
    }

    i64 GetInteger() const
    {
        YASSERT(Type_ == EKeyPartType::Integer);
        return IntValue;
    }

    double GetDouble() const
    {
        YASSERT(Type_ == EKeyPartType::Double);
        return DoubleValue;
    }

    const char* Begin() const 
    {
        YASSERT(Type_ == EKeyPartType::String);
        return StrValue.begin();
    }

    size_t GetStringSize() const 
    {
        YASSERT(Type_ == EKeyPartType::String);
        return StrValue.size();
    }

    TStringBuf GetString() const
    {
        YASSERT(Type_ == EKeyPartType::String);
        return TStringBuf(StrValue.begin(), StrValue.size());
    }

    size_t GetSize() const
    {
        auto typeSize = sizeof(Type_);

        switch (Type_) {
        case EKeyPartType::String:
            return typeSize + StrValue.size();
        case EKeyPartType::Integer:
            return typeSize + sizeof(i64);
        case EKeyPartType::Double:
            return typeSize + sizeof(double);
        default:
            return typeSize;
        }
    }

    //! Converts the part into protobuf.
    //! Trims string part length to #maxSize if it exceeds the limit.
    NProto::TKeyPart ToProto(size_t maxSize = 0) const
    {
        NProto::TKeyPart keyPart;
        keyPart.set_type(Type_);

        switch (Type_) {
            case EKeyPartType::String:
                keyPart.set_str_value(StrValue.begin(), StrValue.size());
                break;

            case EKeyPartType::Integer:
                keyPart.set_int_value(IntValue);
                break;

            case EKeyPartType::Double:
                keyPart.set_double_value(DoubleValue);
                break;

            case EKeyPartType::MinSentinel:
            case EKeyPartType::Null:
            case EKeyPartType::Composite:
                break;

            default:
                YUNREACHABLE();
        }

        return keyPart;
    }

private:
    // The actual value. 
    i64 IntValue;
    double DoubleValue;
    TStrType StrValue;

};

////////////////////////////////////////////////////////////////////////////////

template <class TStrType>
Stroka ToString(const NYT::NTableClient::TKeyPart<TStrType>& keyPart)
{
    switch (keyPart.GetType()) {
        case NYT::NTableClient::EKeyPartType::Null:
            return "<Null>";
        case NYT::NTableClient::EKeyPartType::Composite:
            return "<Composite>";
        case NYT::NTableClient::EKeyPartType::MinSentinel:
            return "<Min>";
        case NYT::NTableClient::EKeyPartType::String:
            return keyPart.GetString().ToString().Quote();
        case NYT::NTableClient::EKeyPartType::Integer:
            return ::ToString(keyPart.GetInteger());
        case NYT::NTableClient::EKeyPartType::Double:
            return ::ToString(keyPart.GetDouble());
        default:
            YUNREACHABLE();
    }
}

template <class TLhsStrType, class TRhsStrType>
int CompareKeyParts(const TKeyPart<TLhsStrType>& lhs, const TKeyPart<TRhsStrType>& rhs)
{
    if (rhs.GetType() != lhs.GetType()) {
        return static_cast<int>(lhs.GetType()) - static_cast<int>(rhs.GetType());
    }

    switch (rhs.GetType()) {
    case EKeyPartType::String: {
        size_t minLen = std::min(lhs.GetStringSize(), rhs.GetStringSize());
        auto res = strncmp(lhs.Begin(), rhs.Begin(), minLen);
        return res ? res : static_cast<int>(lhs.GetStringSize()) - static_cast<int>(rhs.GetStringSize());
        // Too slow because of allocations.
        // return lhs.GetString().compare(rhs.GetString());
    }

    case EKeyPartType::Integer:
        if (lhs.GetInteger() > rhs.GetInteger())
            return 1;
        if (lhs.GetInteger() < rhs.GetInteger())
            return -1;
        return 0;

    case EKeyPartType::Double:
        if (lhs.GetDouble() > rhs.GetDouble())
            return 1;
        if (lhs.GetDouble() < rhs.GetDouble())
            return -1;
        return 0;

    case EKeyPartType::Null:
    case EKeyPartType::Composite:
        return 0; // All composites are equal to each other.

    default:
        YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

template <class TBuffer>
class TKey;

template <class TBuffer>
Stroka ToString(const NYT::NTableClient::TKey<TBuffer>& key);

//ToDo(psushin): make key-inl.h

template <class TBuffer>
class TKey
{
public:
    //! Creates an empty key.
    /* 
     *  \param maxSize maximum string key size.
     */
    explicit TKey(int columnCount = 0)
        : ColumnCount(columnCount)
        , Buffer(columnCount * MaxKeySize)
        , Parts(ColumnCount)
    { }

    template<class TOtherBuffer>
    TKey(const TKey<TOtherBuffer>& other)
        : ColumnCount(other.ColumnCount)
        , Buffer(ColumnCount * MaxKeySize)
        , Parts(ColumnCount)
    {
        *this = other;
    }

    template<class TOtherBuffer>
    TKey<TBuffer>& operator=(const TKey<TOtherBuffer>& other)
    {
        Assign(other);
        return *this;
    }

    TKey<TBuffer>& operator=(const TKey<TBuffer>& other)
    {
        if (this != &other) {
            Assign(other);
        }
        return *this;
    }

    void SetValue(int index, i64 value)
    {
        YASSERT(index < ColumnCount);
        Parts[index] = TKeyPart<typename TBuffer::TStoredType>::CreateValue(value);
    }

    void SetValue(int index, double value)
    {
        YASSERT(index < ColumnCount);
        Parts[index] = TKeyPart<typename TBuffer::TStoredType>::CreateValue(value);
    }

    void SetValue(int index, const TStringBuf& value)
    {
        YASSERT(index < ColumnCount);

        // Strip long values.
        int length = std::min(MaxKeySize - sizeof(EKeyPartType), value.size());

        auto part = Buffer.PutData(TStringBuf(value.begin(), length));

        Parts[index] = TKeyPart<typename TBuffer::TStoredType>::CreateValue(part);
    }

    void SetComposite(int index)
    {
        YASSERT(index < ColumnCount);
        Parts[index] = TKeyPart<typename TBuffer::TStoredType>::CreateComposite();
    }

    void Reset(int columnCount = -1)
    {
        if (columnCount >= 0)
            ColumnCount = columnCount;

        Buffer.Clear();
        Parts.clear();
        Parts.resize(ColumnCount);
    }

    size_t GetSize() const
    {
        size_t result = 0;
        FOREACH (const auto& part, Parts) {
            result += part.GetSize();
        }
        return std::min(result, MaxKeySize);
    }

    NProto::TKey ToProto() const
    {
        NProto::TKey key;
        size_t currentSize = 0;
        FOREACH (const auto& part, Parts) {
            if (currentSize < MaxKeySize) {
                *key.add_parts() = part.ToProto(MaxKeySize - currentSize);
                currentSize += part.GetSize();
            } else {
                *key.add_parts() = TKeyPart<typename TBuffer::TStoredType>::CreateNull().ToProto();
            }
        }
        return key;
    }

    static TKey FromProto(const NProto::TKey& protoKey)
    {
        TKey key;
        key.Reset(protoKey.parts_size());
        for (int i = 0; i < protoKey.parts_size(); ++i) {
            switch (protoKey.parts(i).type()) {
                case EKeyPartType::Composite:
                    key.SetComposite(i);
                    break;

                case EKeyPartType::Double:
                    key.SetValue(i, protoKey.parts(i).double_value());
                    break;

                case EKeyPartType::Integer:
                    key.SetValue(i, protoKey.parts(i).int_value());
                    break;

                case EKeyPartType::String:
                    key.SetValue(i, protoKey.parts(i).str_value());
                    break;

                case EKeyPartType::Null:
                    break;

                default:
                    YUNREACHABLE();
                }
        }
        return key;
    }

    void SetKeyPart(int index, const TStringBuf& yson, NYTree::TLexer& lexer)
    {
        lexer.Reset();
        YVERIFY(lexer.Read(yson) > 0);
        YASSERT(lexer.GetState() == NYTree::TLexer::EState::Terminal);

        const auto& token = lexer.GetToken();
        switch (token.GetType()) {
            case NYTree::ETokenType::Integer:
                SetValue(index, token.GetIntegerValue());
                break;

            case NYTree::ETokenType::String:
                SetValue(index, token.GetStringValue());
                break;

            case NYTree::ETokenType::Double:
                SetValue(index, token.GetDoubleValue());
                break;

            default:
                SetComposite(index);
                break;
        }
    }

private:
    template <class TLhsBuffer, class TRhsBuffer>
    friend int CompareKeys(const TKey<TLhsBuffer>& lhs, const TKey<TRhsBuffer>& rhs, int prefixLength);

    friend Stroka ToString<>(const TKey& key);

    // This is required for correct compilation of operator =.
    template <class TOtherBuffer>
    friend class TKey;

    int ColumnCount;

    TSmallVector< TKeyPart<typename TBuffer::TStoredType>, 4> Parts;
    TBuffer Buffer;

    template <class TOtherBuffer>
    void Assign(const TKey<TOtherBuffer>& other)
    {
        Reset(other.Parts.size());
        for (int i = 0; i < other.Parts.size(); ++i) {
            switch (other.Parts[i].GetType()) {
            case EKeyPartType::Composite:
                SetComposite(i);
                break;

            case EKeyPartType::Integer:
                SetValue(i, other.Parts[i].GetInteger());
                break;

            case EKeyPartType::Double:
                SetValue(i, other.Parts[i].GetDouble());
                break;

            case EKeyPartType::String:
                SetValue(i, other.Parts[i].GetString());
                break;

            case EKeyPartType::Null:
                // Do nothing.
                break;

            default:
                YUNREACHABLE();
            }
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class TBuffer>
Stroka ToString(const TKey<TBuffer>& key)
{
    return "[" + JoinToString(key.Parts) + "]";
}

//! Compares given keys (truncated to #prefixLength). Returns zero if |lhs == rhs|, a negative value
//! if |lhs < rhs| and a positive value otherwise.
template <class TLhsBuffer, class TRhsBuffer>
int CompareKeys(const TKey<TLhsBuffer>& lhs, const TKey<TRhsBuffer>& rhs, int prefixLength = std::numeric_limits<int>::max())
{
    int lhsSize = std::min(static_cast<int>(lhs.Parts.size()), prefixLength);
    int rhsSize = std::min(static_cast<int>(rhs.Parts.size()), prefixLength);
    int minSize = std::min(lhsSize, rhsSize);
    for (int index = 0; index < minSize; ++index) {
        int result = CompareKeyParts(lhs.Parts[index], rhs.Parts[index]);
        if (result != 0) {
            return result;
        }
    }
    return lhsSize - rhsSize;
}

int CompareKeys(const NProto::TKey& lhs, const NProto::TKey& rhs, int prefixLength = std::numeric_limits<int>::max());

bool operator >  (const NProto::TKey& lhs, const NProto::TKey& rhs);
bool operator >= (const NProto::TKey& lhs, const NProto::TKey& rhs);
bool operator <  (const NProto::TKey& lhs, const NProto::TKey& rhs);
bool operator <= (const NProto::TKey& lhs, const NProto::TKey& rhs);
bool operator == (const NProto::TKey& lhs, const NProto::TKey& rhs);

//! Returns the successor of |key|, i.e. the key
//! obtained from |key| by appending a sentinel part.
NProto::TKey GetSuccessorKey(const NProto::TKey& key);

//! Constructs a new chunk by slicing the original one and restricting
//! it to a given range. The original chunk may already contain non-trivial limits.
NProto::TInputChunk SliceChunk(
    const NProto::TInputChunk& chunk,
    const TNullable<NProto::TKey>& startKey = Null,
    const TNullable<NProto::TKey>& endKey = Null);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

