// game_gfx.cpp — a 2D graphical frontend for the v2 rule-reward game. [VERSION 2]
//
// Same security model as the terminal game.cpp -- the ONLY thing that changed is
// presentation. Every state change still goes through gg_trigger() and is decided
// by the GPU engine against its authoritative shadow + the rule table; every value
// drawn is read from the GPU-healed mirror page. The window shows, live, what the
// GPU owns: HP, ammo, cooldown, position, monster HP, and the GPU-owned score.
//
// Graphics: raylib (fetched + built by CMake; nothing to install). A top-down
// arena: you (a circle) move on a 32x32 grid, a monster sits at the top with an
// HP bar, and a HUD on the right shows the protected state and the engine counters.
//
// Controls
//   WASD / arrows  MOVE   (bounds-checked by the GPU; a blocked move flashes)
//   SPACE          SHOOT  (needs ammo>0 and cooldown=0)
//   R              RELOAD
//   E              USE_ITEM (spend score to heal)
//   1..4           select weapon
//   F              ATTACK (honest): CPU proposes damage, GPU verifies
//   C              ATTACK (T1 cheat): claim MAX damage -- works only in BOUNDS mode
//   X              try to forge score directly -- shows it cannot be expressed
//   T              TAMPER (T0): poke the score page; the GPU heals it + alerts
//
// A per-frame TICK trigger advances weapon cooldown and the GPU's RNG, exactly as
// a real frame would. (NB: spamming frames is itself the documented rate-cheat the
// GPU cannot bound -- see DESIGN_v2.md section 11.)

#include "raylib.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "gpuguard.h"
#include "game_rules.cuh"
#include "protected.h"

// Protected read views, allocated in SLOT_* order (slot index == enum), before
// gg_start() so their initials are baked into the GPU shadow.
struct GameState {
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

static const int GRID = 32;
static const int CELL = 20;                 // px per grid cell
static const int PLAY = GRID * CELL;        // 640 px play area
static const int HUD  = 320;                // right-hand HUD width
static const int WINW = PLAY + HUD;
static const int WINH = PLAY;

static std::mt19937 g_cpuRng{12345};

// The CPU-heavy "computation": an honest client rolls within the weapon's cap; a
// T1 cheat always claims the maximum. In REDERIVE mode the GPU ignores this; in
// BOUNDS mode it trusts it within the cap.
static int32_t cpu_attack_calc(int weapon, bool cheat) {
	static const int32_t cap[GG_NWEAPONS] = {25, 40, 60, 90};
	if (weapon < 0 || weapon >= GG_NWEAPONS) weapon = 0;
	if (cheat) return cap[weapon];
	std::uniform_int_distribution<int> d(0, cap[weapon]);
	return d(g_cpuRng);
}

static const char* modeName() {
	int m = gg_combat_mode();
	return m == GG_MODE_BOUNDS ? "BOUNDS (GPU trusts a capped claim)"
	     : "REDERIVE (GPU recomputes; claim ignored)";
}

// A tiny rolling event log for the HUD.
struct Log {
	std::string lines[6];
	void push(const std::string& s) {
		for (int i = 5; i > 0; i--) lines[i] = lines[i-1];
		lines[0] = s;
	}
} g_log;

static const char* verdictStr(int v) { return v == GG_APPLIED ? "APPLIED" : "REJECTED"; }

static void drawBar(int x, int y, int w, int h, float frac, Color fill, const char* label) {
	if (frac < 0) frac = 0; if (frac > 1) frac = 1;
	DrawRectangle(x, y, w, h, Fade(LIGHTGRAY, 0.3f));
	DrawRectangle(x, y, (int)(w * frac), h, fill);
	DrawRectangleLines(x, y, w, h, DARKGRAY);
	DrawText(label, x + 6, y + h/2 - 8, 16, BLACK);
}

int main() {
	GameState st;
	gg_start();   // bake initials, launch the engine (or stub)

	InitWindow(WINW, WINH, "GPU Rule-Reward Game (v2)");
	SetTargetFPS(60);

	int weapon = 3;                  // start on the strongest weapon
	int flashBlocked = 0;            // frames to flash a blocked move
	g_log.push(gg_active() ? "GPU engine active" : "CPU STUB - unprotected");
	g_log.push(std::string("mode: ") + modeName());

	while (!WindowShouldClose()) {
		// ---- per-frame housekeeping trigger ----
		gg_wait(gg_trigger(GG_TRIG_TICK, 0, 0, 0, 0));

		// ---- input -> triggers ----
		if (IsKeyPressed(KEY_ONE))   weapon = 0;
		if (IsKeyPressed(KEY_TWO))   weapon = 1;
		if (IsKeyPressed(KEY_THREE)) weapon = 2;
		if (IsKeyPressed(KEY_FOUR))  weapon = 3;

		auto move = [&](int dir, const char* name) {
			int v = gg_wait(gg_trigger(GG_TRIG_MOVE, dir, 0, 0, 0));
			if (v != GG_APPLIED) { flashBlocked = 12; g_log.push(std::string("MOVE ")+name+" REJECTED (edge)"); }
		};
		if (IsKeyPressed(KEY_A) || IsKeyPressed(KEY_LEFT))  move(0, "L");
		if (IsKeyPressed(KEY_D) || IsKeyPressed(KEY_RIGHT)) move(1, "R");
		if (IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP))    move(2, "U");
		if (IsKeyPressed(KEY_S) || IsKeyPressed(KEY_DOWN))  move(3, "D");

		if (IsKeyPressed(KEY_SPACE)) {
			int v = gg_wait(gg_trigger(GG_TRIG_SHOOT, 0, 0, 0, 0));
			g_log.push(std::string("SHOOT ") + verdictStr(v));
		}
		if (IsKeyPressed(KEY_R)) {
			gg_wait(gg_trigger(GG_TRIG_RELOAD, 0, 0, 0, 0));
			g_log.push("RELOAD");
		}
		if (IsKeyPressed(KEY_E)) {
			int v = gg_wait(gg_trigger(GG_TRIG_USE_ITEM, 0, 0, 0, 0));
			g_log.push(std::string("USE_ITEM ") + verdictStr(v));
		}

		// Honest ATTACK: CPU computes a candidate, GPU verifies.
		if (IsKeyPressed(KEY_F)) {
			int32_t prev  = st.monhp.get();
			int32_t claim = cpu_attack_calc(weapon, /*cheat*/false);
			int v = gg_wait(gg_trigger(GG_TRIG_ATTACK, SLOT_MON_HP, prev, claim, weapon));
			char b[64]; snprintf(b, sizeof b, "ATTACK w%d claim=%d %s", weapon, claim, verdictStr(v));
			g_log.push(b);
		}
		// T1 cheat ATTACK: claim the maximum every swing.
		if (IsKeyPressed(KEY_C)) {
			int32_t prev  = st.monhp.get();
			int32_t claim = cpu_attack_calc(weapon, /*cheat*/true);
			int v = gg_wait(gg_trigger(GG_TRIG_ATTACK, SLOT_MON_HP, prev, claim, weapon));
			char b[80]; snprintf(b, sizeof b, "CHEAT ATTACK w%d claim=MAX(%d) %s", weapon, claim, verdictStr(v));
			g_log.push(b);
		}
		// Try to forge score directly: there is no such trigger.
		if (IsKeyPressed(KEY_X)) {
			g_log.push("forge score: NO SUCH TRIGGER (impossible in v2)");
		}
		// T0 tamper: poke the mirror page. The engine heals it and raises the alert.
		if (IsKeyPressed(KEY_T)) {
			volatile uint32_t* p = gg_slot_ptr(SLOT_SCORE);
			if (p) *p = 999999;
			g_log.push("TAMPER: poked score page <- 999999 (watch it heal)");
		}

		// ---- read authoritative (GPU-healed) values ----
		int hp = st.hp.get(), ammo = st.ammo.get(), cd = st.weaponcd.get();
		int px = st.posx.get(), py = st.posy.get();
		int mhp = st.monhp.get(), score = st.score.get();
		bool tampered = gg_tampered();

		// ---- draw ----
		BeginDrawing();
		ClearBackground(RAYWHITE);

		// play area
		DrawRectangle(0, 0, PLAY, PLAY, CLITERAL(Color){245,245,250,255});
		for (int i = 0; i <= GRID; i += 4) {
			DrawLine(i*CELL, 0, i*CELL, PLAY, Fade(LIGHTGRAY, 0.6f));
			DrawLine(0, i*CELL, PLAY, i*CELL, Fade(LIGHTGRAY, 0.6f));
		}

		// monster (top center) with HP bar
		int mx = (GRID/2) * CELL, my = 2 * CELL;
		DrawRectangle(mx - 24, my - 24, 48, 48, CLITERAL(Color){180,60,60,255});
		DrawRectangleLines(mx - 24, my - 24, 48, 48, MAROON);
		DrawRectangle(mx - 40, my - 44, 80, 8, Fade(GRAY, 0.4f));
		DrawRectangle(mx - 40, my - 44, (int)(80 * (mhp / 120.0f)), 8, RED);
		DrawText("MONSTER", mx - 36, my + 28, 14, MAROON);

		// player
		int cx = px * CELL + CELL/2, cy = py * CELL + CELL/2;
		DrawCircle(cx, cy, CELL*0.7f, CLITERAL(Color){50,100,200,255});
		DrawCircleLines(cx, cy, CELL*0.7f, DARKBLUE);
		if (flashBlocked > 0) { DrawCircleLines(cx, cy, CELL*0.95f, RED); flashBlocked--; }

		// HUD panel
		int hx = PLAY + 16, hy = 14;
		DrawRectangle(PLAY, 0, HUD, PLAY, CLITERAL(Color){250,250,252,255});
		DrawLine(PLAY, 0, PLAY, PLAY, LIGHTGRAY);

		DrawText(gg_active() ? "GPU ENGINE ACTIVE" : "CPU STUB (UNPROTECTED)",
		         hx, hy, 18, gg_active() ? DARKGREEN : RED); hy += 26;
		DrawText(modeName(), hx, hy, 13, DARKGRAY); hy += 24;

		drawBar(hx, hy, HUD-32, 22, hp/100.0f, GREEN, TextFormat("HP %d", hp)); hy += 30;
		drawBar(hx, hy, HUD-32, 22, ammo/12.0f, CLITERAL(Color){40,120,220,255}, TextFormat("AMMO %d", ammo)); hy += 30;
		drawBar(hx, hy, HUD-32, 22, cd/3.0f, ORANGE, TextFormat("COOLDOWN %d", cd)); hy += 34;

		DrawText(TextFormat("SCORE  %d", score), hx, hy, 24, CLITERAL(Color){30,30,30,255}); hy += 30;
		DrawText("(rises only on a GPU-adjudicated kill)", hx, hy, 12, GRAY); hy += 24;
		DrawText(TextFormat("weapon: %d    pos: (%d,%d)", weapon, px, py), hx, hy, 14, DARKGRAY); hy += 26;

		DrawText(TextFormat("engine  applied=%u  rejected=%u", gg_applied(), gg_rejected()),
		         hx, hy, 13, DARKGRAY); hy += 18;
		DrawText(TextFormat("cpu-damage-mismatch=%u", gg_mismatch()), hx, hy, 13,
		         gg_mismatch() ? CLITERAL(Color){180,100,0,255} : DARKGRAY); hy += 24;

		if (tampered) {
			DrawRectangle(hx-6, hy-4, HUD-20, 26, Fade(RED, 0.15f));
			DrawText(TextFormat("TAMPER DETECTED (slot %d) + healed", gg_tamper_slot()),
			         hx, hy, 14, RED);
		}
		hy += 30;

		// event log
		DrawText("log:", hx, hy, 14, DARKGRAY); hy += 20;
		for (int i = 0; i < 6; i++)
			DrawText(g_log.lines[i].c_str(), hx, hy + i*16, 12,
			         i == 0 ? BLACK : Fade(DARKGRAY, 0.7f));

		// controls footer
		DrawText("WASD move  SPACE shoot  R reload  E item  1-4 weapon",
		         8, PLAY-40, 13, DARKGRAY);
		DrawText("F attack   C cheat-attack   X forge-score   T tamper",
		         8, PLAY-22, 13, DARKGRAY);

		EndDrawing();
	}

	CloseWindow();
	gg_shutdown();
	return 0;
}
