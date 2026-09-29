#include "protected.h"
#include "gpuguard.h"

#include <iostream>

namespace {
// Used when gg_alloc_slot fails, so a Protected is never left with a null ptr and
// the failure shows up as an obviously wrong value rather than a crash.
uint32_t g_deadSlot = 0;
}

Protected::Protected(int value)
	: slot(gg_alloc_slot((uint32_t)value)),          // lazily brings up the GPU
	  ptr(slot >= 0 ? gg_slot_ptr(slot) : (volatile uint32_t*)&g_deadSlot)
{
	if (slot < 0) {
		std::cerr << "[protected] no slot available; this value is UNGUARDED\n";
	}
}

Protected::~Protected() = default;   // slots are not recycled; see README

// --- the write channel ------------------------------------------------------
// Every mutation forwards to gg_submit and waits for the engine to consume it, so
// the next read observes the change. These are the operators v2 removes.

Protected& Protected::operator=(int value) {
	wait(submitAsync(GG_OP_SET, (int32_t)value));
	return *this;
}

Protected& Protected::operator+=(int delta) {
	wait(submitAsync(GG_OP_ADD, (int32_t)delta));
	return *this;
}

Protected& Protected::operator-=(int delta) {
	wait(submitAsync(GG_OP_ADD, (int32_t)(-delta)));
	return *this;
}

Protected& Protected::operator++() { return *this += 1; }
Protected& Protected::operator--() { return *this -= 1; }

uint64_t Protected::submitAsync(int op, int32_t arg) {
	return gg_submit(slot, op, arg);
}

void Protected::wait(uint64_t seq) {
	gg_wait(seq);
}

void Protected::print() const {
	std::cout << "P[" << get() << "]\n";
}

std::ostream& operator<<(std::ostream& stream, const Protected& value) {
	return stream << value.get();
}
