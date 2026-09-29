// game.cpp — a terminal coin game guarded by the GPU write channel.  [VERSION 1]
//
// Plain C++, compiled by g++/clang++, never nvcc. It knows only "gpuguard.h" and
// the Protected datatype.
//
// THE V1 MODEL: a WRITE CHANNEL. Money is held in two Protected counters, pennies
// and dollars. The game changes them with ordinary arithmetic (pennies += 10),
// which forwards to gg_submit -- the GPU applies the value and keeps the
// authoritative copy in a register. This is simple and scales to any value.
//
// WHAT IT DEFENDS, AND THE HOLE IT LEAVES:
//   tamper  -- an EXTERNAL memory editor pokes the plaintext page. It diverges
//              from the GPU's register shadow and is DETECTED. (Defended, like v2.)
//   forge   -- INJECTED in-process code calls the same gg_submit the game uses.
//              The engine cannot tell it from a legitimate write, so it APPLIES it
//              and there is NO alert. This is v1's central weakness -- the confused
//              deputy / direct forge. code/game/ (v2) closes it by removing the raw
//              write path (rules + GPU-owned rewards). Run `forge` here, then run
//              it in v2, to see the difference.
//
// A background watcher prints the tamper alert the instant the GPU sets it.

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include "gpuguard.h"
#include "protected.h"

static const int PLAY_REWARD        = 10;    // pennies minted per `play`
static const int PENNIES_PER_DOLLAR = 100;   // pennies melted into one dollar

struct GameState {
	bool      running = true;
	Protected pennies{0};
	Protected dollars{0};
};

// --- background tamper watcher --------------------------------------------
static std::atomic<bool> g_stopWatch{false};
static std::atomic<bool> g_reported{false};

static void watcher() {
	while (!g_stopWatch.load(std::memory_order_relaxed)) {
		if (gg_tampered() && !g_reported.exchange(true)) {
			std::cout << "\n\n*** TAMPER DETECTED on slot #" << gg_tamper_slot()
			          << " -- the plaintext was edited outside the write channel.\n"
			          << "*** Caught immediately, while waiting for input.\n> "
			          << std::flush;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

static void printWelcome() {
	std::cout << "\n=== Penny Game (v1: write channel) ===\n";
	if (gg_active())
		std::cout << "money is GPU-guarded; the game updates it via the write channel.\n";
	else
		std::cout << "*** UNPROTECTED BUILD (CPU stub) *** logic only, nothing guarded.\n";
	std::cout << "Type 'help' to see available commands.\n";
}

static void printHelp() {
	std::cout << "\nCommands:\n"
	          << "  play       - earn " << PLAY_REWARD << " pennies (pennies += " << PLAY_REWARD << ")\n"
	          << "  exchange   - melt " << PENNIES_PER_DOLLAR << " pennies into 1 dollar\n"
	          << "  score      - show how many dollars and pennies you have\n"
	          << "  tamper     - EXTERNAL edit of the dollars page (detected)\n"
	          << "  forge      - INJECTED raw gg_submit on dollars (APPLIED -- v1 hole)\n"
	          << "  status     - engine counters and alert state\n"
	          << "  addrs      - where the money lives\n"
	          << "  help / quit\n";
}

static void score(GameState& s) {
	std::cout << "Dollars: " << s.dollars.get() << "  Pennies: " << s.pennies.get() << '\n';
}

// What an external memory editor does: store a value straight into the plaintext
// page, bypassing the write channel. The GPU shadow disagrees, so this is caught.
static void tamper(GameState& s) {
	volatile uint32_t* p = gg_slot_ptr(s.dollars.slotIndex());
	if (!p) { std::cout << "no dollars slot\n"; return; }
	std::cout << "external edit: poking dollars page (" << (const void*)p << ") <- 999\n";
	*p = 999;
	std::cout << "dollars now reads " << s.dollars.get()
	          << " (the read is honest; the value is not real -- watch for the alert)\n";
}

// Play the injected attacker: forge dollars with a RAW gg_submit -- the very call
// the game's own operators use. In v1 the engine applies it: no divergence, no
// alert. This is the confused deputy v2 removes.
static void forge(GameState& s) {
	uint32_t app0 = gg_applied();
	gg_wait(gg_submit(s.dollars.slotIndex(), GG_OP_SET, 999));
	std::cout << "injected raw gg_submit(SET dollars=999) ... dollars reads "
	          << s.dollars.get()
	          << (s.dollars.get() == 999 ? "  (!! APPLIED -- forged, no alert: the v1 hole)"
	                                     : "  (unchanged)")
	          << "\n  engine applied count: " << app0 << " -> " << gg_applied()
	          << "   alert: " << gg_tampered() << '\n';
}

static void status(GameState& s) {
	std::cout << "pennies=" << s.pennies.get() << "  dollars=" << s.dollars.get()
	          << "  applied=" << gg_applied()
	          << "  rejected=" << gg_rejected()
	          << "  alert=" << gg_tampered();
	if (gg_tampered()) std::cout << " (slot #" << gg_tamper_slot() << ")";
	std::cout << '\n';
}

static void addrs(GameState& s) {
	std::cout << "where the money lives -- what an attacker can reach:\n"
	          << "  pennies       " << s.pennies.addr()
	          << "   host, mapped   <- attacker-writable, but guarded\n"
	          << "  dollars       " << s.dollars.addr()
	          << "   host, mapped   <- attacker-writable, but guarded\n"
	          << "  doorbell/tail " << (const void*)gg_bell_addr()
	          << "   DEVICE         <- one-way; not host-writable\n"
	          << "  request ring  " << gg_ring_devptr()
	          << "   DEVICE         <- cudaMalloc, not host-addressable\n"
	          << "  shadows       (GPU registers -- no address at all)\n";
}

static void processCommand(const std::string& line, GameState& state) {
	std::istringstream in(line);
	std::string cmd;
	in >> cmd;

	if (cmd == "play") {
		state.pennies += PLAY_REWARD;
		std::cout << "You played: +" << PLAY_REWARD << " pennies. Pennies: "
		          << state.pennies.get() << '\n';
	} else if (cmd == "exchange") {
		if (state.pennies.get() >= PENNIES_PER_DOLLAR) {
			state.pennies -= PENNIES_PER_DOLLAR;
			state.dollars += 1;
			std::cout << "Melted " << PENNIES_PER_DOLLAR << " pennies into 1 dollar. "
			          << "Dollars: " << state.dollars.get()
			          << "  Pennies: " << state.pennies.get() << '\n';
		} else {
			std::cout << "Not enough pennies -- you need " << PENNIES_PER_DOLLAR
			          << ". Pennies: " << state.pennies.get() << '\n';
		}
	} else if (cmd == "score") {
		score(state);
	} else if (cmd == "tamper") {
		tamper(state);
	} else if (cmd == "forge") {
		forge(state);
	} else if (cmd == "status") {
		status(state);
	} else if (cmd == "addrs") {
		addrs(state);
	} else if (cmd == "late") {
		// Construct a Protected AFTER gg_start(). gg_alloc_slot refuses once the
		// engine is up, so this value is left UNGUARDED (dead slot), with a warning.
		std::cout << "constructing a Protected value AFTER gg_start()...\n";
		Protected late{12345};
		std::cout << "  requested initial = 12345\n"
		          << "  slotIndex = " << late.slotIndex()
		          << "   value reads = " << late.get()
		          << "   addr = " << late.addr() << '\n'
		          << "  guarded page 0 is at " << (const void*)gg_slot_ptr(0) << '\n';
		if (late.slotIndex() < 0)
			std::cout << "  -> REFUSED: no slot, value is UNGUARDED (points at the shared dead slot)\n";
		else
			std::cout << "  -> got slot " << late.slotIndex() << " (unexpected!)\n";
	} else if (cmd == "help") {
		printHelp();
	} else if (cmd == "quit" || cmd == "exit") {
		state.running = false;
	} else if (!cmd.empty()) {
		std::cout << "Unknown command. Type 'help' for options.\n";
	}
}

int main() {
	// Protected members are constructed here, reserving their slots and seeding
	// initial values BEFORE the engine launches (GPU-init).
	GameState state;

	// End of trusted startup: launch the engine. It bakes the initial values into
	// its register shadows, then serves the write channel.
	gg_start();

	printWelcome();
	std::thread w(watcher);

	std::string command;
	while (state.running) {
		std::cout << "\n> " << std::flush;
		if (!std::getline(std::cin, command)) break;
		processCommand(command, state);
	}

	g_stopWatch.store(true);
	w.join();
	std::cout << "Thanks for playing!\n";
	std::cout.flush();
	gg_shutdown();
	return 0;
}
