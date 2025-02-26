/*
 * Copyright (c) 2001, 2020 Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc_implementation/g1/concurrentMark.hpp"
#include "gc_implementation/g1/concurrentG1Refine.hpp"
#include "gc_implementation/g1/concurrentG1RefineThread.hpp"
#include "gc_implementation/g1/g1BlockOffsetTable.inline.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1CollectorPolicy.hpp"
#include "gc_implementation/g1/g1HotCardCache.hpp"
#include "gc_implementation/g1/g1GCPhaseTimes.hpp"
#include "gc_implementation/g1/g1OopClosures.inline.hpp"
#include "gc_implementation/g1/g1RemSet.inline.hpp"
#include "gc_implementation/g1/heapRegionManager.inline.hpp"
#include "gc_implementation/g1/heapRegionRemSet.hpp"
#include "memory/iterator.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/intHisto.hpp"

PRAGMA_FORMAT_MUTE_WARNINGS_FOR_GCC

#define CARD_REPEAT_HISTO 0

#if CARD_REPEAT_HISTO
static size_t ct_freq_sz;
static jbyte* ct_freq = NULL;

void init_ct_freq_table(size_t heap_sz_bytes) {
  if (ct_freq == NULL) {
    ct_freq_sz = heap_sz_bytes/CardTableModRefBS::card_size;
    ct_freq = new jbyte[ct_freq_sz];
    for (size_t j = 0; j < ct_freq_sz; j++) ct_freq[j] = 0;
  }
}

void ct_freq_note_card(size_t index) {
  assert(0 <= index && index < ct_freq_sz, "Bounds error.");
  if (ct_freq[index] < 100) { ct_freq[index]++; }
}

static IntHistogram card_repeat_count(10, 10);

void ct_freq_update_histo_and_reset() {
  for (size_t j = 0; j < ct_freq_sz; j++) {
    card_repeat_count.add_entry(ct_freq[j]);
    ct_freq[j] = 0;
  }

}
#endif

G1RemSet::G1RemSet(G1CollectedHeap* g1, CardTableModRefBS* ct_bs)
  : _g1(g1), _conc_refine_cards(0),
    _ct_bs(ct_bs), _g1p(_g1->g1_policy()),
    _cg1r(g1->concurrent_g1_refine()),
    _cset_rs_update_cl(NULL),
    _cards_scanned(NULL), _total_cards_scanned(0),
    _prev_period_summary()
{
  guarantee(n_workers() > 0, "There should be some workers");
  _cset_rs_update_cl = NEW_C_HEAP_ARRAY(G1ParPushHeapRSClosure*, n_workers(), mtGC);
  for (uint i = 0; i < n_workers(); i++) {
    _cset_rs_update_cl[i] = NULL;
  }
  if (G1SummarizeRSetStats) {
    _prev_period_summary.initialize(this);
  }
}

G1RemSet::~G1RemSet() {
  for (uint i = 0; i < n_workers(); i++) {
    assert(_cset_rs_update_cl[i] == NULL, "it should be");
  }
  FREE_C_HEAP_ARRAY(G1ParPushHeapRSClosure*, _cset_rs_update_cl, mtGC);
}

void CountNonCleanMemRegionClosure::do_MemRegion(MemRegion mr) {
  if (_g1->is_in_g1_reserved(mr.start())) {
    _n += (int) ((mr.byte_size() / CardTableModRefBS::card_size));
    if (_start_first == NULL) _start_first = mr.start();
  }
}

class ScanRSClosure : public HeapRegionClosure {
  size_t _cards_done, _cards;
  G1CollectedHeap* _g1h;

  G1ParPushHeapRSClosure* _oc;
  CodeBlobClosure* _code_root_cl;

  G1BlockOffsetSharedArray* _bot_shared;
  G1SATBCardTableModRefBS *_ct_bs;

  G1ParScanThreadState* _par_scan_state;

  double _strong_code_root_scan_time_sec;
  uint   _worker_i;
  int    _block_size;
  bool   _try_claimed;

public:
  ScanRSClosure(G1ParPushHeapRSClosure* oc,
                CodeBlobClosure* code_root_cl,
                uint worker_i,
                G1ParScanThreadState* pss) :
    _oc(oc),
    _code_root_cl(code_root_cl),
    _strong_code_root_scan_time_sec(0.0),
    _cards(0),
    _cards_done(0),
    _worker_i(worker_i),
    _try_claimed(false),
    _par_scan_state(pss)
  {
    _g1h = G1CollectedHeap::heap();
    _bot_shared = _g1h->bot_shared();
    _ct_bs = _g1h->g1_barrier_set();
    _block_size = MAX2<int>(G1RSetScanBlockSize, 1);
  }

  void set_try_claimed() { _try_claimed = true; }

  void scanCard(size_t index, HeapRegion *r) {
    // Stack allocate the DirtyCardToOopClosure instance
    HeapRegionDCTOC cl(_g1h, r, _oc,
                       CardTableModRefBS::Precise);

    // Set the "from" region in the closure.
    _oc->set_region(r);
    MemRegion card_region(_bot_shared->address_for_index(index), G1BlockOffsetSharedArray::N_words);
    MemRegion pre_gc_allocated(r->bottom(), r->scan_top());
    MemRegion mr = pre_gc_allocated.intersection(card_region);
    if (!mr.is_empty() && !_ct_bs->is_card_claimed(index)) {
      // We make the card as "claimed" lazily (so races are possible
      // but they're benign), which reduces the number of duplicate
      // scans (the rsets of the regions in the cset can intersect).
      _ct_bs->set_card_claimed(index);
      _cards_done++;
      cl.do_MemRegion(mr);
    }
  }

  void printCard(HeapRegion* card_region, size_t card_index,
                 HeapWord* card_start) {
    gclog_or_tty->print_cr("T " UINT32_FORMAT " Region [" PTR_FORMAT ", " PTR_FORMAT ") "
                           "RS names card %p: "
                           "[" PTR_FORMAT ", " PTR_FORMAT ")",
                           _worker_i,
                           card_region->bottom(), card_region->end(),
                           card_index,
                           card_start, card_start + G1BlockOffsetSharedArray::N_words);
  }

  void scan_strong_code_roots(HeapRegion* r) {
    double scan_start = os::elapsedTime();
    r->strong_code_roots_do(_code_root_cl);
    _par_scan_state->trim_queue_partially();
    _strong_code_root_scan_time_sec += (os::elapsedTime() - scan_start);
  }

  bool doHeapRegion(HeapRegion* r) {
    assert(r->in_collection_set(), "should only be called on elements of CS.");
    HeapRegionRemSet* hrrs = r->rem_set();
    if (hrrs->iter_is_complete()) return false; // All done.
    if (!_try_claimed && !hrrs->claim_iter()) return false;
    // If we ever free the collection set concurrently, we should also
    // clear the card table concurrently therefore we won't need to
    // add regions of the collection set to the dirty cards region.
    _g1h->push_dirty_cards_region(r);
    // If we didn't return above, then
    //   _try_claimed || r->claim_iter()
    // is true: either we're supposed to work on claimed-but-not-complete
    // regions, or we successfully claimed the region.

    HeapRegionRemSetIterator iter(hrrs);
    size_t card_index;

    // We claim cards in block so as to recude the contention. The block size is determined by
    // the G1RSetScanBlockSize parameter.
    size_t jump_to_card = hrrs->iter_claimed_next(_block_size);
    for (size_t current_card = 0; iter.has_next(card_index); current_card++) {
      if (current_card >= jump_to_card + _block_size) {
        jump_to_card = hrrs->iter_claimed_next(_block_size);
      }
      if (current_card < jump_to_card) continue;
      HeapWord* card_start = _g1h->bot_shared()->address_for_index(card_index);
#if 0
      gclog_or_tty->print("Rem set iteration yielded card [" PTR_FORMAT ", " PTR_FORMAT ").\n",
                          card_start, card_start + CardTableModRefBS::card_size_in_words);
#endif

      HeapRegion* card_region = _g1h->heap_region_containing(card_start);
      _cards++;

      if (!card_region->is_on_dirty_cards_region_list()) {
        _g1h->push_dirty_cards_region(card_region);
      }

      // If the card is dirty, then we will scan it during updateRS.
      if (!card_region->in_collection_set() &&
          !_ct_bs->is_card_dirty(card_index)) {
        scanCard(card_index, card_region);
      }
    }
    if (!_try_claimed) {
      // Scan the strong code root list attached to the current region
      scan_strong_code_roots(r);

      hrrs->set_iter_complete();
    }
    return false;
  }

  double strong_code_root_scan_time_sec() {
    return _strong_code_root_scan_time_sec;
  }

  size_t cards_done() { return _cards_done;}
  size_t cards_looked_up() { return _cards;}
};

void G1RemSet::scanRS(G1ParPushHeapRSClosure* oc,
                      CodeBlobClosure* code_root_cl,
                      uint worker_i) {
  double rs_time_start = os::elapsedTime();

  ScanRSClosure scanRScl(oc, code_root_cl, worker_i, oc->par_scan_state());

  _g1->collection_set_iterate_from(&scanRScl, worker_i);
  scanRScl.set_try_claimed();
  _g1->collection_set_iterate_from(&scanRScl, worker_i);

  double scan_rs_time_sec = (os::elapsedTime() - rs_time_start)
                            - scanRScl.strong_code_root_scan_time_sec();

  assert(_cards_scanned != NULL, "invariant");
  _cards_scanned[worker_i] = scanRScl.cards_done();

  _g1p->phase_times()->record_time_secs(G1GCPhaseTimes::ScanRS, worker_i, scan_rs_time_sec);
  _g1p->phase_times()->record_time_secs(G1GCPhaseTimes::CodeRoots, worker_i, scanRScl.strong_code_root_scan_time_sec());
}

// Closure used for updating RSets and recording references that
// point into the collection set. Only called during an
// evacuation pause.

class RefineRecordRefsIntoCSCardTableEntryClosure: public CardTableEntryClosure {
  G1RemSet* _g1rs;
  DirtyCardQueue* _into_cset_dcq;
  G1ParScanThreadState* _par_scan_state;
public:
  RefineRecordRefsIntoCSCardTableEntryClosure(G1CollectedHeap* g1h,
                                              DirtyCardQueue* into_cset_dcq,
                                              G1ParScanThreadState* pss) :
    _g1rs(g1h->g1_rem_set()), _into_cset_dcq(into_cset_dcq), _par_scan_state(pss)
  {}
  bool do_card_ptr(jbyte* card_ptr, uint worker_i) {
    // The only time we care about recording cards that
    // contain references that point into the collection set
    // is during RSet updating within an evacuation pause.
    // In this case worker_i should be the id of a GC worker thread.
    assert(SafepointSynchronize::is_at_safepoint(), "not during an evacuation pause");
    assert(worker_i < (ParallelGCThreads == 0 ? 1 : ParallelGCThreads), "should be a GC worker");

    if (_g1rs->refine_card_during_gc(card_ptr, worker_i)) {
      _par_scan_state->trim_queue_partially();
      // 'card_ptr' contains references that point into the collection
      // set. We need to record the card in the DCQS
      // (G1CollectedHeap::into_cset_dirty_card_queue_set())
      // that's used for that purpose.
      //
      // Enqueue the card
      _into_cset_dcq->enqueue(card_ptr);
    }
    return true;
  }
};

void G1RemSet::updateRS(DirtyCardQueue* into_cset_dcq, uint worker_i, G1ParScanThreadState* pss) {
  G1GCParPhaseTimesTracker x(_g1p->phase_times(), G1GCPhaseTimes::UpdateRS, worker_i);
  // Apply the given closure to all remaining log entries.
  RefineRecordRefsIntoCSCardTableEntryClosure into_cset_update_rs_cl(_g1, into_cset_dcq, pss);

  _g1->iterate_dirty_card_closure(&into_cset_update_rs_cl, into_cset_dcq, false, worker_i);
}

void G1RemSet::cleanupHRRS() {
  HeapRegionRemSet::cleanup();
}

void G1RemSet::oops_into_collection_set_do(G1ParPushHeapRSClosure* oc,
                                           CodeBlobClosure* code_root_cl,
                                           uint worker_i) {
#if CARD_REPEAT_HISTO
  ct_freq_update_histo_and_reset();
#endif

  // We cache the value of 'oc' closure into the appropriate slot in the
  // _cset_rs_update_cl for this worker
  assert(worker_i < n_workers(), "sanity");
  _cset_rs_update_cl[worker_i] = oc;

  // A DirtyCardQueue that is used to hold cards containing references
  // that point into the collection set. This DCQ is associated with a
  // special DirtyCardQueueSet (see g1CollectedHeap.hpp).  Under normal
  // circumstances (i.e. the pause successfully completes), these cards
  // are just discarded (there's no need to update the RSets of regions
  // that were in the collection set - after the pause these regions
  // are wholly 'free' of live objects. In the event of an evacuation
  // failure the cards/buffers in this queue set are passed to the
  // DirtyCardQueueSet that is used to manage RSet updates
  DirtyCardQueue into_cset_dcq(&_g1->into_cset_dirty_card_queue_set());

  assert((ParallelGCThreads > 0) || worker_i == 0, "invariant");

  updateRS(&into_cset_dcq, worker_i, oc->par_scan_state());
  scanRS(oc, code_root_cl, worker_i);

  // We now clear the cached values of _cset_rs_update_cl for this worker
  _cset_rs_update_cl[worker_i] = NULL;
}

void G1RemSet::prepare_for_oops_into_collection_set_do() {
  _g1->set_refine_cte_cl_concurrency(false);
  DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
  dcqs.concatenate_logs();

  guarantee( _cards_scanned == NULL, "invariant" );
  _cards_scanned = NEW_C_HEAP_ARRAY(size_t, n_workers(), mtGC);
  for (uint i = 0; i < n_workers(); ++i) {
    _cards_scanned[i] = 0;
  }
  _total_cards_scanned = 0;
}

void G1RemSet::cleanup_after_oops_into_collection_set_do() {
  guarantee( _cards_scanned != NULL, "invariant" );
  _total_cards_scanned = 0;
  for (uint i = 0; i < n_workers(); ++i) {
    _total_cards_scanned += _cards_scanned[i];
  }
  FREE_C_HEAP_ARRAY(size_t, _cards_scanned, mtGC);
  _cards_scanned = NULL;
  // Cleanup after copy
  _g1->set_refine_cte_cl_concurrency(true);
  // Set all cards back to clean.
  _g1->cleanUpCardTable();

  DirtyCardQueueSet& into_cset_dcqs = _g1->into_cset_dirty_card_queue_set();
  int into_cset_n_buffers = into_cset_dcqs.completed_buffers_num();

  if (_g1->evacuation_failed()) {
    double restore_remembered_set_start = os::elapsedTime();

    // Restore remembered sets for the regions pointing into the collection set.
    // We just need to transfer the completed buffers from the DirtyCardQueueSet
    // used to hold cards that contain references that point into the collection set
    // to the DCQS used to hold the deferred RS updates.
    _g1->dirty_card_queue_set().merge_bufferlists(&into_cset_dcqs);
    _g1->g1_policy()->phase_times()->record_evac_fail_restore_remsets((os::elapsedTime() - restore_remembered_set_start) * 1000.0);
  }

  // Free any completed buffers in the DirtyCardQueueSet used to hold cards
  // which contain references that point into the collection.
  _g1->into_cset_dirty_card_queue_set().clear();
  assert(_g1->into_cset_dirty_card_queue_set().completed_buffers_num() == 0,
         "all buffers should be freed");
  _g1->into_cset_dirty_card_queue_set().clear_n_completed_buffers();
}

class ScrubRSClosure: public HeapRegionClosure {
  G1CollectedHeap* _g1h;
  BitMap* _region_bm;
  BitMap* _card_bm;
  CardTableModRefBS* _ctbs;
public:
  ScrubRSClosure(BitMap* region_bm, BitMap* card_bm) :
    _g1h(G1CollectedHeap::heap()),
    _region_bm(region_bm), _card_bm(card_bm),
    _ctbs(_g1h->g1_barrier_set()) {}

  bool doHeapRegion(HeapRegion* r) {
    if (!r->continuesHumongous()) {
      r->rem_set()->scrub(_ctbs, _region_bm, _card_bm);
    }
    return false;
  }
};

void G1RemSet::scrub(BitMap* region_bm, BitMap* card_bm) {
  ScrubRSClosure scrub_cl(region_bm, card_bm);
  _g1->heap_region_iterate(&scrub_cl);
}

void G1RemSet::scrub_par(BitMap* region_bm, BitMap* card_bm,
                                uint worker_num, int claim_val) {
  ScrubRSClosure scrub_cl(region_bm, card_bm);
  _g1->heap_region_par_iterate_chunked(&scrub_cl,
                                       worker_num,
                                       n_workers(),
                                       claim_val);
}

inline void check_card_ptr(jbyte* card_ptr, CardTableModRefBS* ct_bs) {
#ifdef ASSERT
  G1CollectedHeap* g1 = G1CollectedHeap::heap();
  assert(g1->is_in_exact(ct_bs->addr_for(card_ptr)),
         err_msg("Card at " PTR_FORMAT " index " SIZE_FORMAT " representing heap at " PTR_FORMAT " (%u) must be in committed heap",
         p2i(card_ptr),
         ct_bs->index_for(ct_bs->addr_for(card_ptr)),
         p2i(ct_bs->addr_for(card_ptr)),
         g1->addr_to_region(ct_bs->addr_for(card_ptr))));
#endif
}

G1UpdateRSOrPushRefOopClosure::G1UpdateRSOrPushRefOopClosure(G1CollectedHeap* g1h,
                                                             G1ParPushHeapRSClosure* push_ref_cl,
                                                             bool record_refs_into_cset,
                                                             uint worker_i) :
  _g1(g1h),
  _from(NULL),
  _record_refs_into_cset(record_refs_into_cset),
  _has_refs_into_cset(false),
  _push_ref_cl(push_ref_cl),
  _worker_i(worker_i) { }

void G1RemSet::refine_card_concurrently(jbyte* card_ptr,
                                        uint worker_i) {
  assert(!_g1->is_gc_active(), "Only call concurrently");

  check_card_ptr(card_ptr, _ct_bs);
  // If the card is no longer dirty, nothing to do.
  if (*card_ptr != CardTableModRefBS::dirty_card_val()) {
    return;
  }

  // Construct the region representing the card.
  HeapWord* start = _ct_bs->addr_for(card_ptr);
  // And find the region containing it.
  HeapRegion* r = _g1->heap_region_containing(start);

  // This check is needed for some uncommon cases where we should
  // ignore the card.
  //
  // The region could be young.  Cards for young regions are
  // distinctly marked (set to g1_young_gen), so the post-barrier will
  // filter them out.  However, that marking is performed
  // concurrently.  A write to a young object could occur before the
  // card has been marked young, slipping past the filter.
  //
  // The card could be stale, because the region has been freed since
  // the card was recorded. In this case the region type could be
  // anything.  If (still) free or (reallocated) young, just ignore
  // it.  If (reallocated) old or humongous, the later card trimming
  // and additional checks in iteration may detect staleness.  At
  // worst, we end up processing a stale card unnecessarily.
  //
  // In the normal (non-stale) case, the synchronization between the
  // enqueueing of the card and processing it here will have ensured
  // we see the up-to-date region type here.
  if (!r->is_old_or_humongous()) {
    return;
  }

  // The result from the hot card cache insert call is either:
  //   * pointer to the current card
  //     (implying that the current card is not 'hot'),
  //   * null
  //     (meaning we had inserted the card ptr into the "hot" card cache,
  //     which had some headroom),
  //   * a pointer to a "hot" card that was evicted from the "hot" cache.
  //

  G1HotCardCache* hot_card_cache = _cg1r->hot_card_cache();
  if (hot_card_cache->use_cache()) {
    assert(!SafepointSynchronize::is_at_safepoint(), "sanity");

    const jbyte* orig_card_ptr = card_ptr;
    card_ptr = hot_card_cache->insert(card_ptr);
    if (card_ptr == NULL) {
      // There was no eviction. Nothing to do.
      return;
    } else if (card_ptr != orig_card_ptr) {
      // Original card was inserted and an old card was evicted.
      start = _ct_bs->addr_for(card_ptr);
      r = _g1->heap_region_containing(start);

      // Check whether the region formerly in the cache should be
      // ignored, as discussed earlier for the original card.  The
      // region could have been freed while in the cache.  The cset is
      // not relevant here, since we're in concurrent phase.
      if (!r->is_old_or_humongous()) {
        return;
      }
    } // Else we still have the original card.
  }

  // Trim the region designated by the card to what's been allocated
  // in the region.  The card could be stale, or the card could cover
  // (part of) an object at the end of the allocated space and extend
  // beyond the end of allocation.

  // Non-humongous objects are only allocated in the old-gen during
  // GC, so if region is old then top is stable.  Humongous object
  // allocation sets top last; if top has not yet been set, this is
  // a stale card and we'll end up with an empty intersection.  If
  // this is not a stale card, the synchronization between the
  // enqueuing of the card and processing it here will have ensured
  // we see the up-to-date top here.
  HeapWord* scan_limit = r->top();

  if (scan_limit <= start) {
    // If the trimmed region is empty, the card must be stale.
    return;
  }

  // Okay to clean and process the card now.  There are still some
  // stale card cases that may be detected by iteration and dealt with
  // as iteration failure.
  *const_cast<volatile jbyte*>(card_ptr) = CardTableModRefBS::clean_card_val();

  // This fence serves two purposes.  First, the card must be cleaned
  // before processing the contents.  Second, we can't proceed with
  // processing until after the read of top, for synchronization with
  // possibly concurrent humongous object allocation.  It's okay that
  // reading top and reading type were racy wrto each other.  We need
  // both set, in any order, to proceed.
  OrderAccess::fence();

  // Don't use addr_for(card_ptr + 1) which can ask for
  // a card beyond the heap.
  HeapWord* end = start + CardTableModRefBS::card_size_in_words;
  MemRegion dirty_region(start, MIN2(scan_limit, end));
  assert(!dirty_region.is_empty(), "sanity");

  G1ConcurrentRefineOopClosure conc_refine_cl(_g1, worker_i);

  bool card_processed = 
    r->oops_on_card_seq_iterate_careful<false>(dirty_region, &conc_refine_cl);

  // If unable to process the card then we encountered an unparsable
  // part of the heap (e.g. a partially allocated object) while
  // processing a stale card.  Despite the card being stale, redirty
  // and re-enqueue, because we've already cleaned the card.  Without
  // this we could incorrectly discard a non-stale card.
  if (!card_processed) {
    // The card might have gotten re-dirtied and re-enqueued while we
    // worked.  (In fact, it's pretty likely.)
    if (*card_ptr != CardTableModRefBS::dirty_card_val()) {
      *card_ptr = CardTableModRefBS::dirty_card_val();
      MutexLockerEx x(Shared_DirtyCardQ_lock,
                      Mutex::_no_safepoint_check_flag);
      DirtyCardQueue* sdcq =
        JavaThread::dirty_card_queue_set().shared_dirty_card_queue();
      sdcq->enqueue(card_ptr);
    }
  } else {
    _conc_refine_cards++;
  }
}

bool G1RemSet::refine_card_during_gc(jbyte* card_ptr,
                                     uint worker_i) {
  assert(_g1->is_gc_active(), "Only call during GC");

  check_card_ptr(card_ptr, _ct_bs);

  // If the card is no longer dirty, nothing to do. This covers cards that were already
  // scanned as parts of the remembered sets.
  if (*card_ptr != CardTableModRefBS::dirty_card_val()) {
    // No need to return that this card contains refs that point
    // into the collection set.
    return false;
  }

  // Construct the region representing the card.
  HeapWord* start = _ct_bs->addr_for(card_ptr);
  // And find the region containing it.
  HeapRegion* r = _g1->heap_region_containing(start);

  // This check is needed for some uncommon cases where we should
  // ignore the card.
  //
  // The region could be young.  Cards for young regions are
  // distinctly marked (set to g1_young_gen), so the post-barrier will
  // filter them out.  However, that marking is performed
  // concurrently.  A write to a young object could occur before the
  // card has been marked young, slipping past the filter.
  //
  // The card could be stale, because the region has been freed since
  // the card was recorded. In this case the region type could be
  // anything.  If (still) free or (reallocated) young, just ignore
  // it.  If (reallocated) old or humongous, the later card trimming
  // and additional checks in iteration may detect staleness.  At
  // worst, we end up processing a stale card unnecessarily.
  //
  // In the normal (non-stale) case, the synchronization between the
  // enqueueing of the card and processing it here will have ensured
  // we see the up-to-date region type here.
  if (!r->is_old_or_humongous()) {
    return false;
  }

  // While we are processing RSet buffers during the collection, we
  // actually don't want to scan any cards on the collection set,
  // since we don't want to update remebered sets with entries that
  // point into the collection set, given that live objects from the
  // collection set are about to move and such entries will be stale
  // very soon. This change also deals with a reliability issue which
  // involves scanning a card in the collection set and coming across
  // an array that was being chunked and looking malformed. Note,
  // however, that if evacuation fails, we have to scan any objects
  // that were not moved and create any missing entries.
  if (r->in_collection_set()) {
    return false;
  }

  // Trim the region designated by the card to what's been allocated
  // in the region.  The card could be stale, or the card could cover
  // (part of) an object at the end of the allocated space and extend
  // beyond the end of allocation.
  // If we're in a STW GC, then a card might be in a GC alloc region
  // and extend onto a GC LAB, which may not be parsable.  Stop such
  // at the "scan_top" of the region.
  HeapWord* scan_limit = r->scan_top();

  if (scan_limit <= start) {
    // If the trimmed region is empty, the card must be stale.
    return false;
  }

  // Okay to clean and process the card now.  There are still some
  // stale card cases that may be detected by iteration and dealt with
  // as iteration failure.
  *const_cast<volatile jbyte*>(card_ptr) = CardTableModRefBS::clean_card_val();

  // a card beyond the heap.
  HeapWord* end = start + CardTableModRefBS::card_size_in_words;
  MemRegion dirty_region(start, MIN2(scan_limit, end));
  assert(!dirty_region.is_empty(), "sanity");

#if CARD_REPEAT_HISTO
  init_ct_freq_table(_g1->max_capacity());
  ct_freq_note_card(_ct_bs->index_for(start));
#endif
  G1ParPushHeapRSClosure* oops_in_heap_closure = _cset_rs_update_cl[worker_i];
  G1UpdateRSOrPushRefOopClosure update_rs_oop_cl(_g1,
                                                 oops_in_heap_closure,
                                                 true,
                                                 worker_i);
  update_rs_oop_cl.set_from(r);

  bool card_processed =
    r->oops_on_card_seq_iterate_careful<true>(dirty_region,
                                        &update_rs_oop_cl);

  assert(card_processed, "must be");
  _conc_refine_cards++;

  return update_rs_oop_cl.has_refs_into_cset();
}

void G1RemSet::print_periodic_summary_info(const char* header) {
  G1RemSetSummary current;
  current.initialize(this);

  _prev_period_summary.subtract_from(&current);
  print_summary_info(&_prev_period_summary, header);

  _prev_period_summary.set(&current);
}

void G1RemSet::print_summary_info() {
  G1RemSetSummary current;
  current.initialize(this);

  print_summary_info(&current, " Cumulative RS summary");
}

void G1RemSet::print_summary_info(G1RemSetSummary * summary, const char * header) {
  assert(summary != NULL, "just checking");

  if (header != NULL) {
    gclog_or_tty->print_cr("%s", header);
  }

#if CARD_REPEAT_HISTO
  gclog_or_tty->print_cr("\nG1 card_repeat count histogram: ");
  gclog_or_tty->print_cr("  # of repeats --> # of cards with that number.");
  card_repeat_count.print_on(gclog_or_tty);
#endif

  summary->print_on(gclog_or_tty);
}

void G1RemSet::prepare_for_verify() {
  if (G1HRRSFlushLogBuffersOnVerify &&
      (VerifyBeforeGC || VerifyAfterGC)
      &&  (!_g1->full_collection() || G1VerifyRSetsDuringFullGC)) {
    cleanupHRRS();
    _g1->set_refine_cte_cl_concurrency(false);
    if (SafepointSynchronize::is_at_safepoint()) {
      DirtyCardQueueSet& dcqs = JavaThread::dirty_card_queue_set();
      dcqs.concatenate_logs();
    }

    G1HotCardCache* hot_card_cache = _cg1r->hot_card_cache();
    bool use_hot_card_cache = hot_card_cache->use_cache();
    hot_card_cache->set_use_cache(false);

    G1ParScanThreadState* pss = _g1->new_par_scan_state(0);
    DirtyCardQueue into_cset_dcq(&_g1->into_cset_dirty_card_queue_set());
    updateRS(&into_cset_dcq, 0, pss);
    _g1->into_cset_dirty_card_queue_set().clear();

    hot_card_cache->set_use_cache(use_hot_card_cache);
    delete pss;
    assert(JavaThread::dirty_card_queue_set().completed_buffers_num() == 0, "All should be consumed");
  }
}

class G1RebuildRemSetTask: public AbstractGangTask {
  // Aggregate the counting data that was constructed concurrently
  // with marking.  
  class G1RebuildRemSetHeapRegionClosure : public HeapRegionClosure {
    ConcurrentMark* _cm;
    G1RebuildRemSetClosure _update_cl;

    // Applies _update_cl to the references of the given object, limiting objArrays
    // to the given MemRegion. Returns the amount of words actually scanned.
    size_t scan_for_references(oop const obj, MemRegion mr) {
      size_t const obj_size = obj->size();
      // All non-objArrays and objArrays completely within the mr
      // can be scanned without passing the mr.
      if (!obj->is_objArray() || mr.contains(MemRegion((HeapWord*)obj, obj_size))) {
        obj->oop_iterate(&_update_cl);
        return obj_size;
      }
      // This path is for objArrays crossing the given MemRegion. Only scan the
      // area within the MemRegion.
      obj->oop_iterate(&_update_cl, mr);
      return mr.intersection(MemRegion((HeapWord*)obj, obj_size)).word_size();
    }

    // A humongous object is live (with respect to the scanning) either
    // a) it is marked on the bitmap as such
    // b) its TARS is larger than TAMS, i.e. has been allocated during marking.
    bool is_humongous_live(oop const humongous_obj, const CMBitMap* const bitmap, HeapWord* tams, HeapWord* tars) const {
      return bitmap->isMarked(humongous_obj) || (tars > tams);
    }

    // Iterator over the live objects within the given MemRegion.
    class LiveObjIterator : public StackObj {
      const CMBitMap* const _bitmap;
      const HeapWord* _tams;
      const MemRegion _mr;
      HeapWord* _current;

      bool is_below_tams() const {
        return _current < _tams;
      }

      bool is_live(HeapWord* obj) const {
        return !is_below_tams() || _bitmap->isMarked(obj);
      }

      HeapWord* bitmap_limit() const {
        return MIN2(const_cast<HeapWord*>(_tams), _mr.end());
      }

      void move_if_below_tams() {
        if (is_below_tams() && has_next()) {
          _current = _bitmap->getNextMarkedWordAddress(_current, bitmap_limit());
        }
      }
    public:
      LiveObjIterator(const CMBitMap* const bitmap, const HeapWord* tams, const MemRegion mr, HeapWord* first_oop_into_mr) :
          _bitmap(bitmap),
          _tams(tams),
          _mr(mr),
          _current(first_oop_into_mr) {

        assert(_current <= _mr.start(),
               err_msg("First oop " PTR_FORMAT " should extend into mr [" PTR_FORMAT ", " PTR_FORMAT ")",
               p2i(first_oop_into_mr), p2i(mr.start()), p2i(mr.end())));

        // Step to the next live object within the MemRegion if needed.
        if (is_live(_current)) {
          // Non-objArrays were scanned by the previous part of that region.
          if (_current < mr.start() && !oop(_current)->is_objArray()) {
            _current += oop(_current)->size();
            // We might have positioned _current on a non-live object. Reposition to the next
            // live one if needed.
            move_if_below_tams();
          }
        } else {
          // The object at _current can only be dead if below TAMS, so we can use the bitmap.
          // immediately.
          _current = _bitmap->getNextMarkedWordAddress(_current, bitmap_limit());
          assert(_current == _mr.end() || is_live(_current),
                 err_msg("Current " PTR_FORMAT " should be live (%s) or beyond the end of the MemRegion (" PTR_FORMAT ")",
                 p2i(_current), BOOL_TO_STR(is_live(_current)), p2i(_mr.end())));
        }
      }

      void move_to_next() {
        _current += next()->size();
        move_if_below_tams();
      }

      oop next() const {
        oop result = oop(_current);
        assert(is_live(_current),
               err_msg("Object " PTR_FORMAT " must be live TAMS " PTR_FORMAT " below %d mr " PTR_FORMAT " " PTR_FORMAT " outside %d",
               p2i(_current), p2i(_tams), _tams > _current, p2i(_mr.start()), p2i(_mr.end()), _mr.contains(result)));
        return result;
      }

      bool has_next() const {
        return _current < _mr.end();
      }
    };

    // Rebuild remembered sets in the part of the region specified by mr and hr.
    // Objects between the bottom of the region and the TAMS are checked for liveness
    // using the given bitmap. Objects between TAMS and TARS are assumed to be live.
    // Returns the number of live words between bottom and TAMS.
    size_t rebuild_rem_set_in_region(const CMBitMap* const bitmap,
                                     HeapWord* const top_at_mark_start,
                                     HeapWord* const top_at_rebuild_start,
                                     HeapRegion* hr,
                                     MemRegion mr) {
      size_t marked_words = 0;

      if (hr->isHumongous()) {
        oop const humongous_obj = oop(hr->humongous_start_region()->bottom());
        if (is_humongous_live(humongous_obj, bitmap, top_at_mark_start, top_at_rebuild_start)) {
          // We need to scan both [bottom, TAMS) and [TAMS, top_at_rebuild_start);
          // however in case of humongous objects it is sufficient to scan the encompassing
          // area (top_at_rebuild_start is always larger or equal to TAMS) as one of the
          // two areas will be zero sized. I.e. TAMS is either
          // the same as bottom or top(_at_rebuild_start). There is no way TAMS has a different
          // value: this would mean that TAMS points somewhere into the object.
          assert(hr->top() == top_at_mark_start || hr->top() == top_at_rebuild_start,
                 "More than one object in the humongous region?");
          humongous_obj->oop_iterate(&_update_cl, mr);
          return top_at_mark_start != hr->bottom() ? mr.byte_size() : 0;
        } else {
          return 0;
        }
      }

      for (LiveObjIterator it(bitmap, top_at_mark_start, mr, hr->block_start(mr.start())); it.has_next(); it.move_to_next()) {
        oop obj = it.next();
        size_t scanned_size = scan_for_references(obj, mr);
        if ((HeapWord*)obj < top_at_mark_start) {
          marked_words += scanned_size;
        }
      }
      return marked_words * HeapWordSize;
    }
  public:
    G1RebuildRemSetHeapRegionClosure(G1CollectedHeap* g1h,
                                     ConcurrentMark* cm,
                                     uint worker_id) :
      HeapRegionClosure(),
      _cm(cm),
      _update_cl(g1h, worker_id) { }

    bool doHeapRegion(HeapRegion* hr) {
      if (_cm->has_aborted()) {
        return true;
      }
      uint const region_idx = hr->hrm_index();
      DEBUG_ONLY(HeapWord* const top_at_rebuild_start_check = _cm->top_at_rebuild_start(region_idx);)
      assert(top_at_rebuild_start_check == NULL ||
             top_at_rebuild_start_check > hr->bottom(),
             err_msg("A TARS (" PTR_FORMAT ") == bottom() (" PTR_FORMAT ") indicates the old region %u is empty (%s)",
             p2i(top_at_rebuild_start_check), p2i(hr->bottom()),  region_idx, hr->get_type_str()));

      size_t total_marked_bytes = 0;
      size_t const chunk_size_in_words = G1RebuildRemSetChunkSize / HeapWordSize;

      HeapWord* const top_at_mark_start = hr->next_top_at_mark_start();

      HeapWord* cur = hr->bottom();
      while (cur < hr->end()) {
        // After every iteration (yield point) we need to check whether the region's
        // TARS changed due to e.g. eager reclaim.
        HeapWord* const top_at_rebuild_start = _cm->top_at_rebuild_start(region_idx);
        if (top_at_rebuild_start == NULL) {
          return false;
        }

        MemRegion next_chunk = MemRegion(hr->bottom(), top_at_rebuild_start).intersection(MemRegion(cur, chunk_size_in_words));
        if (next_chunk.is_empty()) {
          break;
        }

        const Ticks start = Ticks::now();
        size_t marked_bytes = rebuild_rem_set_in_region(_cm->nextMarkBitMap(),
                                                        top_at_mark_start,
                                                        top_at_rebuild_start,
                                                        hr,
                                                        next_chunk);
        Tickspan time = Ticks::now() - start;

        if (G1TraceRebuildRemSet) {
          gclog_or_tty->print_cr("Rebuilt region %u "
                                  "live " SIZE_FORMAT " "
                                  "time %.3fms "
                                  "marked bytes " SIZE_FORMAT " "
                                  "bot " PTR_FORMAT " "
                                  "TAMS " PTR_FORMAT " "
                                  "TARS " PTR_FORMAT,
                                  region_idx,
                                  _cm->live_bytes(region_idx) * HeapWordSize,
                                  time.seconds() * 1000.0,
                                  marked_bytes,
                                  p2i(hr->bottom()),
                                  p2i(top_at_mark_start),
                                  p2i(top_at_rebuild_start));
        }

        if (marked_bytes > 0) {
          total_marked_bytes += marked_bytes;
        } 
        cur += chunk_size_in_words;

        _cm->do_yield_check();
        if (_cm->has_aborted()) {
          return true;
        }
      }

      // In the final iteration of the loop the region might have been eagerly reclaimed.
      // Simply filter out those regions. We can not just use region type because there
      // might have already been new allocations into these regions.
      DEBUG_ONLY(HeapWord* const top_at_rebuild_start = _cm->top_at_rebuild_start(region_idx);)
      assert(top_at_rebuild_start == NULL ||
             total_marked_bytes == hr->next_marked_bytes(),
             err_msg("Marked bytes " SIZE_FORMAT " for region %u (%s) in [bottom, TAMS) do not match calculated marked bytes " SIZE_FORMAT " "
             "(" PTR_FORMAT " " PTR_FORMAT " " PTR_FORMAT ")",
             total_marked_bytes, hr->hrm_index(), hr->get_type_str(), hr->next_marked_bytes(),
             p2i(hr->bottom()), p2i(top_at_mark_start), p2i(top_at_rebuild_start)));
       // Abort state may have changed after the yield check.
      return _cm->has_aborted();
    }
  };

  HeapRegionClaimer _hr_claimer;

  ConcurrentMark* _cm;

  uint _worker_id_offset;
  uint _n_workers;
public:
  G1RebuildRemSetTask(ConcurrentMark* cm,
                      uint n_workers,
                      uint worker_id_offset) :
      AbstractGangTask("G1 Rebuild Remembered Set"),
      _cm(cm),
      _n_workers(n_workers),
      _hr_claimer(n_workers),
      _worker_id_offset(worker_id_offset) {
  }

  void work(uint worker_id) {
    SuspendibleThreadSetJoiner sts_join;
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    G1RebuildRemSetHeapRegionClosure cl(g1h, _cm, _worker_id_offset + worker_id);
    g1h->heap_region_par_iterate_from_worker_offset(&cl, &_hr_claimer, worker_id);
  }
};

void G1RemSet::rebuild_rem_set(ConcurrentMark* cm,
                               FlexibleWorkGang* workers,
                               uint worker_id_offset) {
  uint num_workers = cm->calc_parallel_marking_threads();
  G1RebuildRemSetTask cl(cm,
                         num_workers,
                         worker_id_offset);
  workers->run_task(&cl);
}
