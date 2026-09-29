#pragma once

// protected.h — the game-facing datatype.  [VERSION 1: write channel]
//
// A Protected holds a slot index and a pointer into mapped pinned memory; the
// authoritative copy is a register inside the persistent kernel. Unlike v2 (which
// is read-only from the host), v1 keeps MUTATING operators: they are how the game
// changes a value, by forwarding to the gg_submit write channel.
//
//   x = 5;     ->  gg_submit(slot, GG_OP_SET, 5)
//   x += 10;   ->  gg_submit(slot, GG_OP_ADD, 10)
//   x -= 100;  ->  gg_submit(slot, GG_OP_ADD, -100)
//   ++x; --x;  ->  ADD +1 / -1
//
// This is what makes v1 easy to write and scale -- any value, any amount, plain
// arithmetic. It is ALSO the weakness: the very same gg_submit is callable by
// injected in-process code, and the engine cannot tell the two apart (the
// confused deputy / direct forge). v2 removes these operators precisely to close
// that path.
//
// Two shape constraints carried over from the GPU storage model:
//   1. No operator+ / - by value: they would allocate a GPU slot per temporary.
//      The implicit `operator int` means `score + 5` still compiles as a plain int.
//   2. Copying is deleted: a slot is a resource; two objects sharing one would
//      double-free and alias each other's state.
//
// COST on an RTX 3060: ~7.3 us for a guarded write end to end (submit + round
// trip); reads are a plain load, ~1 ns. Use for event-rate values (gold, score,
// ammo, flags), not per-frame ones.

#include <cstdint>
#include <iosfwd>

class Protected {
public:
	explicit Protected(int value = 0);
	~Protected();

	Protected(const Protected&)            = delete;   // a slot is a resource
	Protected& operator=(const Protected&) = delete;

	// Read: direct load from the plaintext page. Cheap, and safe because the
	// engine is continuously checking that page against its register shadow.
	operator int() const { return (int)*ptr; }
	int get() const      { return (int)*ptr; }

	// Mutating operators: the write channel. Each submits a value op and waits for
	// the engine to consume it, so a following read reflects the change.
	Protected& operator=(int value);      // SET
	Protected& operator+=(int delta);     // ADD delta
	Protected& operator-=(int delta);     // ADD -delta
	Protected& operator++();              // ADD +1  (prefix)
	Protected& operator--();              // ADD -1  (prefix)

	// Fire-and-forget variants: submit without waiting, then wait later. Handy for
	// batching several updates before a single synchronisation.
	uint64_t submitAsync(int op, int32_t arg);
	void     wait(uint64_t seq);

	int slotIndex() const { return slot; }

	// The address an external memory editor would target. No need to hide it --
	// this design rests on integrity, not secrecy.
	const void* addr() const { return (const void*)ptr; }

	void print() const;

	friend std::ostream& operator<<(std::ostream& stream, const Protected& value);

private:
	int                 slot;
	volatile uint32_t*  ptr;
};
