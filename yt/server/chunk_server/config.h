#pragma once

#include "public.h"

#include <ytlib/misc/nullable.h>
#include <ytlib/misc/error.h>

#include <ytlib/ytree/yson_serializable.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunkManagerConfig
    : public TYsonSerializable
{
public:
    //! When the number of online nodes drops below this margin,
    //! replicator gets disabled.
    TNullable<int> SafeOnlineNodeCount;

    //! When the fraction of lost chunks grows above this margin,
    //! replicator gets disabled.
    TNullable<double> SafeLostChunkFraction;

    //! Minimum difference in fill coefficient (between the most and the least loaded nodes) to start balancing.
    double MinBalancingFillFactorDiff;

    //! Minimum fill coefficient of the most loaded node to start balancing.
    double MinBalancingFillFactor;

    //! Maximum duration a job can run before it is considered dead.
    TDuration JobTimeout;

    //! Maximum total size of chunks assigned for replication (per node).
    i64 MaxTotalReplicationJobsSize;

    //! Maximum total size of chunks assigned for repair (per node).
    i64 MaxTotalRepairJobsSize;

    //! Memory usage assigned to every repair job.
    i64 RepairJobMemoryUsage;

    //! Graceful delay before chunk refresh.
    TDuration ChunkRefreshDelay;

    //! Interval between consequent chunk refresh scans.
    TDuration ChunkRefreshPeriod;

    //! Maximum number of chunks to process during a refresh scan.
    int MaxChunksPerRefresh;

    //! Each active upload session adds |ActiveSessionPenality| to effective load factor
    //! when picking an upload target.
    double ActiveSessionPenality;

    //! Interval between consequent chunk properties update scans.
    TDuration ChunkPropertiesUpdatePeriod;

    //! Maximum number of chunks to process during a properties update scan.
    int MaxChunksPerPropertiesUpdate;

    TChunkManagerConfig()
    {
        RegisterParameter("safe_online_node_count", SafeOnlineNodeCount)
            .GreaterThan(0)
            .Default(1);
        RegisterParameter("safe_lost_chunk_fraction", SafeLostChunkFraction)
            .InRange(0.0, 1.0)
            .Default(0.5);

        RegisterParameter("min_chunk_balancing_fill_factor_diff", MinBalancingFillFactorDiff)
            .Default(0.2);
        RegisterParameter("min_chunk_balancing_fill_factor", MinBalancingFillFactor)
            .Default(0.1);

        RegisterParameter("job_timeout", JobTimeout)
            .Default(TDuration::Minutes(5));

        RegisterParameter("max_total_replication_jobs_size", MaxTotalReplicationJobsSize)
            .Default((i64) 1024 * 1024 * 1024)
            .GreaterThanOrEqual(0);
        RegisterParameter("max_total_repair_jobs_size", MaxTotalRepairJobsSize)
            .Default((i64) 4 * 1024 * 1024 * 1024)
            .GreaterThanOrEqual(0);

        RegisterParameter("repair_job_memory_usage", RepairJobMemoryUsage)
            .Default((i64) 256 * 1024 * 1024)
            .GreaterThanOrEqual(0);

        RegisterParameter("chunk_refresh_delay", ChunkRefreshDelay)
            .Default(TDuration::Seconds(15));
        RegisterParameter("chunk_refresh_period", ChunkRefreshPeriod)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("max_chunks_per_refresh", MaxChunksPerRefresh)
            .Default(10000);

        RegisterParameter("chunk_properties_update_period", ChunkPropertiesUpdatePeriod)
            .Default(TDuration::MilliSeconds(1000));
        RegisterParameter("max_chunks_per_properties_update", MaxChunksPerPropertiesUpdate)
            .Default(10000);

        RegisterParameter("active_session_penality", ActiveSessionPenality)
            .Default(0.0001);
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
