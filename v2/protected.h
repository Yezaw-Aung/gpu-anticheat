#pragma once

// protected.h — the game-facing READ view of a protected value.  [VERSION 2]
//
// *** In v2 a Protected is READ-ONLY. *** This is the single most important
// difference from v1. v1's Protected had mutating operators (=, +=, --) that
// forwarded to the raw write channel; those operators ARE the confused-deputy hole
// (any in-process code could call them to forge a value). v2 deletes them
// entirely. The only way to change a protected value is to submit a TRIGGER via
// gg_trigger(), which the GPU adjudicates -- there is no host-callable "set".
//
// A Protected therefore wraps a slot index and a pointer into the mirror page the
// engine heals every iteration. Reading is a plain load; the value you read is
// whatever the GPU most recently healed the page to, i.e. the authoritative value
// (a T0 edit is reverted within one engine iteration and flagged).
//
// Header-only: there is no protected.cpp in v2, because there is no write logic to
// implement -- another way the raw write path simply does not exist.

#include <cstdint>
#include <ostream>
#include "gpuguard.h"

class Protected {
public:
	explicit Protected(int value = 0)
		: slot(gg_alloc_slot((uint32_t)value)),
		  ptr(slot >= 0 ? gg_slot_ptr(slot) : &dead) {}

	Protected(const Protected&)            = delete;   // a slot is a resource
	Protected& operator=(const Protected&) = delete;

	// Read only. No operator=, +=, ++ -- changing a protected value goes through
	// gg_trigger() and the GPU rules, never through this object.
	operator int() const { return (int)*ptr; }
	int get() const      { return (int)*ptr; }

	int slotIndex() const { return slot; }

	// The address an external memory editor would target. No need to hide it --
	// the design rests on integrity (the engine heals and flags edits), not secrecy.
	const void* addr() const { return (const void*)ptr; }

	friend std::ostream& operator<<(std::ostream& os, const Protected& v) { return os << v.get(); }

private:
	int                 slot;
	volatile uint32_t*  ptr;
	static inline uint32_t dead = 0;   // fallback if slot allocation failed
};
