#pragma once
#include "pch.h"

class NesCpu;
class NesConsole;
class BaseNesPpu;

// Real widescreen for Mega Man 1 (NES) and hacks that keep its engine.
// The 4:3 picture is produced by the normal PPU; the extra columns left and right are drawn
// from the game's own level data, and a handful of game routines (spawning, off-screen checks,
// sprite placement, collisions) are intercepted so objects live in the wide area too.
// The ROM is never modified: routines are intercepted by address at execution time.
struct MegaWide
{
	static constexpr int MaxWidth = 768;

	static uint8_t* HookAt;   // 64K table of intercept ids (null when not intercepting)

	static void OnLoad(NesConsole* console);
	static void OnUnload(NesConsole* console);
	static bool IsGameSupported();

	// margin: extra pixels on each side (0 = 4:3). fill: 0 black, 1 dimmed extension, 2 full extension
	static void SetOptions(int margin, int fill, bool wideObjects);
	static int GetWidth();

	static bool Hook(NesCpu* cpu);
	static void OnOamDma(uint8_t page);
	static void OnScanline(BaseNesPpu* ppu);

	// Builds the wide frame from the finished 256x240 ARGB picture. Returns false if nothing to do.
	static bool Compose(const uint32_t* src, uint32_t srcWidth, uint32_t srcHeight, const uint32_t* palette, std::vector<uint32_t>& out, uint32_t& outWidth);
};
