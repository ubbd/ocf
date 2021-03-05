/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ocf/ocf.h"
#include "../ocf_priv.h"
#include "../ocf_cache_priv.h"
#include "../ocf_queue_priv.h"
#include "../ocf_freelist.h"
#include "engine_common.h"
#define OCF_ENGINE_DEBUG_IO_NAME "common"
#include "engine_debug.h"
#include "../utils/utils_cache_line.h"
#include "../ocf_request.h"
#include "../utils/utils_cleaner.h"
#include "../utils/utils_part.h"
#include "../metadata/metadata.h"
#include "../eviction/eviction.h"
#include "../promotion/promotion.h"
#include "../concurrency/ocf_concurrency.h"

void ocf_engine_error(struct ocf_request *req,
		bool stop_cache, const char *msg)
{
	struct ocf_cache *cache = req->cache;

	if (stop_cache)
		env_bit_clear(ocf_cache_state_running, &cache->cache_state);

	if (ocf_cache_log_rl(cache)) {
		ocf_core_log(req->core, log_err,
				"%s sector: %" ENV_PRIu64 ", bytes: %u\n", msg,
				BYTES_TO_SECTORS(req->byte_position),
				req->byte_length);
	}
}

void ocf_engine_lookup_map_entry(struct ocf_cache *cache,
		struct ocf_map_info *entry, ocf_core_id_t core_id,
		uint64_t core_line)
{
	ocf_cache_line_t line;
	ocf_cache_line_t hash;

	hash = ocf_metadata_hash_func(cache, core_line, core_id);

	/* Initially assume that we have cache miss.
	 * Hash points to proper bucket.
	 */
	entry->hash = hash;
	entry->status = LOOKUP_MISS;
	entry->coll_idx = cache->device->collision_table_entries;
	entry->core_line = core_line;
	entry->core_id = core_id;

	line = ocf_metadata_get_hash(cache, hash);

	while (line != cache->device->collision_table_entries) {
		ocf_core_id_t curr_core_id;
		uint64_t curr_core_line;

		ocf_metadata_get_core_info(cache, line, &curr_core_id,
				&curr_core_line);

		if (core_id == curr_core_id && curr_core_line == core_line) {
			entry->coll_idx = line;
			entry->status = LOOKUP_HIT;
			break;
		}

		line = ocf_metadata_get_collision_next(cache, line);
	}
}

static inline int _ocf_engine_check_map_entry(struct ocf_cache *cache,
		struct ocf_map_info *entry, ocf_core_id_t core_id)
{
	ocf_core_id_t _core_id;
	uint64_t _core_line;

	if (entry->status == LOOKUP_MISS)
		return 0;

	ENV_BUG_ON(entry->coll_idx >= cache->device->collision_table_entries);

	ocf_metadata_get_core_info(cache, entry->coll_idx, &_core_id,
			&_core_line);

	if (core_id == _core_id && _core_line == entry->core_line)
		return 0;
	else
		return -1;
}

/* Returns true if core lines on index 'entry' and 'entry + 1' within the request
 * are physically contiguous.
 */
static inline bool ocf_engine_clines_phys_cont(struct ocf_request *req,
		uint32_t entry)
{
	struct ocf_map_info *entry1, *entry2;
	ocf_cache_line_t phys1, phys2;

	entry1 = &req->map[entry];
	entry2 = &req->map[entry + 1];

	if (entry1->status == LOOKUP_MISS || entry2->status == LOOKUP_MISS)
		return false;

	phys1 = ocf_metadata_map_lg2phy(req->cache, entry1->coll_idx);
	phys2 = ocf_metadata_map_lg2phy(req->cache, entry2->coll_idx);

	return phys1 < phys2 && phys1 + 1 == phys2;
}

void ocf_engine_patch_req_info(struct ocf_cache *cache,
		struct ocf_request *req, uint32_t idx)
{
	struct ocf_map_info *entry = &req->map[idx];

	ENV_BUG_ON(entry->status != LOOKUP_REMAPPED);

	req->info.insert_no++;

	if (idx > 0 && ocf_engine_clines_phys_cont(req, idx - 1))
		req->info.seq_no++;
	if (idx + 1 < req->core_line_count &&
			ocf_engine_clines_phys_cont(req, idx)) {
		req->info.seq_no++;
	}
}

static void ocf_engine_update_req_info(struct ocf_cache *cache,
		struct ocf_request *req, uint32_t idx)
{
	uint8_t start_sector = 0;
	uint8_t end_sector = ocf_line_end_sector(cache);
	struct ocf_map_info *entry = &(req->map[idx]);

	start_sector = ocf_map_line_start_sector(req, idx);
	end_sector = ocf_map_line_end_sector(req, idx);

	/* Handle return value */
	switch (entry->status) {
	case LOOKUP_HIT:
		if (metadata_test_valid_sec(cache, entry->coll_idx,
				start_sector, end_sector)) {
			req->info.hit_no++;
		} else {
			req->info.invalid_no++;
		}

		/* Check request is dirty */
		if (metadata_test_dirty(cache, entry->coll_idx)) {
			req->info.dirty_any++;

			/* Check if cache line is fully dirty */
			if (metadata_test_dirty_all_sec(cache, entry->coll_idx,
				start_sector, end_sector))
				req->info.dirty_all++;
		}

		if (req->part_id != ocf_metadata_get_partition_id(cache,
				entry->coll_idx)) {
			/*
			 * Need to move this cache line into other partition
			 */
			entry->re_part = true;
			req->info.re_part_no++;
		}

		break;
	case LOOKUP_INSERTED:
		req->info.insert_no++;
	case LOOKUP_MISS:
		break;
	case LOOKUP_REMAPPED:
		/* remapped cachelines are to be updated via
		 * ocf_engine_patch_req_info()
		 */
	default:
		ENV_BUG();
		break;
	}

	/* Check if cache hit is sequential */
	if (idx > 0 && ocf_engine_clines_phys_cont(req, idx - 1))
		req->info.seq_no++;
}

void ocf_engine_traverse(struct ocf_request *req)
{
	uint32_t i;
	uint64_t core_line;

	struct ocf_cache *cache = req->cache;
	ocf_core_id_t core_id = ocf_core_get_id(req->core);

	OCF_DEBUG_TRACE(req->cache);

	ocf_req_clear_info(req);

	for (i = 0, core_line = req->core_line_first;
			core_line <= req->core_line_last; core_line++, i++) {

		struct ocf_map_info *entry = &(req->map[i]);

		ocf_engine_lookup_map_entry(cache, entry, core_id,
				core_line);

		if (entry->status != LOOKUP_HIT) {
			/* There is miss then lookup for next map entry */
			OCF_DEBUG_PARAM(cache, "Miss, core line = %llu",
					entry->core_line);
			continue;
		}

		OCF_DEBUG_PARAM(cache, "Hit, cache line %u, core line = %llu",
				entry->coll_idx, entry->core_line);

		/* Update eviction (LRU) */
		ocf_eviction_set_hot_cache_line(cache, entry->coll_idx);

		ocf_engine_update_req_info(cache, req, i);
	}

	OCF_DEBUG_PARAM(cache, "Sequential - %s", ocf_engine_is_sequential(req)
			? "Yes" : "No");
}

int ocf_engine_check(struct ocf_request *req)
{
	int result = 0;
	uint32_t i;
	uint64_t core_line;

	struct ocf_cache *cache = req->cache;
	ocf_core_id_t core_id = ocf_core_get_id(req->core);

	OCF_DEBUG_TRACE(req->cache);

	ocf_req_clear_info(req);

	for (i = 0, core_line = req->core_line_first;
			core_line <= req->core_line_last; core_line++, i++) {

		struct ocf_map_info *entry = &(req->map[i]);

		if (entry->status == LOOKUP_MISS) {
			continue;
		}

		if (_ocf_engine_check_map_entry(cache, entry, core_id)) {
			/* Mapping is invalid */
			entry->invalid = true;

			OCF_DEBUG_PARAM(cache, "Invalid, Cache line %u",
					entry->coll_idx);

			result = -1;
		} else {
			entry->invalid = false;

			OCF_DEBUG_PARAM(cache, "Valid, Cache line %u",
					entry->coll_idx);

			ocf_engine_update_req_info(cache, req, i);
		}
	}

	OCF_DEBUG_PARAM(cache, "Sequential - %s", ocf_engine_is_sequential(req)
			? "Yes" : "No");

	return result;
}

void ocf_map_cache_line(struct ocf_request *req,
		unsigned int idx, ocf_cache_line_t cache_line)
{
	ocf_cache_t cache = req->cache;
	ocf_core_id_t core_id = ocf_core_get_id(req->core);
	ocf_cleaning_t clean_policy_type;
	unsigned int hash_index = req->map[idx].hash;
	uint64_t core_line = req->core_line_first + idx;

	/* Add the block to the corresponding collision list */
	ocf_metadata_start_collision_shared_access(cache, cache_line);
	ocf_metadata_add_to_collision(cache, core_id, core_line, hash_index,
			cache_line);
	ocf_metadata_end_collision_shared_access(cache, cache_line);

	/* Update dirty cache-block list */
	clean_policy_type = cache->conf_meta->cleaning_policy_type;

	ENV_BUG_ON(clean_policy_type >= ocf_cleaning_max);

	if (cleaning_policy_ops[clean_policy_type].init_cache_block != NULL)
		cleaning_policy_ops[clean_policy_type].
				init_cache_block(cache, cache_line);

	req->map[idx].coll_idx = cache_line;
}


static void ocf_engine_map_cache_line(struct ocf_request *req,
		unsigned int idx)
{
	struct ocf_cache *cache = req->cache;
	ocf_cache_line_t cache_line;

	if (!ocf_freelist_get_cache_line(cache->freelist, &cache_line)) {
		req->info.mapping_error = 1;
		return;
	}

	ocf_metadata_add_to_partition(cache, req->part_id, cache_line);

	ocf_map_cache_line(req, idx, cache_line);

	/* Update LRU:: Move this node to head of lru list. */
	ocf_eviction_init_cache_line(cache, cache_line);
	ocf_eviction_set_hot_cache_line(cache, cache_line);
}

static void ocf_engine_map_hndl_error(struct ocf_cache *cache,
		struct ocf_request *req)
{
	uint32_t i;
	struct ocf_map_info *entry;

	for (i = 0; i < req->core_line_count; i++) {
		entry = &(req->map[i]);

		switch (entry->status) {
		case LOOKUP_HIT:
		case LOOKUP_MISS:
			break;

		case LOOKUP_INSERTED:
		case LOOKUP_REMAPPED:
			OCF_DEBUG_RQ(req, "Canceling cache line %u",
					entry->coll_idx);

			entry->status = LOOKUP_MISS;

			ocf_metadata_start_collision_shared_access(cache,
					entry->coll_idx);

			set_cache_line_invalid_no_flush(cache, 0,
					ocf_line_end_sector(cache),
					entry->coll_idx);

			ocf_metadata_end_collision_shared_access(cache,
					entry->coll_idx);

			break;

		default:
			ENV_BUG();
			break;
		}
	}
}

static void ocf_engine_map(struct ocf_request *req)
{
	struct ocf_cache *cache = req->cache;
	uint32_t i;
	struct ocf_map_info *entry;
	uint64_t core_line;
	ocf_core_id_t core_id = ocf_core_get_id(req->core);

	if (!ocf_engine_unmapped_count(req))
		return;

	if (ocf_engine_unmapped_count(req) >
			ocf_freelist_num_free(cache->freelist)) {
		ocf_req_set_mapping_error(req);
		return;
	}

	ocf_req_clear_info(req);

	OCF_DEBUG_TRACE(req->cache);

	for (i = 0, core_line = req->core_line_first;
			core_line <= req->core_line_last; core_line++, i++) {
		entry = &(req->map[i]);

		ocf_engine_lookup_map_entry(cache, entry, core_id, core_line);

		if (entry->status != LOOKUP_HIT) {
			ocf_engine_map_cache_line(req, i);

			if (ocf_req_test_mapping_error(req)) {
				/*
				 * Eviction error (mapping error), need to
				 * clean, return and do pass through
				 */
				OCF_DEBUG_RQ(req, "Eviction ERROR when mapping");
				ocf_engine_map_hndl_error(cache, req);
				break;
			}

			entry->status = LOOKUP_INSERTED;
		}

		OCF_DEBUG_PARAM(req->cache,
			"%s, cache line %u, core line = %llu",
			entry->status == LOOKUP_HIT ? "Hit" : "Map",
			entry->coll_idx, entry->core_line);

		ocf_engine_update_req_info(cache, req, i);

	}

	if (!ocf_req_test_mapping_error(req)) {
		/* request has been inserted into cache - purge it from promotion
		 * policy */
		ocf_promotion_req_purge(cache->promotion_policy, req);
	}

	OCF_DEBUG_PARAM(req->cache, "Sequential - %s",
			ocf_engine_is_sequential(req) ? "Yes" : "No");
}

static void _ocf_engine_clean_end(void *private_data, int error)
{
	struct ocf_request *req = private_data;

	if (error) {
		OCF_DEBUG_RQ(req, "Cleaning ERROR");
		req->error |= error;

		/* End request and do not processing */
		ocf_req_unlock(req->cache->device->concurrency.cache_line,
				req);

		/* Complete request */
		req->complete(req, error);

		/* Release OCF request */
		ocf_req_put(req);
	} else {
		req->info.dirty_any = 0;
		req->info.dirty_all = 0;
		ocf_engine_push_req_front(req, true);
	}
}

static int _lock_clines(struct ocf_request *req)
{
	struct ocf_cache_line_concurrency *c =
			req->cache->device->concurrency.cache_line;
	enum ocf_engine_lock_type lock_type =
		req->engine_cbs->get_lock_type(req);

	switch (lock_type) {
	case ocf_engine_lock_write:
		return ocf_req_async_lock_wr(c, req, req->engine_cbs->resume);
	case ocf_engine_lock_read:
		return ocf_req_async_lock_rd(c, req, req->engine_cbs->resume);
	default:
		return OCF_LOCK_ACQUIRED;
	}
}

static inline int ocf_prepare_clines_miss(struct ocf_request *req)
{
	int lock_status = -OCF_ERR_NO_LOCK;
	struct ocf_metadata_lock *metadata_lock = &req->cache->metadata.lock;

	/* requests to disabled partitions go in pass-through */
	if (!ocf_part_is_enabled(&req->cache->user_parts[req->part_id])) {
		ocf_req_set_mapping_error(req);
		ocf_hb_req_prot_unlock_rd(req);
		return lock_status;
	}

	if (!ocf_part_has_space(req)) {
		ocf_hb_req_prot_unlock_rd(req);
		goto eviction;
	}

	/* Mapping must be performed holding (at least) hash-bucket write lock */
	ocf_hb_req_prot_lock_upgrade(req);

	ocf_engine_map(req);

	if (!ocf_req_test_mapping_error(req)) {
		lock_status = _lock_clines(req);
		if (lock_status < 0) {
			/* Mapping succeeded, but we failed to acquire cacheline lock.
			 * Don't try to evict, just return error to caller */
			ocf_req_set_mapping_error(req);
		}
		ocf_hb_req_prot_unlock_wr(req);
		return lock_status;
	}

	ocf_hb_req_prot_unlock_wr(req);

eviction:
	ocf_metadata_start_exclusive_access(metadata_lock);

	/* repeat traversation to pick up latest metadata status */
	ocf_engine_traverse(req);

	if (!ocf_part_has_space(req))
		ocf_req_set_part_evict(req);
	else
		ocf_req_clear_part_evict(req);

	if (space_managment_evict_do(req->cache, req,
			ocf_engine_unmapped_count(req)) == LOOKUP_MISS) {
		ocf_req_set_mapping_error(req);
		goto unlock;
	}

	ocf_engine_map(req);
	if (ocf_req_test_mapping_error(req))
		goto unlock;

	lock_status = _lock_clines(req);
	if (lock_status < 0)
		ocf_req_set_mapping_error(req);

unlock:
	ocf_metadata_end_exclusive_access(metadata_lock);

	return lock_status;
}

int ocf_engine_prepare_clines(struct ocf_request *req)
{
	bool mapped;
	bool promote = true;
	int lock = -OCF_ERR_NO_LOCK;

	/* Calculate hashes for hash-bucket locking */
	ocf_req_hash(req);

	/* Read-lock hash buckets associated with request target core & LBAs
	 * (core lines) to assure that cache mapping for these core lines does
	 * not change during traversation */
	ocf_hb_req_prot_lock_rd(req);

	/* Traverse to check if request is mapped fully */
	ocf_engine_traverse(req);

	mapped = ocf_engine_is_mapped(req);
	if (mapped) {
		lock = _lock_clines(req);
		ocf_hb_req_prot_unlock_rd(req);
		return lock;
	}

	/* check if request should promote cachelines */
	promote = ocf_promotion_req_should_promote(
			req->cache->promotion_policy, req);
	if (!promote) {
		ocf_req_set_mapping_error(req);
		ocf_hb_req_prot_unlock_rd(req);
		return lock;
	}

	return ocf_prepare_clines_miss(req);
}

static int _ocf_engine_clean_getter(struct ocf_cache *cache,
		void *getter_context, uint32_t item, ocf_cache_line_t *line)
{
	struct ocf_cleaner_attribs *attribs = getter_context;
	struct ocf_request *req = attribs->cmpl_context;

	for (; attribs->getter_item < req->core_line_count;
			attribs->getter_item++) {

		struct ocf_map_info *entry = &req->map[attribs->getter_item];

		if (entry->status != LOOKUP_HIT)
			continue;

		if (!metadata_test_dirty(cache, entry->coll_idx))
			continue;

		/* Line to be cleaned found, go to next item and return */
		*line = entry->coll_idx;
		attribs->getter_item++;
		return 0;
	}

	return -1;
}

void ocf_engine_clean(struct ocf_request *req)
{
	/* Initialize attributes for cleaner */
	struct ocf_cleaner_attribs attribs = {
			.lock_cacheline = false,

			.cmpl_context = req,
			.cmpl_fn = _ocf_engine_clean_end,

			.getter = _ocf_engine_clean_getter,
			.getter_context = &attribs,
			.getter_item = 0,

			.count = req->info.dirty_any,
			.io_queue = req->io_queue
	};

	/* Start cleaning */
	ocf_cleaner_fire(req->cache, &attribs);
}

void ocf_engine_update_block_stats(struct ocf_request *req)
{
	ocf_core_stats_vol_block_update(req->core, req->part_id, req->rw,
			req->byte_length);
}

void ocf_engine_update_request_stats(struct ocf_request *req)
{
	ocf_core_stats_request_update(req->core, req->part_id, req->rw,
			req->info.hit_no, req->core_line_count);
}

void ocf_engine_push_req_back(struct ocf_request *req, bool allow_sync)
{
	ocf_cache_t cache = req->cache;
	ocf_queue_t q = NULL;
	unsigned long lock_flags = 0;

	INIT_LIST_HEAD(&req->list);

	ENV_BUG_ON(!req->io_queue);
	q = req->io_queue;

	if (!req->info.internal) {
		env_atomic_set(&cache->last_access_ms,
				env_ticks_to_msecs(env_get_tick_count()));
	}

	env_spinlock_lock_irqsave(&q->io_list_lock, lock_flags);

	list_add_tail(&req->list, &q->io_list);
	env_atomic_inc(&q->io_no);

	env_spinlock_unlock_irqrestore(&q->io_list_lock, lock_flags);

	/* NOTE: do not dereference @req past this line, it might
	 * be picked up by concurrent io thread and deallocated
	 * at this point */

	ocf_queue_kick(q, allow_sync);
}

void ocf_engine_push_req_front(struct ocf_request *req, bool allow_sync)
{
	ocf_cache_t cache = req->cache;
	ocf_queue_t q = NULL;
	unsigned long lock_flags = 0;

	ENV_BUG_ON(!req->io_queue);
	INIT_LIST_HEAD(&req->list);

	q = req->io_queue;

	if (!req->info.internal) {
		env_atomic_set(&cache->last_access_ms,
				env_ticks_to_msecs(env_get_tick_count()));
	}

	env_spinlock_lock_irqsave(&q->io_list_lock, lock_flags);

	list_add(&req->list, &q->io_list);
	env_atomic_inc(&q->io_no);

	env_spinlock_unlock_irqrestore(&q->io_list_lock, lock_flags);

	/* NOTE: do not dereference @req past this line, it might
	 * be picked up by concurrent io thread and deallocated
	 * at this point */

	ocf_queue_kick(q, allow_sync);
}

void ocf_engine_push_req_front_if(struct ocf_request *req,
		const struct ocf_io_if *io_if,
		bool allow_sync)
{
	req->error = 0; /* Please explain why!!! */
	req->io_if = io_if;
	ocf_engine_push_req_front(req, allow_sync);
}

void inc_fallback_pt_error_counter(ocf_cache_t cache)
{
	ENV_BUG_ON(env_atomic_read(&cache->fallback_pt_error_counter) < 0);

	if (cache->fallback_pt_error_threshold == OCF_CACHE_FALLBACK_PT_INACTIVE)
		return;

	if (env_atomic_inc_return(&cache->fallback_pt_error_counter) ==
			cache->fallback_pt_error_threshold) {
		ocf_cache_log(cache, log_info, "Error threshold reached. "
				"Fallback Pass Through activated\n");
	}
}

static int _ocf_engine_refresh(struct ocf_request *req)
{
	int result;

	/* Check under metadata RD lock */
	ocf_hb_req_prot_lock_rd(req);

	result = ocf_engine_check(req);

	ocf_hb_req_prot_unlock_rd(req);

	if (result == 0) {

		/* Refresh successful, can process with original IO interface */
		req->io_if = req->priv;

		req->priv = NULL;

		if (req->rw == OCF_READ)
			req->io_if->read(req);
		else if (req->rw == OCF_WRITE)
			req->io_if->write(req);
		else
			ENV_BUG();
	} else {
		ENV_WARN(true, "Inconsistent request");
		req->error = -OCF_ERR_INVAL;

		/* Complete request */
		req->complete(req, req->error);

		/* Release WRITE lock of request */
		ocf_req_unlock(req->cache->device->concurrency.cache_line,
			req);

		/* Release OCF request */
		ocf_req_put(req);
	}

	return 0;
}

static const struct ocf_io_if _io_if_refresh = {
	.read = _ocf_engine_refresh,
	.write = _ocf_engine_refresh,
};

void ocf_engine_on_resume(struct ocf_request *req)
{
	ENV_BUG_ON(req->priv);
	OCF_CHECK_NULL(req->io_if);

	/* Exchange IO interface */
	req->priv = (void *)req->io_if;

	OCF_DEBUG_RQ(req, "On resume");

	ocf_engine_push_req_front_if(req, &_io_if_refresh, false);
}
