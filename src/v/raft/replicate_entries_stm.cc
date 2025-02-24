// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "raft/replicate_entries_stm.h"

#include "base/likely.h"
#include "base/outcome.h"
#include "features/feature_table.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "model/record.h"
#include "model/record_batch_reader.h"
#include "outcome_future_utils.h"
#include "raft/consensus.h"
#include "raft/consensus_utils.h"
#include "raft/errc.h"
#include "raft/group_configuration.h"
#include "raft/logger.h"
#include "raft/raftgen_service.h"
#include "raft/types.h"
#include "rpc/types.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/util/defer.hh>

#include <chrono>
#include <memory>

namespace raft {
using namespace std::chrono_literals;

ss::future<model::record_batch_reader> replicate_entries_stm::share_batches() {
    // one extra copy is needed for retries
    auto u = co_await _share_mutex.get_units();

    auto readers = co_await details::foreign_share_n(
      std::move(_batches.value()), 2);

    // keep a copy around until the end
    _batches = std::move(readers.back());
    readers.pop_back();

    co_return std::move(readers.back());
}

ss::future<result<append_entries_reply>> replicate_entries_stm::flush_log() {
    using ret_t = result<append_entries_reply>;
    auto flush_f = ss::now();
    if (_is_flush_required) {
        flush_f = _ptr->flush_log().discard_result();
    }

    auto f = flush_f
               .then([this]() {
                   /**
                    * Replicate STM _dirty_offset is set to the dirty offset of
                    * a log after successfull self append. After flush we are
                    * certain that data to at least `_dirty_offset` were
                    * flushed. Sampling offset again right before the flush
                    * isn't necessary since it will not influence result of
                    * replication process in current `replicate_entries_stm`
                    * instance.
                    */
                   auto new_committed_offset = _dirty_offset;
                   append_entries_reply reply;
                   reply.node_id = _ptr->_self;
                   reply.target_node_id = _ptr->_self;
                   reply.group = _ptr->group();
                   reply.term = _ptr->term();
                   // we just flushed offsets are the same
                   reply.last_dirty_log_index = new_committed_offset;
                   reply.last_flushed_log_index = new_committed_offset;
                   reply.result = reply_result::success;
                   return ret_t(reply);
               })
               .handle_exception(
                 []([[maybe_unused]] const std::exception_ptr& ex) {
                     return ret_t(errc::leader_flush_failed);
                 });
    _dispatch_sem.signal();
    return f;
}

clock_type::time_point replicate_entries_stm::append_entries_timeout() {
    return raft::clock_type::now() + _ptr->_replicate_append_timeout;
}

ss::future<result<append_entries_reply>>
replicate_entries_stm::send_append_entries_request(
  vnode n, model::record_batch_reader batches) {
    _ptr->update_node_append_timestamp(n);

    vlog(_ctxlog.trace, "Sending append entries request {} to {}", _meta, n);

    auto opts = rpc::client_opts(append_entries_timeout());
    opts.resource_units = ss::make_foreign<ss::lw_shared_ptr<units_t>>(_units);

    auto f = _ptr->_fstats.get_append_entries_unit(n).then_wrapped(
      [this, batches = std::move(batches), opts = std::move(opts), n](
        ss::future<ssx::semaphore_units> f) mutable {
          // we want to signal dispatch semaphore after calling append entries.
          // When dispatch semaphore is released the append_entries_stm releases
          // op_lock so next append entries request can be dispatched to the
          // follower
          auto signal_dispatch_sem = ss::defer(
            [this] { _dispatch_sem.signal(); });
          if (f.failed()) {
              f.ignore_ready_future();
              return ss::make_ready_future<result<append_entries_reply>>(
                make_error_code(errc::append_entries_dispatch_error));
          }
          auto u = f.get();

          return _ptr->_client_protocol
            .append_entries(
              n.id(),
              append_entries_request(
                _ptr->self(), n, _meta, std::move(batches), _is_flush_required),
              std::move(opts),
              _ptr->use_all_serde_append_entries())
            .then([this, target_node_id = n.id()](
                    result<append_entries_reply> reply) {
                return _ptr->validate_reply_target_node(
                  "append_entries_replicate", reply, target_node_id);
            })
            .finally([this, n, u = std::move(u)] {
                _ptr->_fstats.return_append_entries_units(n);
            });
      });

    return f
      .handle_exception([this](const std::exception_ptr& e) {
          vlog(_ctxlog.warn, "Error while replicating entries {}", e);
          return result<append_entries_reply>(
            errc::append_entries_dispatch_error);
      })
      .finally([this, n] { _hb_guards[n].unsuppress(); });
}

ss::future<> replicate_entries_stm::dispatch_one(vnode id) {
    return ss::with_gate(
             _req_bg,
             [this, id]() mutable {
                 return dispatch_single_retry(id).then(
                   [this, id](result<append_entries_reply> reply) {
                       raft::follower_req_seq seq{0};
                       if (id != _ptr->self()) {
                           auto it = _followers_seq.find(id);
                           vassert(
                             it != _followers_seq.end(),
                             "Follower request sequence is required to exists "
                             "for each follower. No follower sequence found "
                             "for {}",
                             id);
                           seq = it->second;
                       }

                       if (!reply) {
                           _ptr->get_probe().replicate_request_error();
                       }
                       _ptr->process_append_entries_reply(
                         id.id(), reply, seq, _dirty_offset);
                   });
             })
      .handle_exception_type([](const ss::gate_closed_exception&) {});
}

ss::future<result<append_entries_reply>>
replicate_entries_stm::dispatch_single_retry(vnode id) {
    if (id == _ptr->_self) {
        return flush_log();
    }
    return share_batches().then(
      [this, id](model::record_batch_reader batches) mutable {
          return send_append_entries_request(id, std::move(batches));
      });
}

ss::future<result<storage::append_result>>
replicate_entries_stm::append_to_self() {
    return share_batches()
      .then([this](model::record_batch_reader batches) mutable {
          vlog(_ctxlog.trace, "Self append entries - {}", _meta);

          _ptr->_last_write_consistency_level
            = _is_flush_required ? consistency_level::quorum_ack
                                 : consistency_level::leader_ack;
          return _ptr->disk_append(
            std::move(batches),
            _is_flush_required ? consensus::update_last_quorum_index::yes
                               : consensus::update_last_quorum_index::no);
      })
      .then([this](storage::append_result res) {
          vlog(_ctxlog.trace, "Leader append result: {}", res);
          // only update visibility upper bound if all quorum
          // replicated entries are committed already
          if (_ptr->_commit_index >= _ptr->_last_quorum_replicated_index) {
              // for relaxed consistency mode update visibility
              // upper bound with last offset appended to the log
              _ptr->_visibility_upper_bound_index = std::max(
                _ptr->_visibility_upper_bound_index, res.last_offset);
              _ptr->maybe_update_majority_replicated_index();
          }
          return result<storage::append_result>(std::move(res));
      })
      .handle_exception([this](const std::exception_ptr& e) {
          vlog(
            _ctxlog.warn,
            "Error replicating entries, leader append failed - {}",
            e);
          return result<storage::append_result>(errc::leader_append_failed);
      });
}
/**
 *  We skip sending follower requests it those two cases:
 *   - follower is recovering - when follower is not fully caught up it will not
 *     accept append entries request, missing data will be replicated to
 *     follower during recovery process
 *   - we haven't received any response from the follower for replicate append
 *     timeout duration - follower is probably down, we will not be able to
 *     send the request to the follower and it will require recovery. This
 *     prevents pending follower request queue build up and relieve memory
 *     pressure. Follower will still receive heartbeats, as we skip sending
 *     append entries request, after recovery follower will start receiving
 *     requests.
 */
inline bool replicate_entries_stm::should_skip_follower_request(vnode id) {
    if (auto it = _ptr->_fstats.find(id); it != _ptr->_fstats.end()) {
        const follower_index_metadata& f_meta = it->second;

        auto seq_it = _followers_seq.find(id);
        vassert(
          seq_it != _followers_seq.end(),
          "No follower sequence found for {}",
          id);
        if (
          !f_meta.is_learner
          && follower_index_metadata::is_first_request(seq_it->second)) {
            // If this is the first request (probably, replicating the
            // configuration after a leadership change), we don't have enough
            // info to make a decision on whether to skip. Send the request to a
            // voter regardless, as it is likely to be in-sync.
            return false;
        }

        const auto timeout = clock_type::now()
                             - _ptr->_replicate_append_timeout;
        if (f_meta.last_received_reply_timestamp < timeout) {
            vlog(
              _ctxlog.trace,
              "Skipping sending append request to {} - didn't receive "
              "follower heartbeat",
              id);
            return true;
        }
        if (f_meta.expected_log_end_offset != _meta.prev_log_index) {
            vlog(
              _ctxlog.trace,
              "Skipping sending append request to {} - expected follower log "
              "end offset: {}, request expected last offset: {}",
              id,
              f_meta.expected_log_end_offset,
              _meta.prev_log_index);
            return true;
        }
        return false;
    }

    return false;
}

ss::future<result<replicate_result>> replicate_entries_stm::apply(units_t u) {
    // first append lo leader log, no flushing
    auto cfg = _ptr->config();
    cfg.for_each_broker_id([this](const vnode& rni) {
        // suppress follower heartbeat, before appending to self log
        if (rni != _ptr->_self) {
            _hb_guards.emplace(rni, _ptr->suppress_heartbeats(rni));
        }
    });
    _units = ss::make_lw_shared<units_t>(std::move(u));
    _append_result = co_await append_to_self();

    if (!_append_result) {
        co_return build_replicate_result();
    }
    _dirty_offset = _append_result->value().last_offset;
    // store committed offset to check if it advanced
    _initial_committed_offset = _ptr->committed_offset();
    // dispatch requests to followers & leader flush
    cfg.for_each_broker_id([this](const vnode& rni) {
        // We are not dispatching request to followers that are
        // recovering
        if (should_skip_follower_request(rni)) {
            _hb_guards[rni].unsuppress();
            return;
        }
        if (rni != _ptr->self()) {
            auto it = _ptr->_fstats.find(rni);
            if (it != _ptr->_fstats.end()) {
                it->second.expected_log_end_offset = _dirty_offset;
                it->second.last_sent_protocol_meta = _meta;
            }
        }
        ++_requests_count;
        (void)dispatch_one(rni); // background
    });

    // wait for the requests to be dispatched in background and then release
    // units
    ssx::spawn_with_gate(_req_bg, [this]() {
        // Wait until all RPCs will be dispatched
        return _dispatch_sem.wait(_requests_count).then([this] {
            // release memory reservations, and destroy data
            _batches.reset();
            _units.release();
        });
    });

    co_return build_replicate_result();
}

result<replicate_result> replicate_entries_stm::build_replicate_result() const {
    vassert(
      _append_result,
      "Leader append result must be present before returning any result to "
      "caller");

    if (_append_result->has_error()) {
        return _append_result->error();
    }

    return replicate_result{.last_offset = _append_result->value().last_offset};
}

ss::future<result<replicate_result>>
replicate_entries_stm::wait_for_majority() {
    if (!_append_result) {
        co_return build_replicate_result();
    }
    // this is happening outside of _opsem
    // store offset and term of an appended entry
    auto appended_offset = _append_result->value().last_offset;
    auto appended_term = _append_result->value().last_term;
    /**
     * we have to finish replication when committed offset is greater or
     * equal to the appended offset or when term have changed after
     * commit_index update, if that happend it means that entry might
     * have been either commited or truncated
     */
    auto stop_cond = [this, appended_offset, appended_term] {
        const auto current_committed_offset = _ptr->committed_offset();
        const auto committed = current_committed_offset >= appended_offset;
        const auto truncated = _ptr->term() > appended_term
                               && current_committed_offset
                                    > _initial_committed_offset
                               && _ptr->_log->get_term(appended_offset)
                                    != appended_term;

        return committed || truncated;
    };
    try {
        co_await _ptr->_commit_index_updated.wait(stop_cond);
        co_return process_result(appended_offset, appended_term);

    } catch (const ss::broken_condition_variable&) {
        vlog(
          _ctxlog.debug,
          "Replication of entries with last offset: {} aborted - "
          "shutting down",
          _dirty_offset);
        co_return result<replicate_result>(
          make_error_code(errc::shutting_down));
    }
}

result<replicate_result> replicate_entries_stm::process_result(
  model::offset appended_offset, model::term_id appended_term) {
    using ret_t = result<replicate_result>;
    vlog(
      _ctxlog.trace,
      "Replication result [offset: {}, term: {}, commit_idx: "
      "{}, "
      "current_term: {}]",
      appended_offset,
      appended_term,
      _ptr->committed_offset(),
      _ptr->term());

    // if term has changed we have to check if entry was
    // replicated
    if (unlikely(appended_term != _ptr->term())) {
        const auto current_term = _ptr->_log->get_term(appended_offset);
        if (current_term != appended_term) {
            vlog(
              _ctxlog.debug,
              "Replication failure: appended term of entry {} is different "
              "than expected, expected term: {}, current term: {}",
              appended_offset,
              appended_term,
              current_term);
            return ret_t(errc::replicated_entry_truncated);
        }
    }

    // better crash than allow for inconsistency
    vassert(
      appended_offset <= _ptr->_commit_index,
      "{} - Successfull replication means that committed offset passed last "
      "appended offset. Current committed offset: {}, last appended offset: "
      "{}, initial_commited_offset: {}",
      _ptr->ntp(),
      _ptr->committed_offset(),
      appended_offset,
      _initial_committed_offset);

    vlog(
      _ctxlog.trace,
      "Replication success, last offset: {}, term: {}",
      appended_offset,
      appended_term);
    return build_replicate_result();
}

ss::future<> replicate_entries_stm::wait_for_shutdown() {
    return _req_bg.close();
}

replicate_entries_stm::replicate_entries_stm(
  consensus* p,
  append_entries_request r,
  absl::flat_hash_map<vnode, follower_req_seq> seqs)
  : _ptr(p)
  , _meta(r.metadata())
  , _is_flush_required(r.is_flush_required())
  , _batches(std::move(r).release_batches())
  , _followers_seq(std::move(seqs))
  , _ctxlog(_ptr->_ctxlog) {}

replicate_entries_stm::~replicate_entries_stm() {
    vassert(
      _req_bg.get_count() <= 0 || _req_bg.is_closed(),
      "Must call replicate_entries_stm::wait(). is_gate_closed:{}",
      _req_bg.is_closed());
}

} // namespace raft
