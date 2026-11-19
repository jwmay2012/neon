/* Isolated PostgreSQL fixture for the production lfc_get_state implementation. */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/timestamp.h"
#include "common/pg_prng.h"
#include "port/pg_bitutils.h"
#include "file_cache.h"
#include "bitmap.h"
#include "validate.inc"

PG_MODULE_MAGIC;
void _PG_init(void);
PG_FUNCTION_INFO_V1(snapshot_seed);
PG_FUNCTION_INFO_V1(snapshot_capture);
PG_FUNCTION_INFO_V1(snapshot_probe);
PG_FUNCTION_INFO_V1(snapshot_shuffle);

#define MAX_BLOCKS_PER_CHUNK 128
static int lfc_blocks_per_chunk = 128;
static int lfc_chunk_size_log = 7;
#include "entry.inc"
#include "format.inc"

/* Only the control fields consulted by the captured function are needed. */
typedef struct SnapshotControl
{
	uint64 generation;
	uint32 used, pinned, limit;
	dlist_head lru;
	int chunk_log;
} SnapshotControl;

static SnapshotControl *lfc_ctl;
static HTAB *lfc_hash;
static LWLock *lfc_lock;
static int fixture_capacity = 65536;
static shmem_request_hook_type previous_request;
static shmem_startup_hook_type previous_startup;
static uint64 acquisitions, releases, allocations_locked, max_hold_us;
static uint64 hold_samples[65536];
static TimestampTz acquired_at;
static const char *action;
static pg_prng_state random_state;
static int fixture_batch_size(void);

static bool lfc_maybe_disabled(void) { return !lfc_ctl || lfc_ctl->limit == 0; }
#define LFC_ENABLED() (lfc_ctl->limit != 0)

static BufferTag
fixture_key(uint32 index)
{
	BufferTag tag;
	BufTagInit(tag, 20000, MAIN_FORKNUM, index * lfc_blocks_per_chunk, 1663, 16384);
	return tag;
}

static void
fixture_request(void)
{
	if (previous_request) previous_request();
	RequestAddinShmemSpace(MAXALIGN(sizeof(SnapshotControl)) +
						  hash_estimate_size(fixture_capacity + 1, FILE_CACHE_ENRTY_SIZE));
	RequestNamedLWLockTranche("lfc_snapshot_test", 1);
}

static void
fixture_startup(void)
{
	HASHCTL info = {0};
	bool found;

	if (previous_startup) previous_startup();
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	lfc_ctl = ShmemInitStruct("lfc_snapshot_test", sizeof(SnapshotControl), &found);
	if (!found) memset(lfc_ctl, 0, sizeof(SnapshotControl));
	lfc_lock = &GetNamedLWLockTranche("lfc_snapshot_test")[0].lock;
	info.keysize = sizeof(BufferTag);
	info.entrysize = FILE_CACHE_ENRTY_SIZE;
	lfc_hash = ShmemInitHash("lfc_snapshot_test_hash", fixture_capacity + 1,
						   fixture_capacity + 1, &info, HASH_ELEM | HASH_BLOBS);
	if (!found) dlist_init(&lfc_ctl->lru);
	LWLockRelease(AddinShmemInitLock);
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		elog(ERROR, "load snapshot_test through shared_preload_libraries");
	DefineCustomIntVariable("snapshot_test.capacity", "Fixture cache chunks", NULL,
							&fixture_capacity, 65536, 1024, 6291456, PGC_POSTMASTER, 0,
							NULL, NULL, NULL);
	previous_request = shmem_request_hook;
	shmem_request_hook = fixture_request;
	previous_startup = shmem_startup_hook;
	shmem_startup_hook = fixture_startup;
}

Datum
snapshot_seed(PG_FUNCTION_ARGS)
{
	int count = PG_GETARG_INT32(0);
	int chunk_log = PG_GETARG_INT32(1);
	bool valid = PG_GETARG_BOOL(2);
	HASH_SEQ_STATUS seq;
	FileCacheEntry *entry;

	if (count < 0 || count > fixture_capacity || chunk_log < 0 || chunk_log > 7)
		elog(ERROR, "invalid fixture size");
	lfc_chunk_size_log = chunk_log;
	lfc_blocks_per_chunk = 1 << chunk_log;
	LWLockAcquire(lfc_lock, LW_EXCLUSIVE);
	hash_seq_init(&seq, lfc_hash);
	while ((entry = hash_seq_search(&seq)) != NULL)
		hash_search(lfc_hash, &entry->key, HASH_REMOVE, NULL);
	lfc_ctl->generation++;
	lfc_ctl->used = count;
	lfc_ctl->pinned = 0;
	lfc_ctl->limit = fixture_capacity;
	lfc_ctl->chunk_log = chunk_log;
	dlist_init(&lfc_ctl->lru);
	for (int i = 0; i < count; i++)
	{
		BufferTag key = fixture_key(i);
		if (!valid) BufTagInit(key, 20000, MAIN_FORKNUM, i * lfc_blocks_per_chunk, InvalidOid, 16384);
		entry = hash_search(lfc_hash, &key, HASH_ENTER, NULL);
		entry->access_count = 0;
		/* Include holes and all three nonzero page states in every chunk. */
		memset(entry->state, 0, (lfc_blocks_per_chunk * 2 + 31) / 32 * 4);
		for (int j = 0; j < lfc_blocks_per_chunk; j++) SET_STATE(entry, j, j % 4);
		dlist_push_tail(&lfc_ctl->lru, &entry->list_node);
	}
	LWLockRelease(lfc_lock);
	PG_RETURN_VOID();
}

Datum
snapshot_shuffle(PG_FUNCTION_ARGS)
{
	FileCacheEntry **entries = palloc(sizeof(FileCacheEntry *) * lfc_ctl->used);
	dlist_iter iter;
	uint32 count = 0;

	pg_prng_seed(&random_state, 42);
	LWLockAcquire(lfc_lock, LW_EXCLUSIVE);
	dlist_foreach(iter, &lfc_ctl->lru)
		entries[count++] = dlist_container(FileCacheEntry, list_node, iter.cur);
	for (uint32 i = count; i > 1; i--)
	{
		uint32 j = pg_prng_uint64(&random_state) % i;
		FileCacheEntry *entry = entries[i - 1];
		entries[i - 1] = entries[j];
		entries[j] = entry;
	}
	dlist_init(&lfc_ctl->lru);
	for (uint32 i = 0; i < count; i++) dlist_push_tail(&lfc_ctl->lru, &entries[i]->list_node);
	LWLockRelease(lfc_lock);
	pfree(entries);
	PG_RETURN_VOID();
}

/* Deliberate interleavings run only after the production function unlocks. */
static void
fixture_interleave(void)
{
	int count = strcmp(action, "pin_all") == 0 ? 8 : 1;
	bool churn = strcmp(action, "churn") == 0;

	if (strcmp(action, "empty") == 0 && releases == 1)
	{
		LWLockAcquire(lfc_lock, LW_EXCLUSIVE);
		while (!dlist_is_empty(&lfc_ctl->lru))
		{
			FileCacheEntry *entry = dlist_container(FileCacheEntry, list_node, dlist_pop_head_node(&lfc_ctl->lru));
			hash_search(lfc_hash, &entry->key, HASH_REMOVE, NULL);
		}
		lfc_ctl->used = 0;
		LWLockRelease(lfc_lock);
		return;
	}
	if (strcmp(action, "none") == 0 || releases < 2 || (!churn && releases != 2)) return;
	LWLockAcquire(lfc_lock, LW_EXCLUSIVE);
	if (strcmp(action, "reset") == 0) lfc_ctl->generation++;
	if (strcmp(action, "disable") == 0) lfc_ctl->limit = 0;
	for (int i = 0; i < (churn ? 8 : count); i++)
	{
		uint32 index = churn ? pg_prng_uint64(&random_state) % lfc_ctl->used : lfc_ctl->used - fixture_batch_size() - 1 - i;
		BufferTag key = fixture_key(index);
		FileCacheEntry *entry = hash_search(lfc_hash, &key, HASH_FIND, NULL);

		if (entry == NULL || entry->access_count != 0) continue;
		if (strcmp(action, "pin") == 0 || strcmp(action, "pin_all") == 0)
		{
			dlist_delete(&entry->list_node);
			entry->access_count = 1;
			lfc_ctl->pinned++;
		}
		if (strcmp(action, "evict") == 0)
		{
			dlist_delete(&entry->list_node);
			hash_search(lfc_hash, &key, HASH_REMOVE, NULL);
			lfc_ctl->used--;
			/* Reuse a hash entry for another key while the snapshot is unlocked. */
			key = fixture_key(fixture_capacity - 1);
			entry = hash_search(lfc_hash, &key, HASH_ENTER, NULL);
			entry->access_count = 0;
			memset(entry->state, 0, (lfc_blocks_per_chunk * 2 + 31) / 32 * 4);
			for (int j = 0; j < lfc_blocks_per_chunk; j++) SET_STATE(entry, j, j % 4);
			dlist_push_tail(&lfc_ctl->lru, &entry->list_node);
			lfc_ctl->used++;
		}
		if (strcmp(action, "touch") == 0 || churn)
		{
			dlist_delete(&entry->list_node);
			dlist_push_tail(&lfc_ctl->lru, &entry->list_node);
		}
	}
	LWLockRelease(lfc_lock);
	if (strcmp(action, "cancel") == 0)
	{
		QueryCancelPending = true;
		InterruptPending = true;
	}
}

static bool
capture_acquire(LWLock *lock, LWLockMode mode)
{
	bool result = LWLockAcquire(lock, mode);
	acquisitions++;
	acquired_at = GetCurrentTimestamp();
	return result;
}

static void
capture_release(LWLock *lock)
{
	uint64 elapsed = GetCurrentTimestamp() - acquired_at;
	max_hold_us = Max(max_hold_us, elapsed);
	LWLockRelease(lock);
	if (releases < lengthof(hold_samples)) hold_samples[releases] = elapsed;
	releases++;
	fixture_interleave();
}

static void *
capture_allocate(Size size)
{
	if (LWLockHeldByMe(lfc_lock)) allocations_locked++;
	return palloc0(size);
}

/* Generated from file_cache.c by run.zsh: this is the real implementation. */
#define LWLockAcquire capture_acquire
#define LWLockRelease capture_release
#define palloc0 capture_allocate
#include "snapshot.inc"
#undef palloc0
#undef LWLockRelease
#undef LWLockAcquire

static int
fixture_batch_size(void)
{
#ifdef LFC_STATE_BATCH_SIZE
	return LFC_STATE_BATCH_SIZE;
#else
	return 128;
#endif
}

static int
compare_holds(const void *a, const void *b)
{
	uint64 left = *(const uint64 *)a, right = *(const uint64 *)b;
	return (left > right) - (left < right);
}

Datum
snapshot_capture(PG_FUNCTION_ARGS)
{
	int limit = PG_GETARG_INT32(0);
	FileCacheState *state;
	TimestampTz started;
	uint64 elapsed;
	uint32 unique = 0;
	bool ordered = true;
	bool *seen;
	Size bitmap_bytes;
	size_t samples;

	action = text_to_cstring(PG_GETARG_TEXT_PP(1));
	lfc_chunk_size_log = lfc_ctl->chunk_log;
	lfc_blocks_per_chunk = 1 << lfc_chunk_size_log;
	acquisitions = releases = allocations_locked = max_hold_us = 0;
	pg_prng_seed(&random_state, 42);
	started = GetCurrentTimestamp();
	state = lfc_get_state(limit);
	elapsed = GetCurrentTimestamp() - started;
	samples = Min(releases, lengthof(hold_samples));
	qsort(hold_samples, samples, sizeof(uint64), compare_holds);
	if (state == NULL) PG_RETURN_TEXT_P(cstring_to_text("null"));
	bitmap_bytes = ((size_t)state->n_chunks * lfc_blocks_per_chunk + 7) / 8;
	if (VARSIZE(state) != FILE_CACHE_STATE_SIZE(state) ||
		pg_popcount((char *)FILE_CACHE_STATE_BITMAP(state), bitmap_bytes) != state->n_pages)
		elog(ERROR, "malformed snapshot size or bitmap");
	seen = palloc0(fixture_capacity);
	for (uint32 i = 0; i < state->n_chunks; i++)
	{
		uint32 index = state->chunks[i].blockNum / lfc_blocks_per_chunk;
		if (!BufferTagIsValid(&state->chunks[i]) || index >= fixture_capacity)
			elog(ERROR, "invalid snapshot tag");
		if (!seen[index]) { unique++; seen[index] = true; }
		if (i > 0 && state->chunks[i - 1].blockNum != state->chunks[i].blockNum + lfc_blocks_per_chunk)
			ordered = false;
		for (int j = 0; j < lfc_blocks_per_chunk; j++)
			if (!!BITMAP_ISSET(FILE_CACHE_STATE_BITMAP(state), (size_t)i * lfc_blocks_per_chunk + j) != (j % 4 != 0))
				elog(ERROR, "snapshot page bits differ from fixture");
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf(
		"{\"chunks\":%u,\"pages\":%u,\"unique\":%u,\"bytes\":%u,\"ordered\":%s,\"batch_size\":%d,"
		"\"locks\":%llu,\"allocations_locked\":%llu,\"p50_hold_us\":%llu,\"p95_hold_us\":%llu,\"max_hold_us\":%llu,\"elapsed_us\":%llu}",
		state->n_chunks, state->n_pages, unique, VARSIZE(state), ordered ? "true" : "false",
		fixture_batch_size(),
		(unsigned long long) acquisitions, (unsigned long long) allocations_locked,
		(unsigned long long) (samples ? hold_samples[samples / 2] : 0),
		(unsigned long long) (samples ? hold_samples[samples * 95 / 100] : 0),
		(unsigned long long) max_hold_us, (unsigned long long) elapsed)));
}

Datum
snapshot_probe(PG_FUNCTION_ARGS)
{
	TimestampTz end = GetCurrentTimestamp() + PG_GETARG_INT32(0) * INT64CONST(1000000);
	uint64 max_wait_us = 0, probes = 0;

	while (GetCurrentTimestamp() < end)
	{
		TimestampTz begin = GetCurrentTimestamp();
		CHECK_FOR_INTERRUPTS();
		LWLockAcquire(lfc_lock, LW_EXCLUSIVE);
		max_wait_us = Max(max_wait_us, GetCurrentTimestamp() - begin);
		LWLockRelease(lfc_lock);
		probes++;
		pg_usleep(100);
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("{\"probes\":%llu,\"max_wait_us\":%llu}",
		(unsigned long long) probes, (unsigned long long) max_wait_us)));
}
