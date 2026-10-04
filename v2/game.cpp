// game.cpp — a small combat game guarded by the GPU rule-reward engine.  [V2]
//
// Plain C++, compiled by g++/clang++, never nvcc. It knows "gpuguard.h", the
// read-only Protected view, and the trigger/slot vocabulary in game_rules.cuh.
//
// THE V2 MODEL. The player has HP, ammo, position, a weapon, and a GPU-owned
// score; there is a monster with HP and armor. The game NEVER writes these. It
// submits TRIGGERS (gg_trigger) and the GPU decides the result from its own
// authoritative state and the rules:
//   * GPU-friendly ops  : MOVE, SHOOT, RELOAD, TICK, USE_ITEM -- cheap to fully
//     validate on the GPU, so the GPU is the true authority over them.
//   * CPU-heavy op       : ATTACK -- the CPU does the expensive damage calc and
//     proposes a result; the GPU verifies it. In GG_MODE_REDERIVE the GPU
//     RECOMPUTES damage and ignores the CPU's claim (true authority); in
//     GG_MODE_BOUNDS the GPU only range-checks the claim (the detect-vs-prevent
//     trap). Switch with GG_COMBAT_MODE=0|1 and compare -- that is the experiment.
//
// WHAT AN IN-PROCESS CHEAT (T1) CAN DO HERE:
//   * `cheat_score`  : try to set score directly. THERE IS NO SUCH TRIGGER -- it
//                      cannot be expressed. (Contrast v1's forge, which worked.)
//   * `cheat_attack` : submit ATTACK with max claimed damage every swing. In
//                      BOUNDS mode this SUCCEEDS (legal transition); in REDERIVE
//                      mode the claim is ignored, so it does NOT. Run both.
//
// A background watcher prints the tamper alert the instant the GPU sets it.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include "gpuguard.h"
#include "game_rules.cuh"
#include "protected.h"

// Protected read views over the GPU's authoritative slots. Allocated in SLOT_*
// order so the slot index equals the enum. Constructed before gg_start().
struct GameState {
	bool      running = true;
	Protected hp      {100};  // SLOT_HP
	Protected ammo    {12};   // SLOT_AMMO
	Protected posx    {16};   // SLOT_POSX
	Protected posy    {16};   // SLOT_POSY
	Protected weaponcd{0};    // SLOT_WEAPON_CD
	Protected monhp   {120};  // SLOT_MON_HP
	Protected monarmor{5};    // SLOT_MON_ARMOR
	Protected buff    {0};    // SLOT_BUFF
	Protected rng     {0x1234};// SLOT_RNG
	Protected score   {0};    // SLOT_SCORE
};

static std::atomic<bool> g_stopWatch{false};
static std::atomic<bool> g_reported{false};

static void watcher() {
	while (!g_stopWatch.load(std::memory_order_relaxed)) {
		if (gg_tampered() && !g_reported.exchange(true)) {
			std::cout << "\n\n*** TAMPER DETECTED on slot #" << gg_tamper_slot()
			          << " -- the mirror page was edited outside the rules.\n"
			          << "*** The engine healed it back to the authoritative value.\n> "
			          << std::flush;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

static const char* modeName() {
	int m = gg_combat_mode();
	return m == GG_MODE_BOUNDS ? "BOUNDS (GPU trusts a bounded CPU claim)"
	     : m == GG_MODE_REDERIVE ? "REDERIVE (GPU recomputes; CPU claim ignored)"
	     : "unknown";
}

// --- the CPU-HEAVY computation --------------------------------------------
// Stands in for the expensive, CPU-friendly part of combat (collision, buffs,
// status effects, damage roll). An HONEST client computes a realistic damage and
// proposes it. The GPU either recomputes it (REDERIVE) or bounds-checks it
// (BOUNDS). This is where the "CPU as helper, GPU as authority" story lives.
static std::mt19937 g_cpuRng{12345};
static int32_t cpu_attack_calc(int weapon, bool cheat) {
	// bounds-mode ceilings the honest client stays within; a cheat ignores them.
	static const int32_t cap[GG_NWEAPONS] = {25, 40, 60, 90};
	if (weapon < 0 || weapon >= GG_NWEAPONS) weapon = 0;
	if (cheat) return cap[weapon];                 // T1: always claim the maximum
	std::uniform_int_distribution<int> d(0, cap[weapon]);
	return d(g_cpuRng);                            // honest: a plausible roll
}

// Submit one ATTACK. Returns the verdict. `cheat` toggles the T1 max-damage claim.
static int do_attack(GameState& s, int weapon, bool cheat) {
	int32_t prev   = s.monhp.get();                // CAS guard: cite current mon HP
	int32_t claim  = cpu_attack_calc(weapon, cheat);
	uint64_t seq   = gg_trigger(GG_TRIG_ATTACK, /*monster*/SLOT_MON_HP, prev, claim, weapon);
	return gg_wait(seq);
}

static void status(GameState& s) {
	std::cout << "mode=" << modeName() << "\n"
	          << "  HP=" << s.hp.get() << " ammo=" << s.ammo.get()
	          << " pos=(" << s.posx.get() << "," << s.posy.get() << ")"
	          << " cd=" << s.weaponcd.get()
	          << " | monHP=" << s.monhp.get() << " armor=" << s.monarmor.get()
	          << " | SCORE=" << s.score.get() << "\n"
	          << "  engine: applied=" << gg_applied() << " rejected=" << gg_rejected()
	          << " cpu-damage-mismatch=" << gg_mismatch()
	          << " alert=" << gg_tampered() << "\n";
}

static void addrs(GameState& s) {
	std::cout << "where the state lives -- what an attacker can reach:\n"
	          << "  score (mirror) " << s.score.addr()
	          << "   host, mapped   <- editable, but healed+flagged, and never the authority\n"
	          << "  doorbell/tail  " << (const void*)gg_bell_addr()
	          << "   DEVICE         <- one-way; not host-writable\n"
	          << "  request ring   " << gg_ring_devptr()
	          << "   DEVICE         <- cudaMalloc, not host-addressable\n"
	          << "  shadows + rules (GPU on-chip / device memory -- no host address)\n";
}

// What an external memory editor (T0) does: poke the mirror page. The engine
// heals it and raises the alert -- the value never actually changes.
static void tamper(GameState& s) {
	volatile uint32_t* p = gg_slot_ptr(SLOT_SCORE);
	if (!p) { std::cout << "no score slot\n"; return; }
	std::cout << "external edit: poking score page (" << (const void*)p << ") <- 999999\n";
	*p = 999999;
	std::this_thread::sleep_for(std::chrono::milliseconds(30));  // let the engine poll
	std::cout << "score now reads " << s.score.get()
	          << " (healed back; watch for the alert)\n";
}

// The v1 'forge' has NO v2 equivalent: there is no trigger that writes a value.
static void cheat_score() {
	std::cout << "A T1 cheat wants score=999999. In v1 it called gg_submit(SET, 999999).\n"
	          << "In v2 there is NO such call: the trigger vocabulary is "
	          << "{MOVE,SHOOT,RELOAD,TICK,USE_ITEM,ATTACK}. Score is GPU-owned and\n"
	          << "rises ONLY when the engine adjudicates a kill. The cheat cannot be expressed.\n";
}

// The realistic T1 cheat in v2: submit ATTACK with max claimed damage every swing.
static void cheat_attack(GameState& s, int rounds) {
	int kills0 = 0; int32_t score0 = s.score.get();
	int applied = 0, rejected = 0;
	for (int i = 0; i < rounds; i++) {
		// clear cooldown-free attacks; feed TICKs so weapon logic advances
		int v = do_attack(s, /*weapon*/3, /*cheat*/true);
		if (v == GG_APPLIED) applied++; else rejected++;
	}
	std::cout << "T1 cheat_attack x" << rounds << " (weapon 3, claim=max):\n"
	          << "  mode=" << modeName() << "\n"
	          << "  applied=" << applied << " rejected=" << rejected
	          << "  score " << score0 << " -> " << s.score.get()
	          << "  cpu-damage-mismatch(now)=" << gg_mismatch() << "\n"
	          << (gg_combat_mode() == GG_MODE_BOUNDS
	              ? "  -> BOUNDS: max damage accepted every swing -- the cheat WORKS (legal transitions).\n"
	              : "  -> REDERIVE: the claim was ignored; damage came from the GPU roll -- cheat NEUTRALISED.\n");
	(void)kills0;
}

// Quick latency microbenchmark over a stream of cheap triggers.
static void bench(int n) {
	auto t0 = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < n; i++) gg_wait(gg_trigger(GG_TRIG_TICK, 0, 0, 0, 0));
	auto t1 = std::chrono::high_resolution_clock::now();
	double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
	std::cout << "bench: " << n << " round-trip triggers in " << us << " us -> "
	          << (us / n) << " us/trigger\n";
}

static void printHelp() {
	std::cout << "\nCommands (GPU-friendly ops):\n"
	          << "  move <l|r|u|d>   request a move (rejected if out of bounds)\n"
	          << "  shoot            fire (needs ammo>0 and cooldown=0)\n"
	          << "  reload           refill the magazine\n"
	          << "  tick             advance one frame (cooldown--, RNG step)\n"
	          << "  item             spend score to heal (GPU-checked)\n"
	          << "\nCPU-heavy op:\n"
	          << "  attack [weapon]  honest attack; CPU proposes damage, GPU verifies\n"
	          << "\nAttacks (T1, in-process cheat):\n"
	          << "  cheat_score      show that forging score cannot even be expressed\n"
	          << "  cheat_attack [n] submit max claimed damage n times (works only in BOUNDS)\n"
	          << "  tamper           external edit of the score page (detected + healed)\n"
	          << "\nInfo:  score | status | addrs | bench [n] | help | quit\n"
	          << "Mode: set GG_COMBAT_MODE=0 (REDERIVE) or 1 (BOUNDS) before launch.\n";
}

static void processCommand(const std::string& line, GameState& s) {
	std::istringstream in(line);
	std::string cmd; in >> cmd;

	if (cmd == "move") {
		std::string d; in >> d;
		int dir = d=="l"?0 : d=="r"?1 : d=="u"?2 : d=="d"?3 : -1;
		int v = gg_wait(gg_trigger(GG_TRIG_MOVE, dir, 0, 0, 0));
		std::cout << (v==GG_APPLIED?"moved":"rejected (out of bounds / bad dir)")
		          << " pos=(" << s.posx.get() << "," << s.posy.get() << ")\n";
	} else if (cmd == "shoot") {
		int v = gg_wait(gg_trigger(GG_TRIG_SHOOT, 0,0,0,0));
		std::cout << (v==GG_APPLIED?"fired":"rejected (no ammo or cooling)")
		          << " ammo=" << s.ammo.get() << " cd=" << s.weaponcd.get() << "\n";
	} else if (cmd == "reload") {
		gg_wait(gg_trigger(GG_TRIG_RELOAD, 0,0,0,0));
		std::cout << "reloaded, ammo=" << s.ammo.get() << "\n";
	} else if (cmd == "tick") {
		gg_wait(gg_trigger(GG_TRIG_TICK, 0,0,0,0));
		std::cout << "tick, cd=" << s.weaponcd.get() << "\n";
	} else if (cmd == "item") {
		int v = gg_wait(gg_trigger(GG_TRIG_USE_ITEM, 0,0,0,0));
		std::cout << (v==GG_APPLIED?"healed":"rejected (not enough score or HP full)")
		          << " HP=" << s.hp.get() << " score=" << s.score.get() << "\n";
	} else if (cmd == "attack") {
		int w = 0; in >> w;
		int v = do_attack(s, w, /*cheat*/false);
		std::cout << (v==GG_APPLIED?"attack applied":"attack rejected (stale/illegal)")
		          << " monHP=" << s.monhp.get() << " score=" << s.score.get() << "\n";
	} else if (cmd == "cheat_score") {
		cheat_score();
	} else if (cmd == "cheat_attack") {
		int n = 10; in >> n; cheat_attack(s, n);
	} else if (cmd == "tamper") {
		tamper(s);
	} else if (cmd == "score" || cmd == "status") {
		status(s);
	} else if (cmd == "addrs") {
		addrs(s);
	} else if (cmd == "bench") {
		int n = 10000; in >> n; bench(n);
	} else if (cmd == "help") {
		printHelp();
	} else if (cmd == "quit" || cmd == "exit") {
		s.running = false;
	} else if (!cmd.empty()) {
		std::cout << "Unknown command. Type 'help'.\n";
	}
}

int main() {
	GameState state;   // reserves slots + seeds initials BEFORE launch (GPU-init)
	gg_start();

	std::cout << "\n=== GPU Rule-Reward Game (v2) ===\n";
	if (gg_active())
		std::cout << "state is GPU-authoritative; the game drives it only via triggers.\n";
	else
		std::cout << "*** UNPROTECTED BUILD (CPU stub) *** logic only, nothing guarded.\n";
	std::cout << "combat mode: " << modeName() << "\nType 'help'.\n";

	std::thread w(watcher);
	std::string command;
	while (state.running) {
		std::cout << "\n> " << std::flush;
		if (!std::getline(std::cin, command)) break;
		processCommand(command, state);
	}
	g_stopWatch.store(true);
	w.join();
	std::cout << "bye\n"; std::cout.flush();
	gg_shutdown();
	return 0;
}
