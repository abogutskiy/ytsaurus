#pragma once

#include "public.h"
#include "tablet_cell.h"
#include "tablet_cell_bundle.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/chunk_server/chunk_tree_statistics.h>

#include <yt/server/hydra/entity_map.h>
#include <yt/server/hydra/mutation.h>

#include <yt/server/object_server/public.h>

#include <yt/server/table_server/public.h>

#include <yt/server/tablet_server/tablet_manager.pb.h>

#include <yt/ytlib/table_client/public.h>

namespace NYT {
namespace NTabletServer {

////////////////////////////////////////////////////////////////////////////////

class TTabletManager
    : public TRefCounted
{
public:
    explicit TTabletManager(
        TTabletManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);
    ~TTabletManager();

    void Initialize();

    int GetAssignedTabletCellCount(const Stroka& address) const;

    TTabletStatistics GetTabletStatistics(const TTablet* tablet);


    void MountTable(
        NTableServer::TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        TTabletCell* hintCell,
        bool freeze);

    void UnmountTable(
        NTableServer::TTableNode* table,
        bool force,
        int firstTabletIndex = -1,
        int lastTabletIndex = -1);

    void RemountTable(
        NTableServer::TTableNode* table,
        int firstTabletIndex = -1,
        int lastTabletIndex = -1);

    void FreezeTable(
        NTableServer::TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex);

    void UnfreezeTable(
        NTableServer::TTableNode* table,
        int firstTabletIndex = -1,
        int lastTabletIndex = -1);

    void DestroyTable(
        NTableServer::TTableNode* table);

    void ReshardTable(
        NTableServer::TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount,
        const std::vector<NTableClient::TOwningKey>& pivotKeys);


    struct TCloneTableData
        : public TIntrinsicRefCounted
    {
        NCypressServer::ENodeCloneMode Mode;
        std::vector<TTablet*> Tablets;
    };

    using TCloneTableDataPtr = TIntrusivePtr<TCloneTableData>;

    TCloneTableDataPtr BeginCloneTable(
        NTableServer::TTableNode* sourceTable,
        NTableServer::TTableNode* clonedTable,
        NCypressServer::ENodeCloneMode mode);
    void CommitCloneTable(
        NTableServer::TTableNode* sourceTable,
        NTableServer::TTableNode* clonedTable,
        TCloneTableDataPtr data);
    void RollbackCloneTable(
        NTableServer::TTableNode* sourceTable,
        NTableServer::TTableNode* clonedTable,
        TCloneTableDataPtr data);

    void MakeTableDynamic(NTableServer::TTableNode* table);
    void MakeTableStatic(NTableServer::TTableNode* table);


    DECLARE_ENTITY_MAP_ACCESSORS(TabletCellBundle, TTabletCellBundle);
    TTabletCellBundle* FindTabletCellBundleByName(const Stroka& name);
    TTabletCellBundle* GetTabletCellBundleByNameOrThrow(const Stroka& name);
    void RenameTabletCellBundle(TTabletCellBundle* cellBundle, const Stroka& newName);
    TTabletCellBundle* GetDefaultTabletCellBundle();
    void SetTabletCellBundle(NTableServer::TTableNode* table, TTabletCellBundle* cellBundle);

    DECLARE_ENTITY_MAP_ACCESSORS(TabletCell, TTabletCell);
    TTabletCell* GetTabletCellOrThrow(const TTabletCellId& id);

    DECLARE_ENTITY_MAP_ACCESSORS(Tablet, TTablet);

private:
    class TTabletCellBundleTypeHandler;
    class TTabletCellTypeHandler;
    class TTabletTypeHandler;
    class TImpl;

    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TTabletManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
