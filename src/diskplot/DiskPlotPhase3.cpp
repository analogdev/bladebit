#include "DiskPlotPhase3.h"
#include "util/BitField.h"
#include "algorithm/RadixSort.h"
#include "memplot/LPGen.h"
#include "DiskPlotDebug.h"
// #include "jobs/UnpackMapJob.h"

#define P3_EXTRA_L_ENTRIES_TO_LOAD 1024     // Extra L entries to load per bucket to ensure we
                                            // have cross bucket entries accounted for
/**
 * Algorithm:
 * 
 * Let rTable be a table in a set {table2, table3, ..., table7}
 * Let lTable be rTable - 1. Such that if rTable is table2, then lTable is table1
 * 
 * For each rTable perform 2 passes:
 *
 * Pass 1. Process each bucket as follows]:
 * - Load L/R back pointers for rTable.
 * - Load y index map for rTable.
 * - Load marked entries from Phase 2 for rTable.
 * - Load lTable, which for rTable==1 is the x buckets, otherwise it is the output of map of 
 *      the previous iteration's rTable.
 * - If rTable > table2:
 *      - Sort the lTable map on its origin (y) index, and then discard the origin index,
 *          keeping only the destination index (final position of an entry after LP sort).
 * - Sort the rTable map on its origin index.
 * - Generate LinePoints (LPs) from the rTable pointers and the lTable x or map values,
 *      while excluding each entry that is not marked in the marked entries table.
 * - Distribute the LPs to their respective buckets along with the rTable (y) map 
 *      and write them to disk.
 *      (The r table (y) map represents the origin index before sorting.)
 * 
 * Pass 2. Process each LP bucket as follows:
 * - Load the rTable LP output and map.
 * - Sort the LP bucket and map on LP.
 * - Compress the LP bucket and write it to disk.
 * - Convert the sorted map into a reverse lookup by extending them with its origin index (its current value)
 *      and its destination index (its current index after sort). Then distribute
 *      them to buckets given its origin value. Write the buckets to disk.
 * 
 * Go to next table.
 */
struct P3FenceId
{
    enum 
    {
        Start = 0,

        RTableLoaded,
        RMapLoaded,

        FENCE_COUNT
    };
};

struct Step2FenceId
{
    enum 
    {
        Start = 0,

        LPLoaded,
        MapLoaded,

        FENCE_COUNT
    };
};


struct ConvertToLPJob : public PrefixSumJob<ConvertToLPJob>
{
    DiskPlotContext* context;
    TableId          rTable;

    uint64           rTableOffset;
    uint32           bucketEntryCount;
    const uint64*    markedEntries;
    const uint32*    lMap;
    const uint32*    rMap;
    Pairs            rTablePairs;

    uint64*          linePoints;        // Buffer for line points/pruned pairs
    uint32*          rMapPruned;        // Where we store our pruned R map

    int64            prunedEntryCount;      // Pruned entry count per thread.
    int64            totalPrunedEntryCount; // Pruned entry count accross all threads
    
    void Run() override;

    // For distributing
    uint32*          bucketCounts;          // Total count of entries per bucket (used by first thread)
    uint64*          lpOutBuffer;
    uint32*          keyOutBuffer;

    void DistributeToBuckets( const int64 enytryCount, const uint64* linePoints, const uint32* map );
};

//-----------------------------------------------------------
DiskPlotPhase3::DiskPlotPhase3( DiskPlotContext& context, const Phase3Data& phase3Data )
    : _context   ( context    )
    , _phase3Data( phase3Data )
{
    memset( _tableEntryCount, 0, sizeof( _tableEntryCount ) );

    DiskBufferQueue& ioQueue = *context.ioQueue;

    // Open required files
    ioQueue.InitFileSet( FileId::LP_2, "lp_2", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_3, "lp_3", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_4, "lp_4", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_5, "lp_5", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_6, "lp_6", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_7, "lp_7", BB_DPP3_LP_BUCKET_COUNT );

    ioQueue.InitFileSet( FileId::LP_KEY_2, "lp_key_2", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_KEY_3, "lp_key_3", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_KEY_4, "lp_key_4", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_KEY_5, "lp_key_5", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_KEY_6, "lp_key_6", BB_DPP3_LP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_KEY_7, "lp_key_7", BB_DPP3_LP_BUCKET_COUNT );

    ioQueue.InitFileSet( FileId::LP_MAP_2, "lp_map_2", BB_DP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_MAP_3, "lp_map_3", BB_DP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_MAP_4, "lp_map_4", BB_DP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_MAP_5, "lp_map_5", BB_DP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_MAP_6, "lp_map_6", BB_DP_BUCKET_COUNT );
    ioQueue.InitFileSet( FileId::LP_MAP_7, "lp_map_7", BB_DP_BUCKET_COUNT );

    // Find largest bucket size accross all tables
    uint32 maxBucketLength = 0;
    for( TableId table = TableId::Table1; table <= TableId::Table7; table = table +1 )
    {
        if( table < TableId::Table2 )
        {
            for( uint32 i = 0; i < BB_DP_BUCKET_COUNT; i++ )
                maxBucketLength = std::max( context.bucketCounts[(int)table][i], maxBucketLength );
        }
        else
        {
            for( uint32 i = 0; i < BB_DP_BUCKET_COUNT; i++ )
            {
                maxBucketLength = std::max( context.bucketCounts[(int)table][i], 
                                    std::max( context.ptrTableBucketCounts[(int)table][i], maxBucketLength ) );
            }
        }
    }

    maxBucketLength += P3_EXTRA_L_ENTRIES_TO_LOAD;

    // Init our buffers
    // #TODO: Remove this as we're moving alignment on to the ioQueue to handle?
    const size_t fileBlockSize        = ioQueue.BlockSize();

    // #TODO: Only have marking table, lp bucket and pruned r map buckets as
    //        fixed buffers, the rest we can just grab from the heap.
    const size_t markedEntriesSize    = phase3Data.bitFieldSize;
    const size_t rTableMapBucketSize  = RoundUpToNextBoundary( maxBucketLength * sizeof( uint32 ), fileBlockSize );
    const size_t rTableLPtrBucketSize = RoundUpToNextBoundary( maxBucketLength * sizeof( uint32 ), fileBlockSize );
    const size_t rTableRPtrBucketSize = RoundUpToNextBoundary( maxBucketLength * sizeof( uint16 ), fileBlockSize );
    
    const size_t lTableBucketSize     = RoundUpToNextBoundary( maxBucketLength * sizeof( uint32 ), fileBlockSize );
    const size_t lpBucketSize         = RoundUpToNextBoundary( maxBucketLength * sizeof( uint64 ), fileBlockSize );

    byte* heap = context.heapBuffer;

    _markedEntries        = (uint64*)heap;
    heap += markedEntriesSize;

    _rMap[0]              = (uint32*)heap; heap += rTableMapBucketSize;
    _rMap[1]              = (uint32*)heap; heap += rTableMapBucketSize;

    _rTablePairs[0].left  = (uint32*)heap; heap += rTableLPtrBucketSize;
    _rTablePairs[1].left  = (uint32*)heap; heap += rTableLPtrBucketSize;

    _rTablePairs[0].right = (uint16*)heap; heap += rTableRPtrBucketSize;
    _rTablePairs[1].right = (uint16*)heap; heap += rTableRPtrBucketSize;

    _lMap[0]    = (uint32*)heap; heap += lTableBucketSize;
    _lMap[1]    = (uint32*)heap; heap += lTableBucketSize;

    _rPrunedMap = (uint32*)heap; heap += rTableMapBucketSize;
    _linePoints = (uint64*)heap; heap += lpBucketSize;

    size_t totalSize = 
        markedEntriesSize        + 
        rTableMapBucketSize  * 3 + 
        rTableLPtrBucketSize * 2 + 
        rTableRPtrBucketSize * 2 + 
        lpBucketSize;

    // Reset our heap to the remainder of what we're not using
    const size_t fullHeapSize  = context.heapSize + context.ioHeapSize;
    const size_t heapRemainder = fullHeapSize - totalSize;

    ioQueue.ResetHeap( heapRemainder, heap );
}

//-----------------------------------------------------------
DiskPlotPhase3::~DiskPlotPhase3()
{}

//-----------------------------------------------------------
void DiskPlotPhase3::Run()
{
    for( TableId table = TableId::Table2; table < TableId::Table7; table++ )
    {
        Log::Line( "Compressing Tables %u and %u...", table, table+1 );
        const auto timer = TimerBegin();

        ProcessTable( table );

        const auto elapsed = TimerEnd( timer );
        Log::Line( "Finished compression in %.2lf seconds.", elapsed );
    }
}

//-----------------------------------------------------------
void DiskPlotPhase3::ProcessTable( const TableId rTable )
{
    DiskPlotContext& context = _context;
    // DiskBufferQueue& ioQueue = *context.ioQueue;

    // Reset table counts 
    _prunedEntryCount = 0;
    memset( _lpBucketCounts  , 0, sizeof( _lpBucketCounts ) );
    memset( _lMapBucketCounts, 0, sizeof( _lMapBucketCounts ) );

    // Reset Fence
    _readFence.Reset( P3FenceId::Start );

    // Prune the R table pairs and key,
    // convert pairs to LPs, then distribute
    // the LPs to buckets, along with the key.
    TableFirstStep( rTable );

    // Test linePoints
    #if _DEBUG
        Debug::ValidateLinePoints( context, rTable, _lpBucketCounts );
    #endif

    // Load LP buckets and key, sort them, 
    // write a reverse lookup map given the sorted key,
    // then compress and write the rTable to disk.
    TableSecondStep( rTable );

    // Unpack map to be used as the L table for the next table iteration
    TableThirdStep( rTable );

    // Update to our new bucket count and table entry count
    const uint64 oldEntryCount = context.entryCounts[(int)rTable];
    Log::Line( " Table %u now has %llu / %llu ( %.2lf%%) entries.", rTable,
                _prunedEntryCount, oldEntryCount, (double)_prunedEntryCount / oldEntryCount * 100 );

    context.entryCounts[(int)rTable] = _prunedEntryCount;
}


///
/// First Step
///
//-----------------------------------------------------------
void DiskPlotPhase3::TableFirstStep( const TableId rTable )
{
    DiskPlotContext& context         = _context;
    DiskBufferQueue& ioQueue         = *context.ioQueue;
    Fence&           readFence       = _readFence;

    const TableId lTable             = rTable - 1;
    const uint64  maxEntries         = 1ull << _K;
    const uint64  lTableEntryCount   = context.entryCounts[(int)lTable];
    const uint64  rTableEntryCount   = context.entryCounts[(int)rTable];

    const FileId markedEntriesFileId = TableIdToMarkedEntriesFileId( rTable );
    const FileId lMapId              = rTable == TableId::Table2 ? FileId::X : TableIdToLinePointMapFileId( lTable );
    const FileId rMapId              = TableIdToMapFileId( rTable );
    const FileId rPtrsRId            = TableIdToBackPointerFileId( rTable ); 
    const FileId rPtrsLId            = rPtrsRId + 1;

    // Prepare our files for reading
    ioQueue.SeekBucket( markedEntriesFileId, 0, SeekOrigin::Begin );
    ioQueue.SeekFile  ( lMapId             , 0, 0, SeekOrigin::Begin );
    ioQueue.SeekFile  ( rMapId             , 0, 0, SeekOrigin::Begin );
    ioQueue.SeekFile  ( rPtrsRId           , 0, 0, SeekOrigin::Begin );
    ioQueue.SeekFile  ( rPtrsLId           , 0, 0, SeekOrigin::Begin );
    ioQueue.CommitCommands();

    uint64 lEntriesLoaded = 0;

    // Read first bucket
    {
        const uint32 lBucketLength = context.bucketCounts[(int)lTable][0] + P3_EXTRA_L_ENTRIES_TO_LOAD;
        const uint32 rBucketLength = context.ptrTableBucketCounts[(int)rTable][0];

        lEntriesLoaded += lBucketLength;

        // Read L Table 1st bucket
        ioQueue.ReadFile( lMapId, 0, _lMap[0], lBucketLength * sizeof( uint32 ) );;

        // Read R Table marks
        ioQueue.ReadFile( markedEntriesFileId, 0, _markedEntries, _phase3Data.bitFieldSize );

        // Read R Table 1st bucket
        ioQueue.ReadFile( rPtrsRId, 0, _rTablePairs[0].left , rBucketLength * sizeof( uint32 ) );
        ioQueue.ReadFile( rPtrsLId, 0, _rTablePairs[0].right, rBucketLength * sizeof( uint16 ) );

        ioQueue.ReadFile( rMapId, 0, _rMap[0], rBucketLength * sizeof( int32 ) );
        ioQueue.SignalFence( readFence, 1 );

        ioQueue.CommitCommands();
    }
    
    // Reset offsets
    _rTableOffset = 0;

    // Start processing buckets
    for( uint bucket = 0; bucket < BB_DP_BUCKET_COUNT; bucket++ )
    {
        const bool isCurrentBucketLastBucket = bucket == BB_DP_BUCKET_COUNT - 1;
        
        if( !isCurrentBucketLastBucket )
        {
            // Load the next bucket on the background
            const uint32 nextBucket             = bucket + 1;
            const bool   nextBucketIsLastBucket = nextBucket == BB_DP_BUCKET_COUNT - 1; 
           
            uint32 lBucketLength = context.bucketCounts[(int)lTable][nextBucket];
            uint32 rBucketLength = context.ptrTableBucketCounts[(int)rTable][nextBucket];

            if( nextBucketIsLastBucket )
                lBucketLength = (uint32)( context.entryCounts[(int)lTable] - lEntriesLoaded );

            lEntriesLoaded += lBucketLength;

            // Load L Table
            ioQueue.ReadFile( lMapId, 0, _lMap[1] + P3_EXTRA_L_ENTRIES_TO_LOAD, lBucketLength * sizeof( uint32 ) );

            // Load R Table
            ioQueue.ReadFile( rPtrsRId, 0, _rTablePairs[1].left , rBucketLength * sizeof( uint32 ) );
            ioQueue.ReadFile( rPtrsLId, 0, _rTablePairs[1].right, rBucketLength * sizeof( uint16 ) );
            
            ioQueue.ReadFile( rMapId, 0, _rMap[1], rBucketLength * sizeof( uint32 ) );
            ioQueue.SignalFence( readFence, nextBucket + 1 );

            ioQueue.CommitCommands();
        }

        // Process the bucket
        BucketFirstStep( rTable, bucket );

        // Copy last L entries from current bucket to next bucket's first entries
        memcpy( _lMap[1], _lMap[0] + context.bucketCounts[(int)lTable][bucket], P3_EXTRA_L_ENTRIES_TO_LOAD * sizeof( uint32 ) );

        // Swap buffers
        std::swap( _lMap[0]       , _lMap[1] );
        std::swap( _rMap[0]       , _rMap[1] );
        std::swap( _rTablePairs[0], _rTablePairs[1] );
    }
}

//-----------------------------------------------------------
void DiskPlotPhase3::BucketFirstStep( const TableId rTable, const uint32 bucket )
{
    DiskPlotContext& context       = _context;
    DiskBufferQueue& ioQueue       = *context.ioQueue;
    ThreadPool&      threadPool    = *context.threadPool;
    Fence&           readFence     = _readFence;

    const TableId lTable           = rTable - 1;

    const bool isLastBucket = bucket == BB_DP_BUCKET_COUNT - 1;

    const uint32 bucketEntryCountR = context.ptrTableBucketCounts[(int)rTable][bucket];

    // Wait for the bucket to be loaded
    readFence.Wait( bucket + 1 );

    #if _DEBUG
    {
        const uint32 r = _rTablePairs[0].left[bucketEntryCountR-1] + _rTablePairs[0].right[bucketEntryCountR-1];
        const uint32 lTableBucketLength = _context.bucketCounts[(int)lTable][bucket] + P3_EXTRA_L_ENTRIES_TO_LOAD;
        ASSERT( r < lTableBucketLength );
    }
    #endif

    // Convert to line points
    const uint32 prunedEntryCount = 
        PointersToLinePoints( 
            rTable, _rTableOffset,
            bucketEntryCountR, _markedEntries, 
            _lMap[0], 
            _rTablePairs[0], _rMap[0], 
            _rPrunedMap, _linePoints );

    _prunedEntryCount += prunedEntryCount;

    // Update our offset for the next bucket
    _rTableOffset += bucketEntryCountR;
}

//-----------------------------------------------------------
uint32 DiskPlotPhase3::PointersToLinePoints( 
    TableId rTable, uint64 entryOffset,
    const uint32 entryCount, const uint64* markedEntries, 
    const uint32* lTable, 
    const Pairs pairs, const uint32* rMapIn, 
    uint32* rMapOut, uint64* outLinePoints )
{
    const uint32 threadCount = _context.threadCount;

    uint32 bucketCounts[BB_DPP3_LP_BUCKET_COUNT];

    MTJobRunner<ConvertToLPJob> jobs( *_context.threadPool );

    for( uint32 i = 0; i < threadCount; i++ )
    {
        ConvertToLPJob& job = jobs[i];

        job.context = &_context;
        job.rTable  = rTable;

        job.rTableOffset     = entryOffset;
        job.bucketEntryCount = entryCount;
        job.markedEntries    = markedEntries;
        job.lMap             = lTable;
        job.rTablePairs      = pairs;
        job.rMap             = rMapIn;
        job.linePoints       = outLinePoints;
        job.rMapPruned       = rMapOut;

        job.bucketCounts     = bucketCounts;
    }

    jobs.Run( threadCount );

    for( uint32 i = 0; i < BB_DPP3_LP_BUCKET_COUNT; i++ )
        _lpBucketCounts[i] += bucketCounts[i];

    uint32 prunedEntryCount = (uint32)jobs[0].totalPrunedEntryCount;
    return prunedEntryCount;
}

//-----------------------------------------------------------
void ConvertToLPJob::Run()
{
    DiskPlotContext& context = *this->context;

    const int32 threadCount  = (int32)this->JobCount();
    int64       entryCount   = (int64)( this->bucketEntryCount / threadCount );

    const int64 bucketOffset = entryCount * (int32)this->JobId();   // Offset in the bucket
    const int64 rTableOffset = (int64)this->rTableOffset;           // Offset in the table overall (used for checking marks)
    const int64 marksOffset  = rTableOffset + bucketOffset;

    if( this->IsLastThread() )
        entryCount += (int64)this->bucketEntryCount - entryCount * threadCount;

    const int64 end = bucketOffset + entryCount;

    const BitField markedEntries( (uint64*)this->markedEntries );
    
    const uint32* rMap  = this->rMap;
    const Pairs   pairs = this->rTablePairs;

    // First, scan our entries in order to prune them
    int64 prunedLength = 0;

    for( int64 i = bucketOffset; i < end; i++)
    {
        // #TODO: Try changing Phase 2 to write atomically to see
        //        (if we don't get a huge performance hit),
        //        if we can do reads without the rMap
        if( markedEntries.Get( rMap[i] ) )
            prunedLength ++;
    }

    this->prunedEntryCount = prunedLength;

    this->SyncThreads();

    // Set our destination offset
    int64 dstOffset = 0;

    for( int32 i = 0; i < (int32)this->JobId(); i++ )
        dstOffset += GetJob( i ).prunedEntryCount;

    // Copy pruned entries into new buffer
    // #TODO: heck if doing 1 pass per buffer performs better
    


    struct Pair
    {
        uint32 left;
        uint32 right;
    };

    Pair*   outPairsStart = (Pair*)(this->linePoints + dstOffset);
    Pair*   outPairs      = outPairsStart;
    uint32* outRMap       = this->rMapPruned + dstOffset;

    for( int64 i = bucketOffset; i < end; i++ )
    {
        const uint32 mapIdx = rMap[i];
        if( !markedEntries.Get( mapIdx ) )
            continue;

        outPairs->left  = pairs.left[i];
        outPairs->right = outPairs->left + pairs.right[i];

        *outRMap        = mapIdx;

        outPairs++;
        outRMap++;
    }

    // Now we can convert our pruned pairs to line points
    uint64* outLinePoints = this->linePoints + dstOffset;
    {
        const uint32* lTable = this->lMap;
        
        for( int64 i = 0; i < prunedLength; i++ )
        {
            Pair p = outPairsStart[i];
            const uint64 x = lTable[p.left ];
            const uint64 y = lTable[p.right];
            // if( p.left == 0 ) BBDebugBreak();

            outLinePoints[i] = SquareToLinePoint( x, y );
            
            // #if _DEBUG
            // if( outLinePoints[i] == 2664297094 ) BBDebugBreak();
            // #endif
        }
        // const uint64* lpEnd         = outLinePoints + prunedLength;

        // do
        // {
        //     Pair p = *((Pair*)outLinePoints);
        //     const uint64 x = lTable[p.left ];
        //     const uint64 y = lTable[p.right];
            
        //     *outLinePoints = SquareToLinePoint( x, y );

        // } while( ++outLinePoints < lpEnd );
    }

    this->DistributeToBuckets( prunedLength, outLinePoints, outRMap );
}

//-----------------------------------------------------------
void ConvertToLPJob::DistributeToBuckets( const int64 entryCount, const uint64* linePoints, const uint32* key )
{
    uint32 counts[BB_DPP3_LP_BUCKET_COUNT];
    uint32 pfxSum[BB_DPP3_LP_BUCKET_COUNT];

    memset( counts, 0, sizeof( counts ) );

    // Count entries per bucket
    for( const uint64* lp = linePoints, *end = lp + entryCount; lp < end; lp++ )
    {
        const uint64 bucket = (*lp) >> 56; ASSERT( bucket < BB_DPP3_LP_BUCKET_COUNT );
        counts[bucket]++;
    }
    
    this->CalculatePrefixSum( BB_DPP3_LP_BUCKET_COUNT, counts, pfxSum, this->bucketCounts );

    uint64* lpOutBuffer  = nullptr;
    uint32* keyOutBuffer = nullptr;

    if( this->IsControlThread() )
    {
        this->LockThreads();

        DiskBufferQueue& ioQueue = *context->ioQueue;

        const int64 threadCount           = (int64)this->JobCount();
        int64       totalEntryCountPruned = entryCount;

        for( int64 i = 1; i < threadCount; i++ )
            totalEntryCountPruned += this->GetJob( (int)i ).prunedEntryCount;

        this->totalPrunedEntryCount = totalEntryCountPruned;

        const size_t sizeLPs = (size_t)totalEntryCountPruned * sizeof( uint64 );
        const size_t sizeKey = (size_t)totalEntryCountPruned * sizeof( uint32 );

        lpOutBuffer  = (uint64*)ioQueue.GetBuffer( sizeLPs, true );
        keyOutBuffer = (uint32*)ioQueue.GetBuffer( sizeKey, true );

        this->lpOutBuffer  = lpOutBuffer;
        this->keyOutBuffer = keyOutBuffer;

        this->ReleaseThreads();
    }
    else
    {
        this->WaitForRelease();
        lpOutBuffer  = GetJob( 0 ).lpOutBuffer ;
        keyOutBuffer = GetJob( 0 ).keyOutBuffer;
    }

    // Distribute entries to their respective buckets
    for( int64 i = 0; i < entryCount; i++ )
    {
        const uint64 lp       = linePoints[i]; //if( lp == 2664297094 ) BBDebugBreak();
        const uint64 bucket   = lp >> 56;           ASSERT( bucket < BB_DPP3_LP_BUCKET_COUNT );
        const uint32 dstIndex = --pfxSum[bucket];

        lpOutBuffer [dstIndex] = lp;
        keyOutBuffer[dstIndex] = key[i];
    }

    if( this->IsControlThread() )
    {
        DiskBufferQueue& ioQueue = *context->ioQueue;

        uint32* lpSizes  = (uint32*)ioQueue.GetBuffer( BB_DPP3_LP_BUCKET_COUNT * sizeof( uint32 ) );
        uint32* keySizes = (uint32*)ioQueue.GetBuffer( BB_DPP3_LP_BUCKET_COUNT * sizeof( uint32 ) );

        const uint32* bucketCounts = this->bucketCounts;

        for( int64 i = 0; i < (int)BB_DPP3_LP_BUCKET_COUNT; i++ )
            lpSizes[i] = bucketCounts[i] * sizeof( uint64 );

        for( int64 i = 0; i < (int)BB_DPP3_LP_BUCKET_COUNT; i++ )
            keySizes[i] = bucketCounts[i] * sizeof( uint32 );

        const FileId lpFileId   = TableIdToLinePointFileId   ( this->rTable );
        const FileId lpKeyFilId = TableIdToLinePointKeyFileId( this->rTable );
        
        ioQueue.WriteBuckets( lpFileId, lpOutBuffer, lpSizes );
        ioQueue.ReleaseBuffer( lpOutBuffer );
        ioQueue.ReleaseBuffer( lpSizes );

        ioQueue.WriteBuckets( lpKeyFilId, keyOutBuffer, keySizes );
        ioQueue.ReleaseBuffer( keyOutBuffer );
        ioQueue.ReleaseBuffer( keySizes );

        ioQueue.CommitCommands();
    }

    // Wait for other thread sp that counts doesn't go out of scope
    this->SyncThreads();
}



///
/// Seconds Step
///
//-----------------------------------------------------------
void DiskPlotPhase3::TableSecondStep( const TableId rTable )
{
    auto& context = _context;
    auto& ioQueue = *context.ioQueue;

    const FileId lpId  = TableIdToLinePointFileId   ( rTable );
    const FileId keyId = TableIdToLinePointKeyFileId( rTable );
    
    Fence& readFence = _readFence;
    readFence.Reset( Step2FenceId::Start );

    ioQueue.SeekBucket( lpId , 0, SeekOrigin::Begin );
    ioQueue.SeekBucket( keyId, 0, SeekOrigin::Begin );
    ioQueue.CommitCommands();

    struct BucketBuffers
    {
        uint64* linePoints;
        uint32* key;
    };


    uint64 entryOffset   = 0;
    uint32 bucketsLoaded = 0;
    BucketBuffers buffers[BB_DPP3_LP_BUCKET_COUNT];

    // Use a capture lampda for now, but change this to a non-capturing one later maybe
    auto LoadBucket = [&]( uint32 bucket, bool forceLoad ) -> BucketBuffers
    {
        const uint32 bucketLength = _lpBucketCounts[bucket];

        const size_t lpBucketSize  = sizeof( uint64 ) * bucketLength;
        const size_t mapBucketSize = sizeof( uint32 ) * bucketLength;

        uint64* linePoints = (uint64*)ioQueue.GetBuffer( lpBucketSize , forceLoad );
        uint32* key        = (uint32*)ioQueue.GetBuffer( mapBucketSize, forceLoad );

        const uint32 fenceIdx = bucket * Step2FenceId::FENCE_COUNT;

        ioQueue.ReadFile( lpId , bucket, linePoints, lpBucketSize  );
        ioQueue.SignalFence( readFence, Step2FenceId::LPLoaded + fenceIdx );

        ioQueue.ReadFile( keyId, bucket, key, mapBucketSize );
        ioQueue.SignalFence( readFence, Step2FenceId::MapLoaded + fenceIdx );
        
        ioQueue.CommitCommands();

        return {
            .linePoints = linePoints,
            .key        = key
        };
    };

    buffers[0] = LoadBucket( 0, true );
    bucketsLoaded++;

    for( uint32 bucket = 0; bucket < BB_DPP3_LP_BUCKET_COUNT; bucket++ )
    {
        const uint32 nextBucket   = bucket + 1;
        const bool   isLastBucket = bucket == BB_DPP3_LP_BUCKET_COUNT - 1;

        if( !isLastBucket )
        {
            // #TODO: Make background loading optional if we have no buffers available,
            //        then force-load if we don't have the current bucket pre-loaded.
            buffers[nextBucket] = LoadBucket( nextBucket, true );
            bucketsLoaded++;
        }

        const uint32 bucketLength = _lpBucketCounts[bucket];
        
        const uint32 fenceIdx = bucket * Step2FenceId::FENCE_COUNT;
        readFence.Wait( Step2FenceId::MapLoaded + fenceIdx );

        uint64* linePoints = buffers[bucket].linePoints;
        uint32* key        = buffers[bucket].key;

        uint64* sortedLinePoints = _linePoints;
        uint32* sortedKey        = _rPrunedMap;

        // Sort line point w/ the key
        // Since we're skipping an iteration, the output will be 
        // stored in the temp buffers, instead on the input ones.
        RadixSort256::SortWithKey<BB_MAX_JOBS, uint64, uint32>( 
            *context.threadPool, linePoints, sortedLinePoints,
            key, sortedKey, bucketLength );

        // RadixSort256::Sort<BB_MAX_JOBS>( 
        //     *context.threadPool, linePoints, sortedLinePoints,
        //      bucketLength );

        ioQueue.ReleaseBuffer( linePoints ); linePoints = nullptr;
        ioQueue.ReleaseBuffer( key        ); key        = nullptr;
        ioQueue.CommitCommands();

        // Write the map back to disk as a reverse lookup map
        WriteLPReverseLookup( rTable, sortedKey, bucket, bucketLength, entryOffset );

        // #TODO: Deltafy, compress and write bucket to plot file in a park

        entryOffset += bucketLength;
    }
}

struct WriteLPMapJob : PrefixSumJob<WriteLPMapJob>
{
    uint32  bucket;
    uint32  entryCount;
    uint64  entryOffset;

    const uint32* inKey;
    uint64*       outMap;

    uint32* bucketCounts;

    void Run() override;
};

//-----------------------------------------------------------
void DiskPlotPhase3::WriteLPReverseLookup( 
    const TableId rTable, const uint32* key,
    const uint32 bucket , const uint32  entryCount,
    const uint64 entryOffset )
{
    constexpr uint32 BucketSize = BB_DP_BUCKET_COUNT;

    // Pack entries to a reverse lookup map and sort them
    // into their buckets of origin (before sorted to line point)
    ASSERT( entryOffset + entryCount <= 0xFFFFFFFFull );

    auto& ioQueue = *_context.ioQueue;

    const size_t bufferSize =  sizeof( uint64 ) * entryCount;

    uint64* outMap       = (uint64*)ioQueue.GetBuffer( bufferSize );
    uint32* bucketCounts = (uint32*)ioQueue.GetBuffer( BucketSize * sizeof( uint32 ) );

    // memset( bucketCounts, 0, BucketSize * sizeof( uint32 ) );

    const uint32 threadCount = _context.threadCount;

    MTJobRunner<WriteLPMapJob> jobs( *_context.threadPool );

    for( uint32 i = 0; i < threadCount; i++ )
    {
        auto& job = jobs[i];

        job.bucket       = bucket;
        job.entryCount   = entryCount;
        job.entryOffset  = entryOffset;

        job.inKey        = key;
        job.outMap       = outMap;
        job.bucketCounts = nullptr;
    }

    jobs[0].bucketCounts = bucketCounts;
    jobs.Run( threadCount );

    // Append to our overall bucket count
    for( uint32 i = 0; i < BucketSize; i++ )
        _lMapBucketCounts[i] += bucketCounts[i];

    // Update count to sizes
    for( uint32 i = 0; i < BucketSize; i++ )
        bucketCounts[i] *= sizeof( uint64 );

    // Write to disk
    const FileId mapId = TableIdToLinePointMapFileId( rTable );

    ioQueue.WriteBuckets( mapId, outMap, bucketCounts );
    ioQueue.ReleaseBuffer( outMap );
    ioQueue.ReleaseBuffer( bucketCounts );
    ioQueue.CommitCommands();
}

//-----------------------------------------------------------
void WriteLPMapJob::Run()
{
    const int32 threadCount = (int32)this->JobCount();
    
    int64 entriesPerThread  = (int64)this->entryCount / threadCount;

    int64 offset = entriesPerThread * this->JobId();

    if( this->IsLastThread() )
        entriesPerThread += (int64)this->entryCount - entriesPerThread * threadCount;

    // Count how many entries we have per bucket
    // #TODO: Use arbirary bucket count here too (from 64-512) and bit-pack entries tightly
    //          then we can save at least 6 bits per entry, since we can infer it from its bucket.
    const uint32 bitShift = 32 - kExtraBits;
    constexpr uint32 BucketSize = BB_DP_BUCKET_COUNT;

    uint32 counts[BucketSize];
    uint32 pfxSum[BucketSize];

    memset( counts, 0, sizeof( counts ) );

    for( const uint32* key = inKey, *end = key + entriesPerThread; key < end; key++ )
    {
        const uint32 bucket = *key >> bitShift;
        counts[bucket]++;
    }

    this->CalculatePrefixSum( BucketSize, counts, pfxSum, this->bucketCounts );

    // Write into our buckets
    const uint64 entryOffset = (uint64)this->entryOffset + (uint64)offset;

    const uint32* inKey  = this->inKey + offset;
    uint64*       outMap = this->outMap;

    for( int64 i = 0; i < entriesPerThread; i++ )
    {
        const uint32 key    = inKey[i];
        const uint32 bucket = key >> bitShift;

        const uint32 writeIdx = --pfxSum[bucket];
        ASSERT( writeIdx < this->entryCount );

        uint64 map = (entryOffset + (uint64)i) << 32;
        
        #if _DEBUG
        // if( key == 4256999824 ) BBDebugBreak();
        if( writeIdx == 42033 ) BBDebugBreak();
        #endif
        outMap[writeIdx] = map | key;
    }

    // Wait for other thread sp that counts doesn't go out of scope
    this->SyncThreads();
}


///
/// Third Step
///


struct LPUnpackMapJob : MTJob<LPUnpackMapJob>
{
    uint32        bucket;
    uint32        entryCount;
    const uint64* mapSrc;
    uint32*       mapDst;

    //-----------------------------------------------------------
    static void RunJob( ThreadPool& pool, const uint32 threadCount, const uint32 bucket,
                        const uint32 entryCount, const uint64* mapSrc, uint32* mapDst )
    {
        MTJobRunner<LPUnpackMapJob> jobs( pool );

        for( uint32 i = 0; i < threadCount; i++ )
        {
            auto& job = jobs[i];
            job.bucket     = bucket;
            job.entryCount = entryCount;
            job.mapSrc     = mapSrc;
            job.mapDst     = mapDst;
        }

        jobs.Run( threadCount );
    }

    //-----------------------------------------------------------
    void Run() override
    {
        const uint64 maxEntries         = 1ull << _K ;
        const uint32 fixedBucketLength  = (uint32)( maxEntries / BB_DP_BUCKET_COUNT );
        const uint32 bucketOffset       = fixedBucketLength * this->bucket;

        const uint32 threadCount = this->JobCount();
        uint32 entriesPerThread = this->entryCount / threadCount;

        const uint32 offset = entriesPerThread * this->JobId();

        if( this->IsLastThread() )
            entriesPerThread += this->entryCount - entriesPerThread * threadCount;

        const uint64* mapSrc = this->mapSrc + offset;
        uint32*       mapDst = this->mapDst;

        // Unpack with the bucket id
        for( uint32 i = 0; i < entriesPerThread; i++ )
        {
            const uint64 m   = mapSrc[i];
            const uint32 idx = (uint32)m - bucketOffset;
            
            ASSERT( idx < this->entryCount );

            mapDst[idx] = (uint32)(m >> 32);
        }
    }
};

//-----------------------------------------------------------
void DiskPlotPhase3::TableThirdStep( const TableId rTable )
{
    // Read back the packed map buffer from the current R table, then
    // write them back to disk as a single, contiguous file

    DiskPlotContext& context = _context;
    DiskBufferQueue& ioQueue = *context.ioQueue;

    constexpr uint32 BucketCount = BB_DP_BUCKET_COUNT;

    const FileId mapId = TableIdToLinePointMapFileId( rTable );

    const uint64 tableEntryCount = context.entryCounts[(int)rTable];

    const uint64 maxEntries      = 1ull << _K;
    const uint32 fixedBucketSize = (uint32)( maxEntries / BucketCount );
    const uint32 lastBucketSize  = (uint32)( tableEntryCount - fixedBucketSize * ( BucketCount - 1 ) );

    Fence& readFence = _readFence;
    readFence.Reset( 0 );

    ioQueue.SeekBucket( mapId, 0, SeekOrigin::Begin );
    ioQueue.CommitCommands();


    uint64* buffers[BucketCount] = { 0 };
    uint32  bucketsLoaded = 0;

    auto LoadBucket = [&]( const bool forceLoad ) -> void
    {
        const uint32 bucket = bucketsLoaded;

        const uint32 entryCount = _lMapBucketCounts[bucket];
        // const size_t bucketSize = RoundUpToNextBoundaryT( entryCount * sizeof( uint64 ), 4096ul );
        const size_t bucketSize = entryCount * sizeof( uint64 );

        auto* buffer = (uint64*)ioQueue.GetBuffer( bucketSize, forceLoad );
        if( !buffer )
            return;

        ioQueue.ReadFile( mapId, bucket, buffer, bucketSize );
        ioQueue.SignalFence( readFence, bucket + 1 );
        ioQueue.CommitCommands();

        if( bucket == 0 )
            ioQueue.SeekFile( mapId, 0, 0, SeekOrigin::Begin ); // Seek to the start to re-use this file for writing the unpacked map
        else
            ioQueue.DeleteFile( mapId, bucket );
        
        ioQueue.CommitCommands();

        buffers[bucketsLoaded++] = buffer;
    };

    LoadBucket( true );

    const uint32 maxBucketsToLoadPerIter = 2;

    for( uint32 bucket = 0; bucket < BucketCount; bucket++ )
    {
        const uint32 nextBucket   = bucket + 1;
        const bool   isLastBucket = nextBucket == BucketCount;

        // Reserve a buffer for writing
        const uint32 entryCount = _lMapBucketCounts[bucket];

        const uint32 writeEntryCount = isLastBucket ? lastBucketSize : fixedBucketSize;
        const size_t writeSize       = writeEntryCount * sizeof( uint32 );
        
        auto* writeBuffer = (uint32*)ioQueue.GetBuffer( writeSize, true );

        // Load next bucket
        // if( !isLastBucket && bucketsLoaded < BucketCount )
        // {
        //     uint32 maxBucketsToLoad = std::min( maxBucketsToLoadPerIter, BucketCount - bucketsLoaded );

        //     for( uint32 i = 0; i < maxBucketsToLoad; i++ )
        //     {
        //         const bool needNextBucket = bucketsLoaded == nextBucket;
        //         LoadBucket( needNextBucket );
        //     }
        // }

        readFence.Wait( nextBucket );

        // Unpack the map
        const uint64* inMap = buffers[bucket];

        // TEST
        {
            const uint64 maxEntries         = 1ull << _K ;
            const uint32 fixedBucketLength  = (uint32)( maxEntries / BB_DP_BUCKET_COUNT );
            // const uint32 bucketOffset       = fixedBucketLength * this->bucket;

            for( int64 i = 0; i < (int64)entryCount; i++ )
            {
                const uint32 idx = (uint32)inMap[i];
                ASSERT( idx < fixedBucketLength );
            }
        }
        // TEST protect this mem
        // SysHost::VirtualProtect( (void*)inMap, sizeof( uint64 ) * entryCount );
        // for( ;; ){}

        LPUnpackMapJob::RunJob( 
            *context.threadPool, context.threadCount, 
            bucket, entryCount, inMap, writeBuffer );

        // Write the unpacked map back to disk
        ioQueue.ReleaseBuffer( (void*)inMap );
        ioQueue.WriteFile( mapId, 0, writeBuffer, writeSize );
        ioQueue.ReleaseBuffer( writeBuffer );
        ioQueue.CommitCommands();
    }
}

