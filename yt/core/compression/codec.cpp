#include "codec.h"
#include "details.h"
#include "lz.h"
#include "lzma.h"
#include "snappy.h"
#include "zlib.h"
#include "zstd.h"
#include "zstd_legacy.h"
#include "brotli.h"

namespace NYT {
namespace NCompression {

////////////////////////////////////////////////////////////////////////////////

template <class TCodec>
struct TCompressedBlockTag { };

template <class TCodec>
struct TDecompressedBlockTag { };

////////////////////////////////////////////////////////////////////////////////

class TCodecBase
    : public ICodec
{
protected:
    static size_t ZeroSizeEstimator(const std::vector<int>&)
    {
        return 0;
    }

    template <class TCodec>
    TSharedRef Run(
        TConverter converter,
        // TODO(ignat): change bool to enum
        bool compress,
        const TSharedRef& ref)
    {
        ByteArraySource input(ref.Begin(), ref.Size());
        auto blobCookie = compress
             ? GetRefCountedTypeCookie<TCompressedBlockTag<TCodec>>()
             : GetRefCountedTypeCookie<TDecompressedBlockTag<TCodec>>();
        auto outputBlob = TBlob(std::move(blobCookie), 0, false);
        converter(&input, &outputBlob);
        return TSharedRef::FromBlob(std::move(outputBlob));
    }

    template <class TCodec>
    TSharedRef Run(
        TConverter converter,
        bool compress,
        const std::vector<TSharedRef>& refs)
    {
        if (refs.size() == 1) {
            return Run<TCodec>(
                converter,
                compress,
                refs.front());
        }

        TVectorRefsSource input(refs);
        auto blobCookie = compress
              ? GetRefCountedTypeCookie<TCompressedBlockTag<TCodec>>()
              : GetRefCountedTypeCookie<TDecompressedBlockTag<TCodec>>();
        auto outputBlob = TBlob(blobCookie, 0, false);
        converter(&input, &outputBlob);
        return TSharedRef::FromBlob(std::move(outputBlob));
    }
};

////////////////////////////////////////////////////////////////////////////////

class TNoneCodec
    : public TCodecBase
{
public:
    virtual TSharedRef Compress(const TSharedRef& block) override
    {
        return block;
    }

    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) override
    {
        return MergeRefs(blocks);
    }

    virtual TSharedRef Decompress(const TSharedRef& block) override
    {
        return block;
    }

    virtual TSharedRef Decompress(const std::vector<TSharedRef>& blocks) override
    {
        return MergeRefs(blocks);
    }

    virtual ECodec GetId() const override
    {
        return ECodec::None;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TSnappyCodec
    : public TCodecBase
{
public:
    virtual TSharedRef Compress(const TSharedRef& block) override
    {
        return Run<TSnappyCodec>(NCompression::SnappyCompress, true, block);
    }

    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TSnappyCodec>(NCompression::SnappyCompress, true, blocks);
    }

    virtual TSharedRef Decompress(const TSharedRef& block) override
    {
        return Run<TSnappyCodec>(NCompression::SnappyDecompress, false, block);
    }

    virtual TSharedRef Decompress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TSnappyCodec>(NCompression::SnappyDecompress, false, blocks);
    }

    virtual ECodec GetId() const override
    {
        return ECodec::Snappy;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TZlibCodec
    : public TCodecBase
{
public:
    explicit TZlibCodec(int level)
        : Compressor_(std::bind(NCompression::ZlibCompress, level, std::placeholders::_1, std::placeholders::_2))
        , Level_(level)
    { }

    virtual TSharedRef Compress(const TSharedRef& block) override
    {
        return Run<TZlibCodec>(Compressor_, true, block);
    }

    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TZlibCodec>(Compressor_, true, blocks);
    }

    virtual TSharedRef Decompress(const TSharedRef& block) override
    {
        return Run<TZlibCodec>(NCompression::ZlibDecompress, false, block);
    }

    virtual TSharedRef Decompress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TZlibCodec>(NCompression::ZlibDecompress, false, blocks);
    }

    virtual ECodec GetId() const override
    {
        switch (Level_) {

#define CASE(level) case level: return PP_CONCAT(ECodec::Zlib_, level);
            PP_FOR_EACH(CASE, (1)(2)(3)(4)(5)(6)(7)(8)(9))
#undef CASE

            default:
                YUNREACHABLE();
        }
    }

private:
    const NCompression::TConverter Compressor_;
    const int Level_;
};

////////////////////////////////////////////////////////////////////////////////

class TLz4Codec
    : public TCodecBase
{
public:
    explicit TLz4Codec(bool highCompression)
        : Compressor_(std::bind(NCompression::Lz4Compress, highCompression, std::placeholders::_1, std::placeholders::_2))
        , CodecId_(highCompression ? ECodec::Lz4HighCompression : ECodec::Lz4)
    { }

    virtual TSharedRef Compress(const TSharedRef& block) override
    {
        return Run<TLz4Codec>(Compressor_, true, block);
    }

    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TLz4Codec>(Compressor_, true, blocks);
    }

    virtual TSharedRef Decompress(const TSharedRef& block) override
    {
        return Run<TLz4Codec>(NCompression::Lz4Decompress, false, block);
    }

    virtual TSharedRef Decompress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TLz4Codec>(NCompression::Lz4Decompress, false, blocks);
    }

    virtual ECodec GetId() const override
    {
        return CodecId_;
    }

private:
    const NCompression::TConverter Compressor_;
    const ECodec CodecId_;
};

////////////////////////////////////////////////////////////////////////////////

class TQuickLzCodec
    : public TCodecBase
{
public:
    TQuickLzCodec()
        : Compressor_(NCompression::QuickLzCompress)
    { }

    virtual TSharedRef Compress(const TSharedRef& block) override
    {
        return Run<TQuickLzCodec>(Compressor_, true, block);
    }

    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TQuickLzCodec>(Compressor_, true, blocks);
    }

    virtual TSharedRef Decompress(const TSharedRef& block) override
    {
        return Run<TQuickLzCodec>(NCompression::QuickLzDecompress, false, block);
    }

    virtual TSharedRef Decompress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TQuickLzCodec>(NCompression::QuickLzDecompress, false, blocks);
    }

    virtual ECodec GetId() const override
    {
        return ECodec::QuickLz;
    }

private:
    const NCompression::TConverter Compressor_;
};

////////////////////////////////////////////////////////////////////////////////

class TZstdLegacyCodec
    : public TCodecBase
{
public:
    virtual TSharedRef Compress(const TSharedRef& block) override
    {
        return Run<TZstdLegacyCodec>(NCompression::ZstdLegacyCompress, true, block);
    }

    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TZstdLegacyCodec>(NCompression::ZstdLegacyCompress, true, blocks);
    }

    virtual TSharedRef Decompress(const TSharedRef& block) override
    {
        return Run<TZstdLegacyCodec>(NCompression::ZstdLegacyDecompress, false, block);
    }

    virtual TSharedRef Decompress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TZstdLegacyCodec>(NCompression::ZstdLegacyDecompress, false, blocks);
    }

    virtual ECodec GetId() const override
    {
        return ECodec::Zstd;
    }
};

class TZstdCodec
    : public TCodecBase
{
public:
    TZstdCodec(int level)
        : Compressor_(std::bind(NCompression::ZstdCompress, level, std::placeholders::_1, std::placeholders::_2))
        , Level_(level)
    { }

    virtual TSharedRef Compress(const TSharedRef& block) override
    {
        return Run<TZstdCodec>(Compressor_, true, block);
    }

    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TZstdCodec>(Compressor_, true, blocks);
    }

    virtual TSharedRef Decompress(const TSharedRef& block) override
    {
        return Run<TZstdCodec>(NCompression::ZstdDecompress, false, block);
    }

    virtual TSharedRef Decompress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TZstdCodec>(NCompression::ZstdDecompress, false, blocks);
    }

    virtual ECodec GetId() const override
    {
        switch (Level_) {

#define CASE(level) case level: return PP_CONCAT(ECodec::Zstd_, level);
            PP_FOR_EACH(CASE, (1)(2)(3)(4)(5)(6)(7)(8)(9)(10)(11)(12)(13)(14)(15)(16)(17)(18)(19)(20)(21))
#undef CASE

            default:
                YUNREACHABLE();
        }
    }

private:
    const NCompression::TConverter Compressor_;
    const int Level_;
};

////////////////////////////////////////////////////////////////////////////////

class TBrotliCodec
    : public TCodecBase
{
public:
    TBrotliCodec(int level)
        : Compressor_(std::bind(NCompression::BrotliCompress, level, std::placeholders::_1, std::placeholders::_2))
        , Level_(level)
    { }

    virtual TSharedRef Compress(const TSharedRef& block) override
    {
        return Run<TBrotliCodec>(Compressor_, true, block);
    }

    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TBrotliCodec>(Compressor_, true, blocks);
    }

    virtual TSharedRef Decompress(const TSharedRef& block) override
    {
        return Run<TBrotliCodec>(NCompression::BrotliDecompress, false, block);
    }

    virtual TSharedRef Decompress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TBrotliCodec>(NCompression::BrotliDecompress, false, blocks);
    }

    virtual ECodec GetId() const override
    {
        switch (Level_) {

#define CASE(level) case level: return PP_CONCAT(ECodec::Brotli_, level);
            PP_FOR_EACH(CASE, (1)(2)(3)(4)(5)(6)(7)(8)(9)(10)(11))
#undef CASE

            default:
                YUNREACHABLE();
        }
    }

private:
    const NCompression::TConverter Compressor_;
    const int Level_;
};

class TLzmaCodec
    : public TCodecBase
{
public:
    TLzmaCodec(int level)
        : Compressor_(std::bind(NCompression::LzmaCompress, level, std::placeholders::_1, std::placeholders::_2))
        , Level_(level)
    { }

    virtual TSharedRef Compress(const TSharedRef& block) override
    {
        return Run<TLzmaCodec>(Compressor_, true, block);
    }

    virtual TSharedRef Compress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TLzmaCodec>(Compressor_, true, blocks);
    }

    virtual TSharedRef Decompress(const TSharedRef& block) override
    {
        return Run<TLzmaCodec>(NCompression::LzmaDecompress, false, block);
    }

    virtual TSharedRef Decompress(const std::vector<TSharedRef>& blocks) override
    {
        return Run<TLzmaCodec>(NCompression::LzmaDecompress, false, blocks);
    }

    virtual ECodec GetId() const override
    {
        switch (Level_) {

#define CASE(level) case level: return PP_CONCAT(ECodec::Lzma_, level);
            PP_FOR_EACH(CASE, (0)(1)(2)(3)(4)(5)(6)(7)(8)(9))
#undef CASE

            default:
                YUNREACHABLE();
        }
    }

private:
    const NCompression::TConverter Compressor_;
    const int Level_;
};

////////////////////////////////////////////////////////////////////////////////

ICodec* GetCodec(ECodec id)
{
    switch (id) {
        case ECodec::None: {
            static TNoneCodec result;
            return &result;
        }

        case ECodec::Snappy: {
            static TSnappyCodec result;
            return &result;
        }

        case ECodec::Lz4: {
            static TLz4Codec result(false);
            return &result;
        }

        case ECodec::Lz4HighCompression: {
            static TLz4Codec result(true);
            return &result;
        }

        case ECodec::QuickLz: {
            static TQuickLzCodec result;
            return &result;
        }

        case ECodec::ZstdLegacy: {
            static TZstdLegacyCodec result;
            return &result;
        }


#define CASE(param)                                                 \
    case ECodec::PP_CONCAT(CODEC, PP_CONCAT(_, param)): {           \
        static PP_CONCAT(T, PP_CONCAT(CODEC, Codec)) result(param); \
        return &result;                                             \
    }

#define CODEC Zlib
        PP_FOR_EACH(CASE, (1)(2)(3)(4)(5)(6)(7)(8)(9))
#undef CODEC

#define CODEC Brotli
        PP_FOR_EACH(CASE, (1)(2)(3)(4)(5)(6)(7)(8)(9)(10)(11))
#undef CODEC

#define CODEC Lzma
        PP_FOR_EACH(CASE, (0)(1)(2)(3)(4)(5)(6)(7)(8)(9))
#undef CODEC

#define CODEC Zstd
        PP_FOR_EACH(CASE, (1)(2)(3)(4)(5)(6)(7)(8)(9)(10)(11)(12)(13)(14)(15)(16)(17)(18)(19)(20)(21))
#undef CODEC

#undef CASE

        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCompression
} // namespace NYT

