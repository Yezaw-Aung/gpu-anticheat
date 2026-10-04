// game_gfx.cpp — 2D graphical frontend for the v2 rule-reward game. [VERSION 2]
//
// Same security model as the terminal frontend -- ONLY presentation differs.
// Every state change goes through gg_trigger() and is decided by the GPU engine
// against its authoritative shadow + rules; every value drawn is read from the
// GPU-healed mirror page via gg_rd(). The GPU owns monster HP, armor AND
// POSITIONS, so melee range is GPU-enforced, not CPU-trusted.
//
// *** REAL-TIME NOTE ***
// The hot loop is FIRE-AND-FORGET: it submits triggers and never calls gg_wait().
// Blocking on a GPU round-trip every frame is what made input laggy (the engine
// shares the GPU with OpenGL, so each synchronous round-trip can stall for ms).
// Instead we submit asynchronously and read the mirror page the engine heals --
// the GPU stays authoritative, the frame rate stays at the 60 FPS target, and
// simultaneous key presses work because movement is level-triggered (held keys),
// not edge-triggered.
//
// Controls
//   WASD / arrows  MOVE (hold to keep moving; press two for diagonals)
//   mouse          aim
//   SPACE / LMB    SHOOT (hold to auto-fire; GPU gates ammo + cooldown)
//   F / RMB        MELEE nearest monster (GPU range-gates it)
//   C              MELEE as a T1 cheat: claim MAX damage (works only in BOUNDS)
//   1..4 weapon   R reload   E item   X forge-score (impossible)   T tamper

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "gpuguard.h"
#include "game_rules.cuh"
#include "game_common.h"

static const int   GRID = 32;
static const int   CELL = 20;
static const int   PLAY = GRID * CELL;      // 640
static const int   HUD  = 320;
static const int   WINW = PLAY + HUD;
static const int   WINH = PLAY;
static const float MON_R    = CELL * 0.75f;
static const float BULLET_R = 4.0f;
static const float BULLET_V = 8.0f;

// Timing knobs (seconds). Monsters step slower now; movement/fire repeat fast.
static const double MON_STEP_PERIOD = 0.45;   // was 0.25 -> monsters are slower
static const double MOVE_PERIOD     = 0.06;   // grid move repeat while held
static const double FIRE_PERIOD     = 0.12;   // auto-fire cadence while held

struct Bullet { float x, y, vx, vy; int weapon; bool alive; };

static std::mt19937 g_rng{12345};

static const char* modeName() {
	int m = gg_combat_mode();
	return m == GG_MODE_BOUNDS ? "BOUNDS (GPU trusts a capped claim)"
	                           : "REDERIVE (GPU recomputes; claim ignored)";
}

struct Log {
	std::string lines[6];
	void push(const std::string& s) {
		for (int i = 5; i > 0; i--) lines[i] = lines[i-1];
		lines[0] = s;
	}
} g_log;

// fire-and-forget submit: never blocks on the engine
static inline void sub(int op, int a0, int a1, int a2, int a3) { gg_trigger(op, a0, a1, a2, a3); }

static inline float gx(int cx) { return cx * (float)CELL + CELL * 0.5f; }

static void drawBar(int x, int y, int w, int h, float frac, Color fill, const char* label) {
	if (frac < 0) frac = 0; if (frac > 1) frac = 1;
	DrawRectangle(x, y, w, h, Fade(LIGHTGRAY, 0.3f));
	DrawRectangle(x, y, (int)(w * frac), h, fill);
	DrawRectangleLines(x, y, w, h, DARKGRAY);
	DrawText(label, x + 6, y + h/2 - 8, 16, BLACK);
}

static int nearestMonster(int px, int py) {
	int best = -1; long bestd = 1L<<30;
	for (int i = 0; i < GG_NMON; i++) {
		if (gg_rd(MON_HP(i)) <= 0) continue;
		long dx = px - gg_rd(MON_POSX(i)), dy = py - gg_rd(MON_POSY(i));
		long d = dx*dx + dy*dy;
		if (d < bestd) { bestd = d; best = i; }
	}
	return best;
}

// Fire-and-forget melee; client-side range note is cosmetic (the GPU decides).
static void meleeAttack(int weapon, bool cheat, int px, int py) {
	int k = nearestMonster(px, py);
	if (k < 0) { g_log.push("MELEE: no monster"); return; }
	int hp = gg_rd(MON_HP(k));
	int32_t claim = gg_cpu_attack_calc(g_rng, weapon, cheat);
	sub(GG_TRIG_ATTACK, k, hp, claim, weapon);
	long dx = px - gg_rd(MON_POSX(k)), dy = py - gg_rd(MON_POSY(k));
	bool inRange = dx*dx + dy*dy <= 9;
	char b[96];
	snprintf(b, sizeof b, "%sMELEE m%d w%d %s", cheat ? "CHEAT " : "", k, weapon,
	         inRange ? "(in range)" : "(too far -> GPU rejects)");
	g_log.push(b);
}

int main() {
	gg_setup_slots();
	gg_start();

	InitWindow(WINW, WINH, "GPU Rule-Reward Game (v2)");
	SetTargetFPS(60);

	int weapon = 3;
	int flashBlocked = 0;
	std::vector<Bullet> bullets;
	double lastStep = GetTime();
	double moveAccum = 0, fireAccum = 0;
	g_log.push(gg_active() ? "GPU engine active" : "CPU STUB - unprotected");
	g_log.push(std::string("mode: ") + modeName());

	while (!WindowShouldClose()) {
		double dt = GetFrameTime();

		// ---- per-frame + timed world triggers (fire-and-forget) ----
		sub(GG_TRIG_TICK, 0, 0, 0, 0);
		if (GetTime() - lastStep > MON_STEP_PERIOD) {
			sub(GG_TRIG_MONSTER_STEP, 0, 0, 0, 0);
			lastStep = GetTime();
		}

		// ---- weapon select ----
		if (IsKeyPressed(KEY_ONE))   weapon = 0;
		if (IsKeyPressed(KEY_TWO))   weapon = 1;
		if (IsKeyPressed(KEY_THREE)) weapon = 2;
		if (IsKeyPressed(KEY_FOUR))  weapon = 3;

		int px = gg_rd(SLOT_POSX), py = gg_rd(SLOT_POSY);

		// ---- movement: level-triggered + repeat timer, so held keys and
		//      diagonals (two keys at once) both work, independent of frame rate ----
		moveAccum += dt;
		if (moveAccum >= MOVE_PERIOD) {
			bool moved = false, blocked = false;
			bool L = IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT);
			bool R = IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT);
			bool U = IsKeyDown(KEY_W) || IsKeyDown(KEY_UP);
			bool D = IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN);
			if (L) { if (px > 0)  sub(GG_TRIG_MOVE,0,0,0,0); else blocked = true; moved = true; }
			if (R) { if (px < 31) sub(GG_TRIG_MOVE,1,0,0,0); else blocked = true; moved = true; }
			if (U) { if (py > 0)  sub(GG_TRIG_MOVE,2,0,0,0); else blocked = true; moved = true; }
			if (D) { if (py < 31) sub(GG_TRIG_MOVE,3,0,0,0); else blocked = true; moved = true; }
			if (moved) moveAccum = 0;
			if (blocked) flashBlocked = 10;
		}

		float pcx = gx(px), pcy = gx(py);
		Vector2 mouse = GetMousePosition();

		// ---- shoot: hold to auto-fire; client cadence + mirror ammo/cooldown
		//      gate the VISUAL bullet, the GPU gates the real ammo ----
		fireAccum += dt;
		bool firing = IsKeyDown(KEY_SPACE) || IsMouseButtonDown(MOUSE_BUTTON_LEFT);
		if (firing && fireAccum >= FIRE_PERIOD && gg_rd(SLOT_AMMO) > 0 && gg_rd(SLOT_WEAPON_CD) == 0) {
			sub(GG_TRIG_SHOOT, 0, 0, 0, 0);
			float dx = mouse.x - pcx, dy = mouse.y - pcy;
			float len = std::sqrt(dx*dx + dy*dy); if (len < 1e-3f) { dx = 1; dy = 0; len = 1; }
			bullets.push_back({ pcx, pcy, dx/len*BULLET_V, dy/len*BULLET_V, weapon, true });
			fireAccum = 0;
		}

		if (IsKeyPressed(KEY_F) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) meleeAttack(weapon, false, px, py);
		if (IsKeyPressed(KEY_C)) meleeAttack(weapon, true, px, py);
		if (IsKeyPressed(KEY_R)) { sub(GG_TRIG_RELOAD,0,0,0,0); g_log.push("RELOAD"); }
		if (IsKeyPressed(KEY_E)) { sub(GG_TRIG_USE_ITEM,0,0,0,0); g_log.push("USE_ITEM (heal if score+room)"); }
		if (IsKeyPressed(KEY_X)) g_log.push("forge score: NO SUCH TRIGGER (impossible in v2)");
		if (IsKeyPressed(KEY_T)) {
			volatile uint32_t* p = gg_slot_ptr(SLOT_SCORE);
			if (p) *p = 999999;
			g_log.push("TAMPER: poked score page (watch it heal)");
		}

		// ---- advance bullets + resolve hits (fire-and-forget BULLET_HIT) ----
		for (auto& b : bullets) {
			if (!b.alive) continue;
			b.x += b.vx; b.y += b.vy;
			if (b.x < 0 || b.y < 0 || b.x > PLAY || b.y > PLAY) { b.alive = false; continue; }
			for (int i = 0; i < GG_NMON; i++) {
				int hp = gg_rd(MON_HP(i));
				if (hp <= 0) continue;
				float mx = gx(gg_rd(MON_POSX(i))), my = gx(gg_rd(MON_POSY(i)));
				float dx = b.x - mx, dy = b.y - my;
				if (dx*dx + dy*dy <= (MON_R + BULLET_R)*(MON_R + BULLET_R)) {
					int32_t claim = gg_cpu_attack_calc(g_rng, b.weapon, false);
					sub(GG_TRIG_BULLET_HIT, i, hp, claim, b.weapon);
					b.alive = false;
					break;
				}
			}
		}
		bullets.erase(std::remove_if(bullets.begin(), bullets.end(),
		              [](const Bullet& b){ return !b.alive; }), bullets.end());

		// ---- read authoritative values for drawing ----
		int hp = gg_rd(SLOT_HP), ammo = gg_rd(SLOT_AMMO), cd = gg_rd(SLOT_WEAPON_CD);
		int score = gg_rd(SLOT_SCORE);
		bool tampered = gg_tampered();

		// ---- draw ----
		BeginDrawing();
		ClearBackground(RAYWHITE);

		DrawRectangle(0, 0, PLAY, PLAY, CLITERAL(Color){245,245,250,255});
		for (int i = 0; i <= GRID; i += 4) {
			DrawLine(i*CELL, 0, i*CELL, PLAY, Fade(LIGHTGRAY, 0.6f));
			DrawLine(0, i*CELL, PLAY, i*CELL, Fade(LIGHTGRAY, 0.6f));
		}

		for (int i = 0; i < GG_NMON; i++) {
			int mhp = gg_rd(MON_HP(i));
			if (mhp <= 0) continue;
			float mx = gx(gg_rd(MON_POSX(i))), my = gx(gg_rd(MON_POSY(i)));
			DrawRectangle((int)(mx-MON_R), (int)(my-MON_R), (int)(MON_R*2), (int)(MON_R*2), CLITERAL(Color){180,60,60,255});
			DrawRectangleLines((int)(mx-MON_R), (int)(my-MON_R), (int)(MON_R*2), (int)(MON_R*2), MAROON);
			DrawRectangle((int)(mx-MON_R), (int)(my-MON_R-8), (int)(MON_R*2), 5, Fade(GRAY,0.4f));
			DrawRectangle((int)(mx-MON_R), (int)(my-MON_R-8), (int)(MON_R*2 * (mhp/(float)GG_MON_HP_INIT)), 5, RED);
			DrawText(TextFormat("%d", i), (int)(mx-4), (int)(my-7), 14, WHITE);
		}

		DrawCircleLines((int)pcx, (int)pcy, 3.0f * CELL, Fade(DARKBLUE, 0.25f));   // melee range ring
		DrawCircle((int)pcx, (int)pcy, CELL*0.6f, CLITERAL(Color){50,100,200,255});
		DrawCircleLines((int)pcx, (int)pcy, CELL*0.6f, DARKBLUE);
		if (flashBlocked > 0) { DrawCircleLines((int)pcx, (int)pcy, CELL*0.9f, RED); flashBlocked--; }
		DrawLineEx(CLITERAL(Vector2){pcx,pcy}, mouse, 1.5f, Fade(DARKGRAY, 0.5f));
		for (auto& b : bullets) DrawCircle((int)b.x, (int)b.y, BULLET_R, CLITERAL(Color){30,30,30,255});

		// HUD
		int hx = PLAY + 16, hy = 14;
		DrawRectangle(PLAY, 0, HUD, PLAY, CLITERAL(Color){250,250,252,255});
		DrawLine(PLAY, 0, PLAY, PLAY, LIGHTGRAY);

		// FPS (top-right); green when at/above 55
		DrawText(TextFormat("%d FPS", GetFPS()), WINW - 74, 12, 16,
		         GetFPS() >= 55 ? DARKGREEN : (GetFPS() >= 40 ? ORANGE : RED));

		DrawText(gg_active() ? "GPU ENGINE ACTIVE" : "CPU STUB (UNPROTECTED)",
		         hx, hy, 18, gg_active() ? DARKGREEN : RED); hy += 26;
		DrawText(modeName(), hx, hy, 13, DARKGRAY); hy += 24;

		drawBar(hx, hy, HUD-32, 22, hp/100.0f, hp > 25 ? GREEN : RED, TextFormat("HP %d", hp)); hy += 30;
		drawBar(hx, hy, HUD-32, 22, ammo/12.0f, CLITERAL(Color){40,120,220,255}, TextFormat("AMMO %d", ammo)); hy += 30;
		drawBar(hx, hy, HUD-32, 22, cd/3.0f, ORANGE, TextFormat("COOLDOWN %d", cd)); hy += 34;

		DrawText(TextFormat("SCORE  %d", score), hx, hy, 24, CLITERAL(Color){30,30,30,255}); hy += 30;
		DrawText("(rises only on a GPU-adjudicated kill)", hx, hy, 12, GRAY); hy += 22;
		DrawText(TextFormat("weapon %d   pos (%d,%d)   bullets %d", weapon, px, py, (int)bullets.size()),
		         hx, hy, 13, DARKGRAY); hy += 24;

		DrawText(TextFormat("engine applied=%u rejected=%u", gg_applied(), gg_rejected()), hx, hy, 13, DARKGRAY); hy += 18;
		DrawText(TextFormat("cpu-damage-mismatch=%u", gg_mismatch()), hx, hy, 13,
		         gg_mismatch() ? CLITERAL(Color){180,100,0,255} : DARKGRAY); hy += 24;

		if (tampered) {
			DrawRectangle(hx-6, hy-4, HUD-20, 26, Fade(RED, 0.15f));
			DrawText(TextFormat("TAMPER DETECTED (slot %d)+healed", gg_tamper_slot()), hx, hy, 14, RED);
		}
		hy += 30;

		DrawText("log:", hx, hy, 14, DARKGRAY); hy += 20;
		for (int i = 0; i < 6; i++)
			DrawText(g_log.lines[i].c_str(), hx, hy + i*16, 12, i == 0 ? BLACK : Fade(DARKGRAY, 0.7f));

		DrawText("WASD move  mouse aim  SPACE/LMB shoot  F/RMB melee", 8, PLAY-40, 13, DARKGRAY);
		DrawText("C cheat-melee  1-4 weapon  R reload  E item  X forge  T tamper", 8, PLAY-22, 13, DARKGRAY);

		EndDrawing();
	}

	CloseWindow();
	gg_shutdown();
	return 0;
}
