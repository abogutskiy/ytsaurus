#include "stdafx.h"
#include "yson_writer.h"

#include "yson_format.h"

#include <ytlib/misc/serialize.h>

#include <util/string/escape.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////
    
// Copied from <util/string/escape.cpp>
namespace {

static inline char HexDigit(char value) {
    YASSERT(value < 16);
    if (value < 10)
        return '0' + value;
    else
        return 'A' + value - 10;
}

static inline char OctDigit(char value) {
    YASSERT(value < 8);
    return '0' + value;
}

static inline bool IsPrintable(char c) {
    return c >= 32 && c <= 126;
}

static inline bool IsHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static inline bool IsOctDigit(char c) {
    return  c >= '0' && c <= '7';
}

static const size_t ESCAPE_C_BUFFER_SIZE = 4;

static inline size_t EscapeC(unsigned char c, char next, char r[ESCAPE_C_BUFFER_SIZE]) {
    // (1) Printable characters go as-is, except backslash and double quote.
    // (2) Characters \r, \n, \t and \0 ... \7 replaced by their simple escape characters (if possible).
    // (3) Otherwise, character is encoded using hexadecimal escape sequence (if possible), or octal.
    if (c == '\"') {
        r[0] = '\\';
        r[1] = '\"';
        return 2;
    } else if (c == '\\') {
        r[0] = '\\';
        r[1] = '\\';
        return 2;
    } else if (IsPrintable(c)) {
        r[0] = c;
        return 1;
    } else if (c == '\r') {
        r[0] = '\\';
        r[1] = 'r';
        return 2;
    } else if (c == '\n') {
        r[0] = '\\';
        r[1] = 'n';
        return 2;
    } else if (c == '\t') {
        r[0] = '\\';
        r[1] = 't';
        return 2;
   } else if (c < 8 && !IsOctDigit(next)) {
        r[0] = '\\';
        r[1] = OctDigit(c);
        return 2;
    } else if (!IsHexDigit(next)) {
        r[0] = '\\';
        r[1] = 'x';
        r[2] = HexDigit((c & 0xF0) >> 4);
        r[3] = HexDigit((c & 0x0F) >> 0);
        return 4;
    } else {
        r[0] = '\\';
        r[1] = OctDigit((c & 0700) >> 6);
        r[2] = OctDigit((c & 0070) >> 3);
        r[3] = OctDigit((c & 0007) >> 0);
        return 4;
    }
}

void EscapeC(const char* str, size_t len, TOutputStream& output) {
    char buffer[ESCAPE_C_BUFFER_SIZE];

    size_t i, j;
    for (i = 0, j = 0; i < len; ++i) {
        size_t rlen = EscapeC(str[i], (i + 1 < len ? str[i + 1] : 0), buffer);

        if (rlen > 1) {
            output.Write(str + j, i - j);
            j = i + 1;
            output.Write(buffer, rlen);
        }
    }

    if (j > 0) {
        output.Write(str + j, len - j);
    } else {
        output.Write(str, len);
    }
}

}

////////////////////////////////////////////////////////////////////////////////

TYsonWriter::TYsonWriter(
    TOutputStream* stream,
    EYsonFormat format,
    EYsonType type,
    bool formatRaw)
    : Stream(stream)
    , Format(format)
    , Type(type)
    , FormatRaw(formatRaw)
    , Depth(0)
    , BeforeFirstItem(true)
{
    YASSERT(stream);
}

void TYsonWriter::WriteIndent()
{
    for (int i = 0; i < IndentSize * Depth; ++i) {
        Stream->Write(' ');
    }
}

bool TYsonWriter::IsTopLevelFragmentContext() const
{
    return Depth == 0 && (Type == EYsonType::ListFragment || Type == EYsonType::KeyedFragment);
}

void TYsonWriter::EndNode()
{
    if (IsTopLevelFragmentContext()) {
        Stream->Write(TokenTypeToChar(ItemSeparatorToken));
        if (Format == EYsonFormat::Text || Format == EYsonFormat::Pretty) {
            Stream->Write('\n');
        }
    }
}

void TYsonWriter::BeginCollection(ETokenType beginToken)
{
    Stream->Write(TokenTypeToChar(beginToken));
    ++Depth;
    BeforeFirstItem = true;
}

void TYsonWriter::CollectionItem()
{
    if (!IsTopLevelFragmentContext()) {
        if (!BeforeFirstItem) {
            Stream->Write(TokenTypeToChar(ItemSeparatorToken));
        }

        if (Format == EYsonFormat::Pretty) {
            Stream->Write('\n');
            WriteIndent();
        }
    }

    BeforeFirstItem = false;
}

void TYsonWriter::EndCollection(ETokenType endToken)
{
    --Depth;
    if (Format == EYsonFormat::Pretty && !BeforeFirstItem) {
        Stream->Write('\n');
        WriteIndent();
    }
    Stream->Write(TokenTypeToChar(endToken));
    BeforeFirstItem = false;
}

void TYsonWriter::WriteStringScalar(const TStringBuf& value)
{
    if (Format == EYsonFormat::Binary) {
        Stream->Write(StringMarker);
        WriteVarInt32(Stream, static_cast<i32>(value.length()));
        Stream->Write(value.begin(), value.length());
    } else {
        Stream->Write('"');
        EscapeC(value.data(), value.length(), *Stream);
        Stream->Write('"');
    }
}

void TYsonWriter::OnStringScalar(const TStringBuf& value)
{
    WriteStringScalar(value);
    EndNode();
}

void TYsonWriter::OnIntegerScalar(i64 value)
{
    if (Format == EYsonFormat::Binary) {
        Stream->Write(IntegerMarker);
        WriteVarInt64(Stream, value);
    } else {
        Stream->Write(ToString(value));
    }
    EndNode();
}

void TYsonWriter::OnDoubleScalar(double value)
{
    if (Format == EYsonFormat::Binary) {
        Stream->Write(DoubleMarker);
        Stream->Write(&value, sizeof(double));
    } else {
        Stream->Write(ToString(value));
    }
    EndNode();
}

void TYsonWriter::OnEntity()
{
    Stream->Write(TokenTypeToChar(EntityToken));
    EndNode();
}

void TYsonWriter::OnBeginList()
{
    BeginCollection(BeginListToken);
}

void TYsonWriter::OnListItem()
{
    CollectionItem();
}

void TYsonWriter::OnEndList()
{
    EndCollection(EndListToken);
    EndNode();
}

void TYsonWriter::OnBeginMap()
{
    BeginCollection(BeginMapToken);
}

void TYsonWriter::OnKeyedItem(const TStringBuf& key)
{
    CollectionItem();

    WriteStringScalar(key);
    
    if (Format == EYsonFormat::Pretty) {
        Stream->Write(' ');
    }
    Stream->Write(TokenTypeToChar(KeyValueSeparatorToken));
    if (Format == EYsonFormat::Pretty) {
        Stream->Write(' ');
    }

    BeforeFirstItem = false;
}

void TYsonWriter::OnEndMap()
{
    EndCollection(EndMapToken);
    EndNode();
}

void TYsonWriter::OnBeginAttributes()
{
    BeginCollection(BeginAttributesToken);
}

void TYsonWriter::OnEndAttributes()
{
    EndCollection(EndAttributesToken);
    if (Format == EYsonFormat::Pretty) {
        Stream->Write(' ');
    }
}

void TYsonWriter::OnRaw(const TStringBuf& yson, EYsonType type)
{
    if (FormatRaw) {
        TYsonConsumerBase::OnRaw(yson, type);
    } else {
        Stream->Write(yson);
        BeforeFirstItem = false;
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
