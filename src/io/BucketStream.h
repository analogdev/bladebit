#include "IStream.h"

class BucketStream : public IStream
{
public:
    enum Mode
    {
        Sequential = 0,
        Interleaved
    };

    BucketStream( IStream& baseStream, const size_t bucketMaxSize, const uint32 numBuckets );

    virtual ~BucketStream();

    ssize_t Read( void* buffer, size_t size ) override;

    ssize_t Write( const void* buffer, size_t size ) override;

    void WriteBucketSlices( const void* slices, const uint32* sliceSizes );

    void ReadBucket( const size_t size, void* readBuffer );

    bool Seek( int64 offset, SeekOrigin origin ) override;

    bool Flush() override;

    size_t BlockSize() const override;

    ssize_t Size() override;

    bool Truncate( const ssize_t length ) override;

    int GetError() override;

    inline void SwitchMode()
    {
        _mode = (Mode)(((uint32)_mode + 1) & 1); // (mode + 1) % 2
    }

    inline Mode GetMode() const { return _mode; }

private:
    struct Slice
    {
        uint32 position;
        uint32 size;
    };
private:
    IStream&     _baseStream;
    Span<Slice*> _sliceSizes;          // Current size of each slice in a bucket (used for reading back buckets)
    size_t       _bucketCapacity;      // Maximum capacity of a single bucket
    uint32       _numBuckets;
    uint16       _bucket = 0;          // Current bucket index
    Mode         _mode   = Sequential; // Current serialization mode
};

