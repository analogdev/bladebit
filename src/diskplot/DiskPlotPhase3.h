#pragma once

#include "DiskPlotContext.h"

class DiskPlotPhase3
{
public:
    DiskPlotPhase3( DiskPlotContext& context );
    ~DiskPlotPhase3();

    void Run();

private:
    void ProcessTable( const TableId rTable );

    void TableFirstPass( const TableId rTable );
    void BucketFirstPass( const TableId rTable, const uint32 bucket );

    void ProcessLTableMap( const uint32 bucket, const uint32 entryCount, const uint64* lMap, uint32* outSortedLMap );

    void LoadNextLTableMap( const TableId lTable );

    uint32 PointersToLinePoints( const uint32 entryCount, const uint64* markedEntries, 
                                 const Pairs pairs, const uint32* lTable, uint64* outLinePoints );



    void TableSecondPass( const TableId rTable );

private:
    DiskPlotContext& _context;
    
    size_t  _markedEntriesSize;
    uint64* _markedEntries;
    uint64* _lMap       [2];
    Pairs   _rTablePairs[2];
    uint64* _rMap       [2];
    uint64* _linePoints;                // Used to convert to line points/tmp buffer
    Fence   _rTableFence;
    Fence   _lTableFence;

    uint64  _tableEntryCount[7];        // Count of each table, after prunning

    uint64  _lTableEntriesLoaded = 0;
    uint32  _lBucketsLoading     = 0;
    uint32  _lBucketsConsumed    = 0;
    uint32  _lBucketPrefixCount  = 0;
    // uint64  _lTableOffset       = 0;    // The starting offset of the lowest bucket currently loaded on the l table
    // int32   _tableBufferIdx     = 0;    // Current entry offset from the l table buckets
};
