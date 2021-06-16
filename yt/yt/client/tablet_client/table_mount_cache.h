#pragma once

#include "public.h"

#include <yt/yt/client/hive/public.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/object_client/public.h>

#include <yt/yt_proto/yt/client/table_chunk_format/proto/chunk_meta.pb.h>
#include <yt/yt/client/table_client/public.h>
#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/table_client/versioned_row.h>

#include <yt/yt/client/ypath/public.h>

#include <yt/yt/core/actions/future.h>

#include <yt/yt/core/misc/small_vector.h>

#include <util/datetime/base.h>

namespace NYT::NTabletClient {

////////////////////////////////////////////////////////////////////////////////

struct TTabletInfo
    : public TRefCounted
{
    TTabletId TabletId;
    NHydra::TRevision MountRevision = NHydra::NullRevision;
    ETabletState State;
    EInMemoryMode InMemoryMode;
    NTableClient::TLegacyOwningKey PivotKey;
    TTabletCellId CellId;
    NObjectClient::TObjectId TableId;
    TInstant UpdateTime;
    std::vector<TWeakPtr<TTableMountInfo>> Owners;

    NTableClient::TKeyBound GetLowerKeyBound() const;
};

DEFINE_REFCOUNTED_TYPE(TTabletInfo)

////////////////////////////////////////////////////////////////////////////////

struct TTableReplicaInfo
    : public TRefCounted
{
    TTableReplicaId ReplicaId;
    TString ClusterName;
    NYPath::TYPath ReplicaPath;
    ETableReplicaMode Mode;
};

DEFINE_REFCOUNTED_TYPE(TTableReplicaInfo)

////////////////////////////////////////////////////////////////////////////////

//! Describes the primary and the auxiliary schemas derived from the table schema.
//! Cf. TTableSchema::ToXXX methods.
DEFINE_ENUM(ETableSchemaKind,
    // Schema assigned to Cypress node, as is.
    (Primary)
    // Schema used for inserting rows.
    (Write)
    // Schema used for querying rows.
    (Query)
    // Schema used for deleting rows.
    (Delete)
    // Schema used for writing versioned rows (during replication).
    (VersionedWrite)
    // Schema used for looking up rows.
    (Lookup)
    // For sorted schemas, coincides with primary.
    // For ordered, contains an additional tablet index columns.
    (PrimaryWithTabletIndex)
);

struct TTableMountInfo
    : public TRefCounted
{
    NYPath::TYPath Path;
    NObjectClient::TObjectId TableId;
    TEnumIndexedVector<ETableSchemaKind, NTableClient::TTableSchemaPtr> Schemas;

    bool Dynamic;
    TTableReplicaId UpstreamReplicaId;
    bool NeedKeyEvaluation;

    std::vector<TTabletInfoPtr> Tablets;
    std::vector<TTabletInfoPtr> MountedTablets;

    std::vector<TTableReplicaInfoPtr> Replicas;

    //! For sorted tables, these are -infinity and +infinity.
    //! For ordered tablets, these are |[0]| and |[tablet_count]| resp.
    NTableClient::TLegacyOwningKey LowerCapBound;
    NTableClient::TLegacyOwningKey UpperCapBound;

    // Master reply revision for master service cache invalidation.
    NHydra::TRevision PrimaryRevision;
    NHydra::TRevision SecondaryRevision;

    bool EnableDetailedProfiling = false;

    bool IsSorted() const;
    bool IsOrdered() const;
    bool IsReplicated() const;

    TTabletInfoPtr GetTabletByIndexOrThrow(int tabletIndex) const;
    TTabletInfoPtr GetTabletForRow(TRange<NTableClient::TUnversionedValue> row) const;
    TTabletInfoPtr GetTabletForRow(NTableClient::TUnversionedRow row) const;
    TTabletInfoPtr GetTabletForRow(NTableClient::TVersionedRow row) const;
    TTabletInfoPtr GetRandomMountedTablet() const;

    void ValidateDynamic() const;
    void ValidateSorted() const;
    void ValidateOrdered() const;
    void ValidateNotReplicated() const;
    void ValidateReplicated() const;
};

DEFINE_REFCOUNTED_TYPE(TTableMountInfo)

////////////////////////////////////////////////////////////////////////////////

struct ITableMountCache
    : public virtual TRefCounted
{
    virtual TFuture<TTableMountInfoPtr> GetTableInfo(const NYPath::TYPath& path) = 0;
    virtual TTabletInfoPtr FindTabletInfo(TTabletId tabletId) = 0;
    virtual void InvalidateTablet(TTabletInfoPtr tabletInfo) = 0;
    virtual std::pair<bool, TTabletInfoPtr> InvalidateOnError(const TError& error, bool forceRetry) = 0;

    virtual void Clear() = 0;
};

DEFINE_REFCOUNTED_TYPE(ITableMountCache)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletClient

