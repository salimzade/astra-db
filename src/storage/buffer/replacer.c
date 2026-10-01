#include "storage/buffer/buffer_internal.h"

/*
 * The replacement policy: CLOCK.
 *
 * One bit per frame, one index for the whole policy, and a linear sweep. That is the
 * entire algorithm, and it is the right amount of machinery for a pool that has to
 * pick a victim on a cache miss.
 *
 * Why not LRU
 * ------------
 * True least-recently-use needs a timestamp per frame and a list to keep them ordered.
 * Every operation on that list is a pointer chase through a page frame's worth of
 * memory, so a fetch that *hits* pays a cache miss before it pays a comparison, and the
 * list has to be kept consistent against concurrent readers even on that hit path.
 * CLOCK gets the same amortised behaviour - every page is offered a second chance
 * after the hand passes it, which is roughly what an LRU timestamp would have said -
 * for one bit and an index into an array the caller already has in hand.
 *
 * The policy is also, importantly, O(1) in the sense the requirement means: no list to
 * walk, no order to maintain, and no state that can become inconsistent with itself.
 * The only loop is the sweep, and the sweep is bounded by the frame count, which is
 * fixed at construction and small.
 *
 * Complexity, stated honestly
 * ---------------------------
 *   - O(1) for record_access and for reading the hand. Neither touches the frame array
 *     beyond the one frame it names.
 *   - O(1) amortised per eviction in the steady state, and O(n) per eviction
 *     immediately after a burst of fetches that touched every frame. That is the
 *     number that matters: on a workload with real reuse, most evictions find an
 *     unreferenced frame within the first frame or two they look at.
 *   - O(n) worst case for a single call, and bounded at two sweeps. See below.
 *
 * The never-select-a-pinned-page rule
 * -----------------------------------
 * A pinned frame is skipped and its reference bit is *left alone*. That is deliberate,
 * and it is the one place where a naive clock goes subtly wrong: a page that was pinned
 * across a long sweep is not "recently used", it is *busy*, and clearing its bit
 * because it was busy would put it at the front of the eviction queue the instant it
 * was unpinned. Leaving the bit set means it is still owed a second chance the next
 * time it becomes reachable.
 *
 * The caller holds the pool latch, so choosing a frame and reporting it is a single
 * step with respect to every other thread. The replacer calls nothing, blocks nowhere,
 * and cannot be interrupted between "this frame" and "this frame's index".
 */

void astra_replacer_record_access(astra_buffer_frame *frame)
{
    if (frame == NULL) {
        return;
    }
    frame->is_referenced = true;
}

uint32 astra_replacer_hand(const astra_replacer *replacer)
{
    if (replacer == NULL) {
        return ASTRA_FRAME_NONE;
    }
    return replacer->hand;
}

bool astra_replacer_evict(astra_replacer *replacer,
                          astra_buffer_frame *frames,
                          uint32 frame_count,
                          uint64 *out_examined,
                          uint32 *out_frame_index)
{
    uint32 steps;
    uint32 examined;
    uint32 victim = 0u;
    bool found = false;
    uint64 looked_at = 0u;

    if (replacer == NULL || frames == NULL || out_frame_index == NULL) {
        return false;
    }
    if (frame_count == 0u) {
        return false;
    }

    /*
     * The sweep is at most two full passes. `frame_count * 2u` cannot overflow: the
     * pool size is bounded by ASTRA_BUFFER_POOL_PAGES_MAX, which is a million, so the
     * product is at most two million and well inside a uint32. The bound is asserted in
     * buffer_internal.h rather than defended against here, because a pool that large
     * cannot be allocated and there is nothing to defend against.
     */
    for (steps = 0u; steps < frame_count * 2u; ++steps) {
        examined = replacer->hand;
        replacer->hand = (replacer->hand + 1u) % frame_count;
        ++looked_at;

        if (frames[examined].pin_count > 0u) {
            /* Busy. Skipped, and its reference bit deliberately not cleared. */
            continue;
        }

        if (frames[examined].is_referenced) {
            /* Second chance: clear the bit and look at the next frame. */
            frames[examined].is_referenced = false;
            continue;
        }

        /*
         * A separate `found` flag rather than testing the index against a sentinel,
         * because frame 0 is a real frame and ASTRA_FRAME_NONE is 0. A sentinel-based
         * "was anything found" test here would silently report that frame 0 could never
         * be chosen - which, in a pool of one, is every frame.
         */
        victim = examined;
        found = true;
        break;
    }

    if (out_examined != NULL) {
        *out_examined = looked_at;
    }

    if (!found) {
        return false;
    }

    /*
     * The hand has already moved past the victim, which is what makes the sweep fair
     * rather than unfair in the way a "always start at zero" scan is: a pool of two
     * frames alternates between them instead of thrashing on one.
     */
    *out_frame_index = victim;
    return true;
}
