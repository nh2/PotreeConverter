#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

// An array of per-index counters that can be incremented concurrently from
// many threads, behaving like `vector<atomic<CounterType>>` but storing only a
// narrow `vector<atomic<NarrowType>>` plus a sparse carry side-table.
//
// Motivation: when counting occurrences into a large, mostly-low-valued array,
// only a few entries are expected to exceed a medium-sized integer type (e.g.
// `uint32_t`). Using a wide counter (e.g. `uint64_t`) for every entry just to
// accommodate those few large entries doubles memory use for no benefit on the
// common entries. This class keeps the narrow per-entry storage for the common
// case while still counting exactly, by recording the rare per-entry overflows
// ("carries") in a small hash map ("carry map").
// The full count of an entry is then `narrow + carry * (narrowMax + 1)`.
//
// `NarrowType` must support wrapping overflow, so it must be an unsigned integer
// type or equivalent. `CounterType` must be an unsigned integer type wide enough to hold
// `carry * (narrowMax + 1) + narrow` for the largest count that can occur.
// (`CounterType` is required to be unsigned: a signed `CounterType` would force
// any negative value to be represented purely through the carry map, since the
// narrow storage only ever wraps upward, so every negative entry would
// needlessly cost a carry-map entry and complicate the invariant. Counts are
// never negative, so we forbid signed types instead.)
template <typename CounterType = uint64_t, typename NarrowType = uint32_t>
class ConcurrentCarriedCounters {
	static_assert(std::is_unsigned<NarrowType>::value, "NarrowType must be an unsigned integer type");
	static_assert(std::is_unsigned<CounterType>::value, "CounterType must be an unsigned integer type");

public:
	explicit ConcurrentCarriedCounters(size_t size)
		: narrow(size) {
		// `std::atomic` is not copyable, so the vector cannot be value-initialized
		// to a non-zero fill; default-construct then zero each element explicitly.
		for (size_t i = 0; i < size; i++) {
			narrow[i].store(0, std::memory_order_relaxed);
		}
	}

	// The compiler cannot synthesize move/copy operations for this class because
	// it holds a `std::mutex`, which is neither movable nor copyable.
	// We provide a move that transfers the (movable) `narrow` vector and `carries`
	// map and leaves the moved-to object with a fresh, default-constructed mutex.
	//
	// Moving is only valid when no concurrent `fetch_add`/`get` calls are in
	// flight (e.g. after joining the incrementing thread pool); otherwise the
	// reads of `other`'s members would race with those calls.
	ConcurrentCarriedCounters(ConcurrentCarriedCounters&& other) noexcept
		: narrow(std::move(other.narrow))
		, carries(std::move(other.carries)) {
	}

	ConcurrentCarriedCounters(const ConcurrentCarriedCounters &) = delete;
	ConcurrentCarriedCounters& operator=(const ConcurrentCarriedCounters &) = delete;
	ConcurrentCarriedCounters& operator=(ConcurrentCarriedCounters &&) = delete;

	size_t size() const {
		return narrow.size();
	}

	// Atomically add `value` to the counter at `index`. Safe to call
	// concurrently from many threads.
	//
	// `value` is a `CounterType` (unsigned) on purpose, so a negative add is
	// forbidden by the type system. Subtraction is intentionally not supported,
	// even when the final total would stay non-negative: under concurrency a
	// borrow's effect on the carry cannot be attributed to a single thread the
	// way a forward wrap can. (A "borrow" is the subtraction analog of a carry:
	// when subtracting underflows the narrow storage below `0`, the missing
	// amount must be taken from the carry, i.e. the carry is decremented; just as
	// in pen-and-paper subtraction one "borrows" from the next-higher digit.)
	// Interleaved adds and subtracts can drive the carry transiently negative
	// (e.g. a subtract that borrows is observed before a concurrent add that
	// would have refilled it), which an unsigned carry cannot represent.
	// Supporting subtraction would require a signed carry plus a scheme tolerating
	// transient negativity, which we do not need here: this structure only ever
	// counts upward.
	void fetch_add(size_t index, CounterType value) {
		// Correctness of the carry bookkeeping:
		// We `fetch_add(value)` on the `NarrowType` and read its previous value
		// `prev`. In wide (`CounterType`) arithmetic, adding `value` to `prev` wraps
		// the narrow storage exactly `(prev + value) / (narrowMax + 1)` times,
		// and that many wraps is attributable solely to this thread's own add
		// (because `prev` is the unique value this thread's atomic add returned).
		// So this thread bumps `index`'s carry by exactly that number of wraps.
		// Different threads' adds compose correctly because each computes the wrap
		// count of its own contribution from its own `prev`, and carries are commutative,
		// so the order in which concurrent adds take the carry mutex does not matter.
		NarrowType prev = narrow[index].fetch_add(NarrowType(value), std::memory_order_relaxed);
		CounterType numWraps = (CounterType(prev) + value) / (CounterType(narrowMax) + 1);
		if (numWraps > 0) {
			std::lock_guard<std::mutex> lock(carriesMutex);
			carries[index] += numWraps;
		}
	}

	// Atomically add one to the counter at `index`. Safe to call concurrently
	// from many threads.
	void increment(size_t index) {
		fetch_add(index, 1);
	}

	// Return the exact full-width count at `index`.
	//
	// If a consistent read is desired, this must only be called once all concurrent `increment`
	// calls have completed (e.g. after joining the thread pool that did the incrementing).
	// If called while increments are still in flight, the result is not
	// corrupting but is simply unspecified: the narrow value and the carry count
	// are read at slightly different times, so the returned count may be lower
	// than the true momentary count (and may even be momentarily inconsistent,
	// e.g. miss a carry that has not yet been recorded for an already-wrapped
	// narrow value). It never over-counts or returns a garbage/torn value.
	CounterType get(size_t index) {
		CounterType narrowValue = CounterType(narrow[index].load(std::memory_order_relaxed));
		CounterType carry = 0;
		{
			std::lock_guard<std::mutex> lock(carriesMutex);
			auto it = carries.find(index);
			if (it != carries.end()) {
				carry = CounterType(it->second);
			}
		}
		return narrowValue + (carry * (CounterType(narrowMax) + 1));
	}

	// A read-only forward iterator over the full-width counts, so callers can use
	// a range-based `for` loop (`for (CounterType count : counters) ...`).
	// Dereferencing yields the same value as `get(index)` for the current index,
	// and so carries the same in-flight-increment caveat as `get` (it must only
	// be used once all concurrent increments have completed). It is not a
	// `const_iterator` because `get` takes the carry mutex and so is non-const.
	class Iterator {
	public:
		Iterator(ConcurrentCarriedCounters* owner, size_t index)
			: owner(owner)
			, index(index) {
		}

		CounterType operator*() const {
			return owner->get(index);
		}

		Iterator& operator++() {
			index++;
			return *this;
		}

		bool operator==(const Iterator& other) const {
			return (owner == other.owner) && (index == other.index);
		}

		bool operator!=(const Iterator& other) const {
			return !(*this == other);
		}

	private:
		ConcurrentCarriedCounters* owner;
		size_t index;
	};

	Iterator begin() {
		return Iterator(this, 0);
	}

	Iterator end() {
		return Iterator(this, narrow.size());
	}

private:
	static constexpr NarrowType narrowMax = std::numeric_limits<NarrowType>::max();

	std::vector<std::atomic<NarrowType>> narrow;
	std::mutex carriesMutex;
	std::unordered_map<size_t, CounterType> carries;
};
