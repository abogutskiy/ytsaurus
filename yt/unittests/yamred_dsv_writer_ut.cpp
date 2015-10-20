#include "stdafx.h"
#include "framework.h"

#include <ytlib/table_client/unversioned_row.h>
#include <ytlib/table_client/name_table.h>

#include <ytlib/formats/yamred_dsv_writer.h>

#include <core/concurrency/async_stream.h>

#include <util/string/vector.h>

namespace NYT {
namespace NFormats {
namespace {

////////////////////////////////////////////////////////////////////////////////

using namespace NYTree;
using namespace NYson;
using namespace NConcurrency;
using namespace NTableClient;

class TShemalessWriterForYamredDsvTest
    : public ::testing::Test
{
protected:
    TNameTablePtr NameTable_;
    TYamredDsvFormatConfigPtr Config_;
    TShemalessWriterForYamredDsvPtr Writer_;

    TStringStream OutputStream_;

    int KeyAId_;
    int KeyBId_;
    int KeyCId_;
    int ValueXId_;
    int ValueYId_;
    
    TShemalessWriterForYamredDsvTest() 
    {
        NameTable_ = New<TNameTable>();
        KeyAId_ = NameTable_->RegisterName("key_a");
        KeyBId_ = NameTable_->RegisterName("key_b");
        KeyCId_ = NameTable_->RegisterName("key_c");
        ValueXId_ = NameTable_->RegisterName("value_x");
        ValueYId_ = NameTable_->RegisterName("value_y");
        Config_ = New<TYamredDsvFormatConfig>();
    }

    void CreateStandardWriter() 
    {
        Writer_ = New<TShemalessWriterForYamredDsv>(
            NameTable_,
            CreateAsyncAdapter(static_cast<TOutputStream*>(&OutputStream_)),
            false, // enableContextSaving  
            false, // enableKeySwitch
            0, // keyColumnCount
            Config_);
    }

    // Splits output into key and sorted vector of values that are entries of the last YAMR column.
    // Returns true if success (there are >= 2 values after splitting by field separator), otherwise false.
    bool ExtractKeyValue(Stroka output, Stroka& key, VectorStrok& value, char fieldSeparator = '\t') 
    {
        char delimiter[2] = {fieldSeparator, 0}; 
        value = splitStroku(output, delimiter, 0 /* maxFields */, KEEP_EMPTY_TOKENS); // Splitting by field separator.
        if (value.size() < 2) // We should at least have key and the rest of values.
            return false;
        key = value[0];
        value.erase(value.begin());
        std::sort(value.begin(), value.end());
        return true;
    }

    // The same function as previous, version with subkey.
    bool ExtractKeySubkeyValue(Stroka output, Stroka& key, Stroka& subkey, VectorStrok& value, char fieldSeparator = '\t')
    {
        char delimiter[2] = {fieldSeparator, 0}; 
        value = splitStroku(output, delimiter, 0 /* maxFields */, KEEP_EMPTY_TOKENS); // Splitting by field separator.
        if (value.size() < 3) // We should at least have key, subkey and the rest of values.
           return false;
        key = value[0];
        subkey = value[1];
        value.erase(value.begin(), value.end());
        std::sort(value.begin(), value.end());
        return true; 
    }

    // Compares output and expected output ignoring the order of entries in YAMR value column.
    void CompareKeyValue(Stroka output, Stroka expected, char recordSeparator = '\n', char fieldSeparator = '\t')
    {
        char delimiter[2] = {recordSeparator, 0};
        VectorStrok outputRows = splitStroku(output, delimiter, 0 /* maxFields */ , KEEP_EMPTY_TOKENS);
        VectorStrok expectedRows = splitStroku(expected, delimiter, 0 /* maxFields */, KEEP_EMPTY_TOKENS);
        EXPECT_EQ(outputRows.size(), expectedRows.size());
        // Since there is \n after each row, there will be an extra empty string in both vectors.
        EXPECT_EQ(outputRows.back(), "");
        ASSERT_EQ(expectedRows.back(), "");
        outputRows.pop_back();
        expectedRows.pop_back();
        
        Stroka outputKey;
        Stroka expectedKey;
        VectorStrok outputValue;
        VectorStrok expectedValue;
        for (int rowIndex = 0; rowIndex < static_cast<int>(outputRows.size()); rowIndex++) {
            EXPECT_TRUE(ExtractKeyValue(outputRows[rowIndex], outputKey, outputValue, fieldSeparator));
            ASSERT_TRUE(ExtractKeyValue(expectedRows[rowIndex], expectedKey, expectedValue, fieldSeparator));
            EXPECT_EQ(outputKey, expectedKey);
            EXPECT_EQ(outputValue, expectedValue);
        }
    }

    // The same function as previous, version with subkey.
    void CompareKeySubkeyValue(Stroka output, Stroka expected, char recordSeparator = '\n', char fieldSeparator = '\t')
    {
        char delimiter[2] = {recordSeparator, 0};
        VectorStrok outputRows = splitStroku(output, delimiter, 0 /* maxFields */ , KEEP_EMPTY_TOKENS);
        VectorStrok expectedRows = splitStroku(expected, delimiter, 0 /* maxFields */, KEEP_EMPTY_TOKENS);
        EXPECT_EQ(outputRows.size(), expectedRows.size());
        // Since there is \n after each row, there will be an extra empty string in both vectors.
        EXPECT_EQ(outputRows.back(), "");
        ASSERT_EQ(expectedRows.back(), "");
        outputRows.pop_back();
        expectedRows.pop_back();
        
        Stroka outputKey;
        Stroka expectedKey;
        Stroka outputSubkey;
        Stroka expectedSubkey;
        VectorStrok outputValue;
        VectorStrok expectedValue;
        for (int rowIndex = 0; rowIndex < static_cast<int>(outputRows.size()); rowIndex++) {
            EXPECT_TRUE(ExtractKeySubkeyValue(outputRows[rowIndex], outputKey, outputSubkey, outputValue, fieldSeparator));
            ASSERT_TRUE(ExtractKeySubkeyValue(expectedRows[rowIndex], expectedKey, expectedSubkey, expectedValue, fieldSeparator));
            EXPECT_EQ(outputKey, expectedKey);
            EXPECT_EQ(outputSubkey, expectedSubkey);
            EXPECT_EQ(outputValue, expectedValue);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TEST_F(TShemalessWriterForYamredDsvTest, Simple) 
{
    Config_->KeyColumnNames.emplace_back("key_a");
    CreateStandardWriter();

    TUnversionedRowBuilder row1;
    row1.AddValue(MakeUnversionedStringValue("a1", KeyAId_));
    row1.AddValue(MakeUnversionedStringValue("x", ValueXId_));
    row1.AddValue(MakeUnversionedSentinelValue(EValueType::Null, ValueYId_));

    TUnversionedRowBuilder row2;
    row2.AddValue(MakeUnversionedStringValue("a2", KeyAId_));
    row2.AddValue(MakeUnversionedStringValue("y", ValueYId_));
    row2.AddValue(MakeUnversionedStringValue("b", KeyBId_));
    
    std::vector<TUnversionedRow> rows = {row1.GetRow(), row2.GetRow()};
    
    EXPECT_EQ(true, Writer_->Write(rows));
    Writer_->Close()
        .Get()
        .ThrowOnError();

    Stroka expectedOutput =
        "a1\tvalue_x=x\n"
        "a2\tvalue_y=y\tkey_b=b\n";
   
    Stroka output = OutputStream_.Str(); 

    CompareKeyValue(output, expectedOutput); 
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TShemalessWriterForYamredDsvTest, SimpleWithSubkey)
{
    Config_->HasSubkey = true;
    Config_->KeyColumnNames.emplace_back("key_a");
    Config_->KeyColumnNames.emplace_back("key_b");
    Config_->SubkeyColumnNames.emplace_back("key_c");
    CreateStandardWriter();

    TUnversionedRowBuilder row1;
    row1.AddValue(MakeUnversionedStringValue("a", KeyAId_));
    row1.AddValue(MakeUnversionedStringValue("b1", KeyBId_));
    row1.AddValue(MakeUnversionedStringValue("c", KeyCId_));

    TUnversionedRowBuilder row2;
    row2.AddValue(MakeUnversionedStringValue("a", KeyAId_));
    row2.AddValue(MakeUnversionedStringValue("b2", KeyBId_));
    row2.AddValue(MakeUnversionedStringValue("c", KeyCId_));
    
    std::vector<TUnversionedRow> rows = {row1.GetRow(), row2.GetRow()};
   
    EXPECT_EQ(true, Writer_->Write(rows));
    Writer_->Close()
        .Get()
        .ThrowOnError();

    Stroka expectedOutput =
        "a b1\tc\t\n"
        "a b2\tc\t\n";
   
    Stroka output = OutputStream_.Str(); 

    CompareKeySubkeyValue(output, expectedOutput); 
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TShemalessWriterForYamredDsvTest, Lenval)
{
    Config_->Lenval = true;
    Config_->HasSubkey = true;
    Config_->EnableTableIndex = true;
    Config_->KeyColumnNames.emplace_back("key_a");
    Config_->KeyColumnNames.emplace_back("key_b");
    Config_->SubkeyColumnNames.emplace_back("key_c");
    CreateStandardWriter();

    TUnversionedRowBuilder row1;
    row1.AddValue(MakeUnversionedStringValue("a", KeyAId_));
    row1.AddValue(MakeUnversionedStringValue("b1", KeyBId_));
    row1.AddValue(MakeUnversionedStringValue("c", KeyCId_));
    row1.AddValue(MakeUnversionedStringValue("x", ValueXId_));


    TUnversionedRowBuilder row2;
    row2.AddValue(MakeUnversionedStringValue("a", KeyAId_));
    row2.AddValue(MakeUnversionedStringValue("b2", KeyBId_));
    row2.AddValue(MakeUnversionedStringValue("c", KeyCId_));
    
    std::vector<TUnversionedRow> rows = {row1.GetRow(), row2.GetRow()};
   
    Writer_->WriteTableIndex(42);
    Writer_->WriteRangeIndex(23);
    EXPECT_EQ(true, Writer_->Write(rows));
    Writer_->WriteRowIndex(17);

    Writer_->Close()
        .Get()
        .ThrowOnError();

    // ToDo(makhmedov): compare Yamr values ignoring the order of entries.
    Stroka expectedOutput = Stroka(
        "\xff\xff\xff\xff" "\x2a\x00\x00\x00" // Table index.
        "\xfd\xff\xff\xff" "\x17\x00\x00\x00" // Row index.

        "\x04\x00\x00\x00" "a b1"
        "\x01\x00\x00\x00" "c"
        "\x09\x00\x00\x00" "value_x=x"

        "\x04\x00\x00\x00" "a b2"
        "\x01\x00\x00\x00" "c"
        "\x00\x00\x00\x00" ""

        "\xfc\xff\xff\xff" "\x11\x00\x00\x00\x00\x00\x00\x00",
        13 * 4 + 4 + 1 + 9 + 4 + 1 + 0
    );

    Stroka output = OutputStream_.Str(); 
    
    EXPECT_EQ(output, expectedOutput);
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TShemalessWriterForYamredDsvTest, Escaping)
{
    Config_->KeyColumnNames.emplace_back("key_a");
    Config_->KeyColumnNames.emplace_back("key_b");
    int columnWithEscapedNameId = NameTable_->GetIdOrRegisterName("value\t_t");
    CreateStandardWriter();
    
    TUnversionedRowBuilder row1;
    row1.AddValue(MakeUnversionedStringValue("a\n", KeyAId_));
    row1.AddValue(MakeUnversionedStringValue("\nb\t", KeyBId_));
    row1.AddValue(MakeUnversionedStringValue("\nva\\lue\t", columnWithEscapedNameId));
 
    std::vector<TUnversionedRow> rows = {row1.GetRow()};

    EXPECT_EQ(true, Writer_->Write(rows));
    Writer_->Close()
        .Get()
        .ThrowOnError();
    
    Stroka expectedOutput = "a\\n \\nb\\t\tvalue\\t_t=\\nva\\\\lue\\t\n";
    Stroka output = OutputStream_.Str();

    EXPECT_EQ(output, expectedOutput);
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TShemalessWriterForYamredDsvTest, SkippedKey)
{
    Config_->KeyColumnNames.emplace_back("key_a");
    Config_->KeyColumnNames.emplace_back("key_b");
    CreateStandardWriter();

    TUnversionedRowBuilder row;
    row.AddValue(MakeUnversionedStringValue("b", KeyBId_));

    std::vector<TUnversionedRow> rows = { row.GetRow() };

    EXPECT_FALSE(Writer_->Write(rows));

    auto callGetReadyEvent = [&]() {
        Writer_->Close()
            .Get()
            .ThrowOnError();
    };
    EXPECT_THROW(callGetReadyEvent(), std::exception);
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TShemalessWriterForYamredDsvTest, SkippedSubkey)
{
    Config_->HasSubkey = true;
    Config_->KeyColumnNames.emplace_back("key_a");
    Config_->SubkeyColumnNames.emplace_back("key_c");
    CreateStandardWriter();

    TUnversionedRowBuilder row;
    row.AddValue(MakeUnversionedStringValue("a", KeyAId_));

    std::vector<TUnversionedRow> rows = { row.GetRow() };

    EXPECT_FALSE(Writer_->Write(rows));

    auto callGetReadyEvent = [&]() {
        Writer_->Close()
            .Get()
            .ThrowOnError();
    };
    EXPECT_THROW(callGetReadyEvent(), std::exception);
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TShemalessWriterForYamredDsvTest, ErasingSubkeyColumnsWhenHasSubkeyIsFalse)
{
    Config_->KeyColumnNames.emplace_back("key_a");
    Config_->SubkeyColumnNames.emplace_back("key_b");
    // Config->HasSubkey = false by default.
    CreateStandardWriter();
    
    TUnversionedRowBuilder row1;
    row1.AddValue(MakeUnversionedStringValue("a", KeyAId_));
    row1.AddValue(MakeUnversionedStringValue("b", KeyBId_));
    row1.AddValue(MakeUnversionedStringValue("c", KeyCId_));
    row1.AddValue(MakeUnversionedStringValue("x", ValueXId_));
 
    std::vector<TUnversionedRow> rows = {row1.GetRow()};

    EXPECT_EQ(true, Writer_->Write(rows));
    Writer_->Close()
        .Get()
        .ThrowOnError();
    
    Stroka expectedOutput = "a\tkey_c=c\tvalue_x=x\n";
    Stroka output = OutputStream_.Str();

    EXPECT_EQ(output, expectedOutput);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NFormats
} // namespace NYT
