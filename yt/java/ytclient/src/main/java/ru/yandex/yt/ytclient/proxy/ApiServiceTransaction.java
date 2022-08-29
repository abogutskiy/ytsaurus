package ru.yandex.yt.ytclient.proxy;

import java.time.Duration;
import java.util.AbstractQueue;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;

import javax.annotation.Nullable;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import ru.yandex.inside.yt.kosher.common.GUID;
import ru.yandex.inside.yt.kosher.common.YtTimestamp;
import ru.yandex.inside.yt.kosher.impl.ytree.object.serializers.YTreeObjectSerializer;
import ru.yandex.inside.yt.kosher.ytree.YTreeNode;
import ru.yandex.yt.rpcproxy.TCheckPermissionResult;
import ru.yandex.yt.ytclient.operations.Operation;
import ru.yandex.yt.ytclient.proxy.request.CheckPermission;
import ru.yandex.yt.ytclient.proxy.request.ConcatenateNodes;
import ru.yandex.yt.ytclient.proxy.request.CopyNode;
import ru.yandex.yt.ytclient.proxy.request.CreateNode;
import ru.yandex.yt.ytclient.proxy.request.ExistsNode;
import ru.yandex.yt.ytclient.proxy.request.GetFileFromCache;
import ru.yandex.yt.ytclient.proxy.request.GetFileFromCacheResult;
import ru.yandex.yt.ytclient.proxy.request.GetNode;
import ru.yandex.yt.ytclient.proxy.request.LinkNode;
import ru.yandex.yt.ytclient.proxy.request.ListNode;
import ru.yandex.yt.ytclient.proxy.request.LockNode;
import ru.yandex.yt.ytclient.proxy.request.LockNodeResult;
import ru.yandex.yt.ytclient.proxy.request.MapOperation;
import ru.yandex.yt.ytclient.proxy.request.MapReduceOperation;
import ru.yandex.yt.ytclient.proxy.request.MergeOperation;
import ru.yandex.yt.ytclient.proxy.request.MoveNode;
import ru.yandex.yt.ytclient.proxy.request.PutFileToCache;
import ru.yandex.yt.ytclient.proxy.request.PutFileToCacheResult;
import ru.yandex.yt.ytclient.proxy.request.ReadFile;
import ru.yandex.yt.ytclient.proxy.request.ReadTable;
import ru.yandex.yt.ytclient.proxy.request.ReduceOperation;
import ru.yandex.yt.ytclient.proxy.request.RemoteCopyOperation;
import ru.yandex.yt.ytclient.proxy.request.RemoveNode;
import ru.yandex.yt.ytclient.proxy.request.SetNode;
import ru.yandex.yt.ytclient.proxy.request.SortOperation;
import ru.yandex.yt.ytclient.proxy.request.StartOperation;
import ru.yandex.yt.ytclient.proxy.request.TransactionalOptions;
import ru.yandex.yt.ytclient.proxy.request.WriteFile;
import ru.yandex.yt.ytclient.proxy.request.WriteTable;
import ru.yandex.yt.ytclient.rpc.RpcError;
import ru.yandex.yt.ytclient.rpc.RpcErrorCode;
import ru.yandex.yt.ytclient.wire.UnversionedRowset;
import ru.yandex.yt.ytclient.wire.VersionedRowset;

public class ApiServiceTransaction implements TransactionalClient, AutoCloseable, Abortable {
    enum State {
        ACTIVE,
        COMMITTING,
        COMMITTED,
        CLOSED,
    }
    private static final Logger logger = LoggerFactory.getLogger(ApiServiceTransaction.class);

    private final ApiServiceClientImpl client;
    private final GUID id;
    private final YtTimestamp startTimestamp;
    private final boolean ping;
    private final boolean sticky;
    private final TransactionalOptions transactionalOptions;
    private final Duration pingPeriod;
    private final Duration failedPingRetryPeriod;
    private final ScheduledExecutorService executor;
    private final CompletableFuture<Void> transactionCompleteFuture = new CompletableFuture<>();
    private final AtomicReference<State> state = new AtomicReference<>(State.ACTIVE);
    private final AtomicReference<Boolean> forcedPingStop = new AtomicReference<>(false);
    private final AbstractQueue<CompletableFuture<Void>> modifyRowsResults = new ConcurrentLinkedQueue<>();
    private final Consumer<Exception> onPingFailed;

    @SuppressWarnings("checkstyle:ParameterNumber")
    ApiServiceTransaction(
            ApiServiceClientImpl client,
            GUID id,
            YtTimestamp startTimestamp,
            boolean ping,
            boolean pingAncestors,
            boolean sticky,
            Duration pingPeriod,
            ScheduledExecutorService executor
    ) {
        this(
                client,
                id,
                startTimestamp,
                ping,
                pingAncestors,
                sticky,
                pingPeriod,
                null,
                executor,
                null
        );
    }

    @SuppressWarnings("checkstyle:ParameterNumber")
    ApiServiceTransaction(
            ApiServiceClientImpl client,
            GUID id,
            YtTimestamp startTimestamp,
            boolean ping,
            boolean pingAncestors,
            boolean sticky,
            Duration pingPeriod,
            Duration failedPingRetryPeriod,
            ScheduledExecutorService executor,
            Consumer<Exception> onPingFailed
    ) {
        this.client = client;
        this.id = Objects.requireNonNull(id);
        this.startTimestamp = Objects.requireNonNull(startTimestamp);
        this.ping = ping;
        this.sticky = sticky;
        this.transactionalOptions = new TransactionalOptions(id, sticky);
        this.pingPeriod = pingPeriod;
        this.failedPingRetryPeriod = isValidPingPeriod(failedPingRetryPeriod) ? failedPingRetryPeriod : pingPeriod;
        this.executor = executor;
        this.onPingFailed = onPingFailed;

        if (isValidPingPeriod(pingPeriod)) {
            executor.schedule(this::runPeriodicPings, pingPeriod.toMillis(), TimeUnit.MILLISECONDS);
        }
    }

    @Override
    public TransactionalClient getRootClient() {
        return client;
    }

    @Override
    public String toString() {
        return "Transaction(" + client + ")@" + id;
    }

    public ApiServiceClient getClient() {
        return client;
    }

    public GUID getId() {
        return id;
    }

    public YtTimestamp getStartTimestamp() {
        return startTimestamp;
    }

    public boolean isPing() {
        return ping;
    }

    public boolean isSticky() {
        return sticky;
    }

    CompletableFuture<Void> getTransactionCompleteFuture() {
        return transactionCompleteFuture;
    }

    boolean isActive() {
        return state.get() == State.ACTIVE;
    }

    @SuppressWarnings("BooleanMethodIsAlwaysInverted")
    private boolean isPingableState() {
        State currentState = state.get();
        return (currentState == State.ACTIVE || currentState == State.COMMITTING) && !forcedPingStop.get();
    }

    public void stopPing() {
        forcedPingStop.set(true);
    }

    private void runPeriodicPings() {
        if (!isPingableState()) {
            return;
        }

        ping().whenComplete((unused, ex) -> {
            long nextPingDelayMs;

            if (ex == null) {
                nextPingDelayMs = pingPeriod.toMillis();
            } else {
                nextPingDelayMs = failedPingRetryPeriod.toMillis();

                if (onPingFailed != null) {
                    onPingFailed.accept(ex instanceof Exception ? (Exception) ex : new RuntimeException(ex));
                }
                if (ex instanceof RpcError) {
                    if (((RpcError) ex).matches(RpcErrorCode.NoSuchTransaction.getCode())) {
                        return;
                    }
                }
            }

            if (!isPingableState()) {
                return;
            }
            executor.schedule(this::runPeriodicPings, nextPingDelayMs, TimeUnit.MILLISECONDS);
        });
    }

    public CompletableFuture<Void> ping() {
        return client.pingTransaction(id);
    }

    private void throwWrongState(State expectedOldState, State newState) {
        // Yep, this state is a little bit outdated but Java8 doesn't have compareAndExchange,
        // so we do our best here. In any case it's a bug.
        State currentState = state.get();
        throw new IllegalStateException(
                String.format(
                        "Failed to set transaction into '%s' state; " +
                                "expected state: '%s'; current state (maybe outdated): '%s'",
                        newState,
                        expectedOldState,
                        currentState
                ));
    }

    private void updateState(State expectedOldState, State newState) {
        boolean set = state.compareAndSet(expectedOldState, newState);
        if (!set) {
            throwWrongState(expectedOldState, newState);
        }
    }

    public CompletableFuture<Void> commit() {
        updateState(State.ACTIVE, State.COMMITTING);

        List<CompletableFuture<Void>> allModifyRowsResults = new ArrayList<>();
        CompletableFuture<Void> current;
        while ((current = modifyRowsResults.poll()) != null) {
            allModifyRowsResults.add(current);
        }

        CompletableFuture<Void> allModifiesCompleted = CompletableFuture.allOf(
                allModifyRowsResults.toArray(new CompletableFuture[0])
        );

        return allModifiesCompleted.whenComplete((unused, error) -> {
            if (error != null) {
                logger.warn("Cannot commit transaction since modify rows failed:", error);
                abortImpl(false);
            }
        }).thenCompose(unused -> client.commitTransaction(id)
                .whenComplete((result, error) -> {
                    if (error == null) {
                        updateState(State.COMMITTING, State.COMMITTED);
                    } else {
                        state.set(State.CLOSED);
                    }
                    transactionCompleteFuture.complete(null);
                }));
    }

    public CompletableFuture<Void> abort() {
        return abortImpl(true);
    }

    private CompletableFuture<Void> abortImpl(boolean complainWrongState) {
        State oldState = state.getAndSet(State.CLOSED);
        if (oldState == State.ACTIVE || oldState == State.COMMITTING && !complainWrongState) {
            // dont wait for answer
            return client.abortTransaction(id)
                    .whenComplete((result, error) -> transactionCompleteFuture.complete(null));
        } else if (complainWrongState) {
            throwWrongState(State.ACTIVE, State.CLOSED);
        }

        return CompletableFuture.completedFuture(null);
    }

    /**
     * Aborts transaction unless it was committed or aborted before.
     */
    @Override
    public void close() {
        // NB. We intentionally ignore all errors of abort.
        abortImpl(false);
    }

    @Override
    public CompletableFuture<UnversionedRowset> lookupRows(AbstractLookupRowsRequest<?> request) {
        return client.lookupRows(request.setTimestamp(startTimestamp));
    }

    @Override
    public <T> CompletableFuture<List<T>> lookupRows(
            AbstractLookupRowsRequest<?> request,
            YTreeObjectSerializer<T> serializer
    ) {
        return client.lookupRows(request.setTimestamp(startTimestamp), serializer);
    }

    @Override
    public CompletableFuture<VersionedRowset> versionedLookupRows(AbstractLookupRowsRequest<?> request) {
        return client.versionedLookupRows(request.setTimestamp(startTimestamp));
    }

    @Override
    public <T> CompletableFuture<List<T>> versionedLookupRows(AbstractLookupRowsRequest<?> request,
                                                              YTreeObjectSerializer<T> serializer) {
        return client.versionedLookupRows(request.setTimestamp(startTimestamp), serializer);
    }

    public CompletableFuture<UnversionedRowset> selectRows(String query) {
        return selectRows(SelectRowsRequest.of(query));
    }

    @Override
    public CompletableFuture<SelectRowsResult> selectRowsV2(SelectRowsRequest request) {
        return client.selectRowsV2(request.setTimestamp(startTimestamp));
    }

    @Override
    public CompletableFuture<UnversionedRowset> selectRows(SelectRowsRequest request) {
        return client.selectRows(request.setTimestamp(startTimestamp));
    }

    @Override
    public <T> CompletableFuture<List<T>> selectRows(SelectRowsRequest request, YTreeObjectSerializer<T> serializer) {
        return client.selectRows(request.setTimestamp(startTimestamp), serializer);
    }

    public CompletableFuture<Void> modifyRows(AbstractModifyRowsRequest<?> request) {
        // TODO: hide id to request
        CompletableFuture<Void> result = client.modifyRows(id, request);
        modifyRowsResults.add(result);
        return result;
    }

    /* nodes */

    @Override
    public CompletableFuture<GUID> createNode(CreateNode req) {
        return client.createNode(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<Boolean> existsNode(ExistsNode req) {
        return client.existsNode(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<YTreeNode> getNode(GetNode req) {
        return client.getNode(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<YTreeNode> listNode(ListNode req) {
        return client.listNode(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<Void> removeNode(RemoveNode req) {
        return client.removeNode(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<Void> setNode(SetNode req) {
        return client.setNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<LockNodeResult> lockNode(LockNode req) {
        return client.lockNode(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<GUID> copyNode(CopyNode req) {
        return client.copyNode(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<GUID> moveNode(MoveNode req) {
        return client.moveNode(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<GUID> linkNode(LinkNode req) {
        return client.linkNode(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<Void> concatenateNodes(ConcatenateNodes req) {
        return client.concatenateNodes(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public <T> CompletableFuture<TableReader<T>> readTable(ReadTable<T> req) {
        return client.readTable(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public <T> CompletableFuture<TableWriter<T>> writeTable(WriteTable<T> req) {
        return client.writeTable(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<FileReader> readFile(ReadFile req) {
        return client.readFile(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<FileWriter> writeFile(WriteFile req) {
        return client.writeFile(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<GUID> startOperation(StartOperation req) {
        return client.startOperation(req.setTransactionOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<Operation> startMap(MapOperation req) {
        return client.startMap(req.toBuilder().setTransactionalOptions(transactionalOptions).build());
    }

    @Override
    public CompletableFuture<Operation> startReduce(ReduceOperation req) {
        return client.startReduce(req.toBuilder().setTransactionalOptions(transactionalOptions).build());
    }

    @Override
    public CompletableFuture<Operation> startSort(SortOperation req) {
        return client.startSort(req.toBuilder().setTransactionalOptions(transactionalOptions).build());
    }

    @Override
    public CompletableFuture<Operation> startMapReduce(MapReduceOperation req) {
        return client.startMapReduce(req.toBuilder().setTransactionalOptions(transactionalOptions).build());
    }

    @Override
    public CompletableFuture<Operation> startMerge(MergeOperation req) {
        return client.startMerge(req.toBuilder().setTransactionalOptions(transactionalOptions).build());
    }

    @Override
    public CompletableFuture<Operation> startRemoteCopy(RemoteCopyOperation req) {
        return client.startRemoteCopy(req.toBuilder().setTransactionalOptions(transactionalOptions).build());
    }

    @Override
    public CompletableFuture<TCheckPermissionResult> checkPermission(CheckPermission req) {
        return client.checkPermission(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<GetFileFromCacheResult> getFileFromCache(GetFileFromCache req) {
        return client.getFileFromCache(req.setTransactionalOptions(transactionalOptions));
    }

    @Override
    public CompletableFuture<PutFileToCacheResult> putFileToCache(PutFileToCache req) {
        return client.putFileToCache(req.setTransactionalOptions(transactionalOptions));
    }

    /**
     * Return address of a proxy that is used for this transaction.
     */
    @Nullable
    String getRpcProxyAddress() {
        return client.getRpcProxyAddress();
    }

    private static boolean isValidPingPeriod(Duration pingPeriod) {
        return pingPeriod != null && !pingPeriod.isZero() && !pingPeriod.isNegative();
    }
}
