// game.cpp — terminal frontend for the v2 rule-reward game.  [VERSION 2]
//
// Plain C++, never nvcc. Same security model as game_gfx.cpp; this one is headless
// (useful for scripted experiments and for boxes without a display). It drives the
// SAME triggers and reads the SAME GPU-owned slots via game_common.h.
//
// The arena has GG_NMON monsters (see game_rules.cuh). Monster 0 spawns adjacent
// to the player, so melee ATTACK on monster 0 lands without moving; monsters only
// move when you issue `step` (GG_TRIG_MONSTER_STEP), so this headless demo stays
// deterministic. Combat uses GG_COMBAT_MODE (0 REDERIVE / 1 BOUNDS).

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
#include "game_common.h"

static std::mt19937 g_rng{12345};

static std::atomic<bool> g_stopWatch{false};
static std::atomic<bool> g_reported{false};
static void watcher() {
	while (!g_stopWatch.load(std::memory_order_relaxed)) {
		if (gg_tampered() && !g_reported.exchange(true))
			std::cout << "\n\n*** TAMPER DETECTED on slot #" << gg_tamper_slot()
			          << " -- edited outside the rules; the engine healed it.\n> " << std::flush;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

static const char* modeName() {
	int m = gg_combat_mode();
	return m == GG_MODE_BOUNDS ? "BOUNDS (GPU trusts a capped claim)"
	                           : "REDERIVE (GPU recomputes; claim ignored)";
}
static const char* vs(int v) { return v == GG_APPLIED ? "APPLIED" : "REJECTED"; }

// one honest or cheating melee attack on monster `k`
static int attackMonster(int k, int weapon, bool cheat) {
	int hp = gg_rd(MON_HP(k));
	int32_t claim = gg_cpu_attack_calc(g_rng, weapon, cheat);
	return gg_wait(gg_trigger(GG_TRIG_ATTACK, k, hp, claim, weapon));
}

static void look() {
	std::cout << "player pos=(" << gg_rd(SLOT_POSX) << "," << gg_rd(SLOT_POSY) << ")"
	          << " HP=" << gg_rd(SLOT_HP) << " ammo=" << gg_rd(SLOT_AMMO) << "\n";
	for (int i = 0; i < GG_NMON; i++)
		std::cout << "  monster " << i << ": HP=" << gg_rd(MON_HP(i))
		          << " armor=" << gg_rd(MON_ARMOR(i))
		          << " pos=(" << gg_rd(MON_POSX(i)) << "," << gg_rd(MON_POSY(i)) << ")\n";
}

static void status() {
	std::cout << "mode=" << modeName()
	          << "  SCORE=" << gg_rd(SLOT_SCORE)
	          << "  applied=" << gg_applied() << " rejected=" << gg_rejected()
	          << "  cpu-damage-mismatch=" << gg_mismatch()
	          << "  alert=" << gg_tampered() << "\n";
}

static void cheatAttack(int rounds, int weapon) {
	int score0 = gg_rd(SLOT_SCORE), applied = 0, rejected = 0;
	for (int i = 0; i < rounds; i++)
		(attackMonster(0, weapon, true) == GG_APPLIED ? applied : rejected)++;
	std::cout << "T1 cheat melee x" << rounds << " on monster 0 (claim=MAX, w" << weapon << "):\n"
	          << "  mode=" << modeName() << "  applied=" << applied << " rejected=" << rejected
	          << "  score " << score0 << " -> " << gg_rd(SLOT_SCORE)
	          << "  mismatch(now)=" << gg_mismatch() << "\n  -> "
	          << (gg_combat_mode() == GG_MODE_BOUNDS
	              ? "BOUNDS: max damage accepted -> cheat WORKS (legal transitions)\n"
	              : "REDERIVE: claim ignored -> cheat NEUTRALISED\n");
}

static void tamper() {
	volatile uint32_t* p = gg_slot_ptr(SLOT_SCORE);
	if (!p) return;
	std::cout << "external edit: score page <- 999999\n";
	*p = 999999;
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	std::cout << "score now reads " << gg_rd(SLOT_SCORE) << " (healed; watch the alert)\n";
}

static void bench(int n) {
	auto t0 = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < n; i++) gg_wait(gg_trigger(GG_TRIG_TICK, 0, 0, 0, 0));
	auto t1 = std::chrono::high_resolution_clock::now();
	double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
	std::cout << "bench: " << n << " round-trips in " << us << " us -> " << (us/n) << " us/trigger\n";
}

static void help() {
	std::cout << "\nGPU-friendly: move <l|r|u|d>  shoot  reload  tick  item\n"
	          << "CPU-heavy   : attack [k] [w]   (melee monster k, GPU range-gates it)\n"
	          << "world       : step             (MONSTER_STEP: monsters chase + contact dmg)\n"
	          << "              look             (player + all monster positions)\n"
	          << "T1 cheat    : cheat_attack [n] [w]   (max-claim melee on monster 0)\n"
	          << "              forge                  (shows score-forge is impossible)\n"
	          << "T0          : tamper             (external edit of score page)\n"
	          << "info        : status  addrs  bench [n]  help  quit\n"
	          << "mode: set GG_COMBAT_MODE=0 (REDERIVE) or 1 (BOUNDS) before launch.\n";
}

static void processCommand(const std::string& line, bool& running) {
	std::istringstream in(line); std::string cmd; in >> cmd;
	if (cmd == "move") {
		std::string d; in >> d;
		int dir = d=="l"?0 : d=="r"?1 : d=="u"?2 : d=="d"?3 : -1;
		int v = gg_wait(gg_trigger(GG_TRIG_MOVE, dir, 0, 0, 0));
		std::cout << (v==GG_APPLIED?"moved":"rejected") << " pos=("
		          << gg_rd(SLOT_POSX) << "," << gg_rd(SLOT_POSY) << ")\n";
	} else if (cmd == "shoot") {
		std::cout << "SHOOT " << vs(gg_wait(gg_trigger(GG_TRIG_SHOOT,0,0,0,0)))
		          << " ammo=" << gg_rd(SLOT_AMMO) << " cd=" << gg_rd(SLOT_WEAPON_CD) << "\n";
	} else if (cmd == "reload") {
		gg_wait(gg_trigger(GG_TRIG_RELOAD,0,0,0,0)); std::cout << "ammo=" << gg_rd(SLOT_AMMO) << "\n";
	} else if (cmd == "tick") {
		gg_wait(gg_trigger(GG_TRIG_TICK,0,0,0,0)); std::cout << "cd=" << gg_rd(SLOT_WEAPON_CD) << "\n";
	} else if (cmd == "item") {
		std::cout << "USE_ITEM " << vs(gg_wait(gg_trigger(GG_TRIG_USE_ITEM,0,0,0,0)))
		          << " HP=" << gg_rd(SLOT_HP) << " score=" << gg_rd(SLOT_SCORE) << "\n";
	} else if (cmd == "attack") {
		int k = 0, w = 3; in >> k; in >> w;
		int v = attackMonster(k, w, false);
		std::cout << "melee m" << k << " " << vs(v)
		          << " (REJECTED if out of melee range) monHP=" << gg_rd(MON_HP(k))
		          << " score=" << gg_rd(SLOT_SCORE) << "\n";
	} else if (cmd == "step") {
		gg_wait(gg_trigger(GG_TRIG_MONSTER_STEP,0,0,0,0)); std::cout << "monsters stepped\n"; look();
	} else if (cmd == "look")    { look();
	} else if (cmd == "cheat_attack") { int n=10,w=3; in>>n; in>>w; cheatAttack(n,w);
	} else if (cmd == "forge") {
		std::cout << "score forge: NO SUCH TRIGGER in v2. Score is GPU-owned (kill-only).\n";
	} else if (cmd == "tamper")  { tamper();
	} else if (cmd == "status")  { status();
	} else if (cmd == "addrs") {
		std::cout << "score page " << (const void*)gg_slot_ptr(SLOT_SCORE) << " host/mapped (healed, flagged)\n"
		          << "doorbell   " << (const void*)gg_bell_addr() << " DEVICE (one-way)\n"
		          << "ring       " << gg_ring_devptr() << " DEVICE (not host-addressable)\n"
		          << "shadows+rules: GPU on-chip/device (no host address)\n";
	} else if (cmd == "bench")   { int n=10000; in>>n; bench(n);
	} else if (cmd == "help")    { help();
	} else if (cmd == "quit" || cmd == "exit") { running = false;
	} else if (!cmd.empty())     { std::cout << "Unknown command. Type 'help'.\n"; }
}

int main() {
	gg_setup_slots();   // allocate all slots in canonical order, before launch
	gg_start();

	std::cout << "\n=== GPU Rule-Reward Game (v2, terminal) ===\n";
	std::cout << (gg_active() ? "state is GPU-authoritative; triggers only.\n"
	                          : "*** UNPROTECTED BUILD (CPU stub) *** logic only.\n");
	std::cout << "combat mode: " << modeName() << "   monsters: " << GG_NMON << "\nType 'help'.\n";

	std::thread w(watcher);
	bool running = true;
	std::string command;
	while (running) {
		std::cout << "\n> " << std::flush;
		if (!std::getline(std::cin, command)) break;
		processCommand(command, running);
	}
	g_stopWatch.store(true); w.join();
	std::cout << "bye\n"; std::cout.flush();
	gg_shutdown();
	return 0;
}
