#include "pch.h"
#include "NES/MegaWide.h"
#include "NES/NesCpu.h"
#include "NES/NesConsole.h"
#include "NES/BaseNesPpu.h"
#include "NES/BaseMapper.h"
#include "NES/NesMemoryManager.h"
#include "NES/NesTypes.h"

uint8_t* MegaWide::HookAt = nullptr;

namespace {
	NesConsole* _con = nullptr;
	BaseMapper* _mapper = nullptr;
	uint8_t* _ram = nullptr;
	uint8_t* _prg = nullptr;
	uint32_t _bankMask = 0;
	bool _supported = false;      // engine signature found
	bool _hooksOk = false;        // all intercept sites verified in the ROM

	int _margin = 0, _fill = 1;
	bool _wideObjects = true;

	uint8_t _hookTable[0x10000];

	// sprite extension state
	uint8_t _oxBuf[64];           // written by the sprite intercepts (high byte of X, 0x40 = HUD)
	bool _oxFresh = false;
	uint8_t _oxPending[64];
	bool _dmaHappened = false;
	uint8_t _ox[64];
	uint8_t _oam[256];
	int _rel16 = 0, _colX = 0, _colW = 0;

	// per-frame output of the side areas
	uint8_t _codes[240][MegaWide::MaxWidth];   // 0..63 colour, |0x40 dimmed, 0x80 nothing
	uint8_t _ovl[240][256];                    // extra sprites inside the 4:3 window, 0xFF = none
	int _frameD = 0, _frameW = 256;
	bool _frameLevel = false, _frameUsed = false;

	struct Lv {
		bool valid = false;
		int stage = 0, bank = 0, worldx = 0, cam = 0, s27 = 0, s28 = 0, s29 = 0;
		int rowScr[30] = {}, rowLo[30] = {}, rowHi[30] = {};
		int D = 0, hudT = 0, hudC = 0;
		bool dInit = false;
	} L;

	// PPU values captured for the current line
	struct Line { uint16_t v = 0, bgBase = 0, sprBase = 0; bool large = false, bgOn = false, sprOn = false, grey = false; uint8_t pal[32] = {}; } P;

	inline int CurW() { return 256 + 2 * _margin; }
	inline uint8_t Rom8(int bank, int addr) { return _prg[((uint32_t)(bank & _bankMask) << 14) | (addr & 0x3FFF)]; }
	inline uint8_t Vram(uint16_t addr) { return _mapper->DebugReadVram(addr); }
	inline uint8_t Rd(uint16_t addr) { return addr < 0x2000 ? _ram[addr & 0x7FF] : _mapper->DebugReadRam(addr); }

	struct Patch { uint8_t bank; uint16_t addr; uint8_t len; uint8_t id; uint8_t exp[40]; };
	const Patch _patches[] = {
		{7,0xD8BF,13,0x01,{0x18,0xa5,0x1a,0x69,0xff,0x85,0x04,0xa5,0x1b,0x69,0x00,0x85,0x05}},
		{7,0xD95A,13,0x01,{0x18,0xa5,0x1a,0x69,0xff,0x85,0x04,0xa5,0x1b,0x69,0x00,0x85,0x05}},
		{7,0xD8D5,2,0x05,{0xc5,0x05}}, {7,0xD8DF,2,0x06,{0xc5,0x04}},
		{7,0xD973,2,0x05,{0xc5,0x05}}, {7,0xD97C,2,0x06,{0xc5,0x04}},
		{7,0xD906,2,0x02,{0xc5,0x1b}}, {7,0xD90F,2,0x03,{0xc5,0x1a}},
		{7,0xD934,2,0x02,{0xc5,0x1b}}, {7,0xD93E,2,0x03,{0xc5,0x1a}},
		{7,0xD8E6,4,0x07,{0xb1,0x06,0x85,0x00}}, {7,0xD943,4,0x07,{0xb1,0x06,0x85,0x00}},
		{5,0x9946,11,0x08,{0x38,0xbd,0x80,0x04,0xe5,0x1a,0xbd,0x60,0x04,0xe5,0x1b}},
		{5,0xBE76,11,0x08,{0x38,0xbd,0x80,0x04,0xe5,0x1a,0xbd,0x60,0x04,0xe5,0x1b}},
		{5,0xB949,22,0x09,{0x38,0xbd,0x80,0x04,0xe5,0x1a,0xa8,0xbd,0x60,0x04,0xe5,0x1b,0xd0,0x0c,0xc0,0xf8,0xb0,0x08,0xc0,0x10,0x90,0x04}},
		{7,0xD221,6,0x0a,{0x38,0xbd,0x80,0x04,0xe5,0x1a}},
		{7,0xD283,16,0x0b,{0x18,0x30,0x06,0x65,0x0f,0x90,0x06,0xb0,0xdd,0x65,0x0f,0x90,0xd9,0x9d,0x07,0x02}},
		{7,0xC8DD,18,0x0c,{0x38,0xbd,0x80,0x04,0xe5,0x1a,0x38,0xe5,0x01,0x85,0x00,0x06,0x01,0x18,0x65,0x01,0x85,0x01}},
		{5,0xA18B,16,0x0d,{0x38,0xad,0x80,0x04,0xe5,0x1a,0x38,0xe9,0x07,0x85,0x00,0x18,0x69,0x0e,0x85,0x01}},
		{7,0xCA04,37,0x0e,{0x38,0xbd,0x80,0x04,0xe5,0x1a,0x85,0x0c,0xbd,0x60,0x04,0xe5,0x1b,0xd0,0x18,0xa5,0x0c,0x38,0xe5,0x0e,0x90,0x11,0xc5,0x01,0xf0,0x0b,0xb0,0x0b,0x18,0xa5,0x0c,0x65,0x0e,0xc5,0x00,0x90,0x02}},
		{7,0xD41C,3,0x0f,{0x99,0x07,0x02}},
		{5,0xADE6,14,0x10,{0xa5,0x1a,0x69,0xf8,0x9d,0x80,0x04,0xa5,0x1b,0x69,0x00,0x9d,0x60,0x04}},
		{7,0xD8B0,6,0x12,{0xa5,0x8d,0x29,0x01,0xf0,0xe4}},
		{7,0xD91E,3,0x13,{0x4c,0xb3,0xc3}},
		{7,0xDB3C,13,0x11,{0x18,0xa5,0x1a,0x65,0x00,0x85,0x00,0xa5,0x1b,0x69,0x00,0x85,0x05}},
		{7,0xD13E,3,0x14,{0x20,0x78,0xd4}},
	};
	constexpr int PatchCount = sizeof(_patches) / sizeof(_patches[0]);
	// engine signature: the block-drawing routine that defines the level format
	const uint8_t _sig[] = { 0xa4,0x05,0xb9,0x00,0x8c,0x0a,0xa8,0xb9,0x30,0x8c,0x85,0x06,0xb9,0x31,0x8c,0x85,0x07 };  // $CE58

	void UpdateHookTable()
	{
		bool on = _supported && _hooksOk && _wideObjects;
		memset(_hookTable, 0, sizeof(_hookTable));
		if(on) {
			for(int i = 0; i < PatchCount; i++) _hookTable[_patches[i].addr] = (uint8_t)(i + 1);
		}
		MegaWide::HookAt = on ? _hookTable : nullptr;
	}

	// ---- level model -------------------------------------------------------------------------
	int LvTile(int scr, int tx, int ty, int& pal)
	{
		if(scr < 0 || scr >= 0x30) return -1;
		int room = Rom8(L.bank, 0x8C00 + scr);
		int ptr = Rom8(L.bank, 0x8C30 + room * 2) | (Rom8(L.bank, 0x8C31 + room * 2) << 8);
		if(ptr < 0x8000 || ptr >= 0xC000) return -1;
		int blk = Rom8(L.bank, ptr + ((tx >> 2) << 3) + (ty >> 2));
		if(!_ram[0x68]) {
			// live block overrides (shutters etc.), same test the game uses when drawing
			int px = (tx >> 2) << 5, py = (ty >> 2) << 5;
			for(int y = 0; y < 252; y += 6) {
				int typ = _ram[0x721 + y], es = _ram[0x722 + y];
				if(!typ) continue;
				if(es < scr) continue;
				if(es > scr) break;
				if(_ram[0x723 + y] > px) continue;
				if(_ram[0x725 + y] && _ram[0x725 + y] <= px) continue;
				if(_ram[0x724 + y] > py) continue;
				if(_ram[0x726 + y] && _ram[0x726 + y] <= py) continue;
				if(typ == 4 && L.stage < 12) blk = Rom8(_bankMask, (scr == Rom8(_bankMask, 0xCE49 + L.stage)) ? 0xCE31 + L.stage : 0xCE3D + L.stage);
				break;
			}
		}
		int mt = Rom8(L.bank, 0x8000 + blk * 4 + (((tx >> 1) & 1) << 1) + ((ty >> 1) & 1));
		int at = Rom8(L.bank, 0x8300 + blk);
		pal = (at >> ((((ty >> 1) & 1) << 2) | (((tx >> 1) & 1) << 1))) & 3;
		return ((mt << 2) + ((tx & 1) << 1) + (ty & 1)) & 0xFF;
	}

	// drawable screen range for the strip containing screen scr
	void Strip(int bank, int s27, int s28, int s29, int scr, int& lo, int& hi)
	{
		int idx, start, end;
		lo = hi = scr;
		if(scr >= s27 && scr <= s28) { idx = s29; start = s27; end = s28; }
		else if(scr == s28 + 1 && s29 < 0x3F) { idx = s29 + 1; start = scr; end = scr + (Rom8(bank, 0x8C71 + idx) & 0x1F); }
		else if(scr == s27 - 1 && s29 > 0) { idx = s29 - 1; end = scr; start = scr - (Rom8(bank, 0x8C71 + idx) & 0x1F); }
		else return;
		if(start < 0) start = 0;
		if(end > 0x2F) end = 0x2F;
		lo = start; hi = end;
		{
			int i2 = idx, e = end, f = Rom8(bank, 0x8C71 + i2);
			for(int n = 0; n < 8 && (f & 0x20) && i2 < 0x3F; n++) { i2++; f = Rom8(bank, 0x8C71 + i2); e = e + 1 + (f & 0x1F); if(e > 0x2F) break; hi = e; }
		}
		{
			int i2 = idx, st = start;
			for(int n = 0; n < 8 && i2 > 0; n++) { i2--; int f = Rom8(bank, 0x8C71 + i2); if(!(f & 0x20)) break; st = st - 1 - (f & 0x1F); if(st < 0) break; lo = st; }
		}
	}

	// where the 4:3 window sits inside the wide view (0..2M) for a camera at world x wx in strip lo..hi
	int ViewD(int wx, int lo, int hi)
	{
		int W = CurW(), M = _margin, span = (hi - lo + 1) << 8, vl;
		if(span <= W) vl = (lo << 8) - (W - span) / 2;
		else { vl = wx - M; if(vl < (lo << 8)) vl = lo << 8; if(vl > ((hi + 1) << 8) - W) vl = ((hi + 1) << 8) - W; }
		int d = wx - vl;
		if(d < 0) d = 0;
		if(d > 2 * M) d = 2 * M;
		return d;
	}

	// game-logic window (world x, inclusive) = visible wide view clipped to the strip, from live RAM
	void LogicBounds(int& left, int& right)
	{
		int st = _ram[0x31], wx = (_ram[0x1B] << 8) | _ram[0x1A];
		left = wx; right = wx + 255;
		if(!_margin || st > 11) return;
		int bank = st >= 6 ? st - 6 : st, s27 = _ram[0x27], s28 = _ram[0x28], s29 = _ram[0x29];
		if(s27 > s28 || s28 >= 0x30 || s29 >= 0x40) return;
		int lo, hi;
		Strip(bank, s27, s28, s29, _ram[0x1B], lo, hi);
		int d = ViewD(wx, lo, hi);
		int l = wx - d, r = wx + 255 + (2 * _margin - d);
		if(l < (lo << 8)) l = lo << 8;
		if(r > ((hi + 1) << 8) - 1) r = ((hi + 1) << 8) - 1;
		if(l > wx) l = wx;
		if(r < wx + 255) r = wx + 255;
		left = l; right = r;
	}

	int RowMatch(int base, int ty)
	{
		int m = 0, pal, wtx = (L.worldx >> 3) + ((base - L.cam) << 5);
		for(int tx = 0; tx < 32; tx++) {
			int wx = wtx + tx, scr = wx >> 5, cx = wx & 31;
			int cxp = ((L.worldx >> 3) + tx) & 63;
			int t = LvTile(scr, cx, ty, pal);
			if(t >= 0 && Vram((uint16_t)(0x2000 | ((cxp >> 5) << 10) | (ty << 5) | (cxp & 31))) == t) m++;
		}
		return m;
	}

	void Latch(uint16_t t, uint8_t fx)
	{
		int st = _ram[0x31];
		L.stage = st; L.bank = st >= 6 ? st - 6 : st;
		L.s27 = _ram[0x27]; L.s28 = _ram[0x28]; L.s29 = _ram[0x29];
		L.cam = _ram[0x1B];
		{
			int base = (_ram[0x1B] << 8) | _ram[0x1A];
			int px = (((t >> 10) & 1) << 8) | ((t & 31) << 3) | fx;
			int d = (px - base) & 511;
			if(d > 255) d -= 512;
			L.worldx = base + d;
		}
		L.valid = false;
		if(st > 11 || L.s27 > L.s28 || L.s28 >= 0x30 || L.s29 >= 0x40 || !P.bgOn) { L.dInit = false; return; }
		int alt = _ram[0x460];
		if(_ram[0x1A] != 0 || alt == L.cam) alt = -1;
		int good = 0, prev = L.cam;
		for(int ty = 0; ty < 30; ty++) {
			int a = RowMatch(L.cam, ty), b = alt >= 0 ? RowMatch(alt, ty) : -1;
			if(a >= b && a >= 12) { L.rowScr[ty] = L.cam; good++; }
			else if(b > a && b >= 12) { L.rowScr[ty] = alt; good++; }
			else L.rowScr[ty] = -2;
		}
		if(good < 15) { L.dInit = false; return; }
		for(int ty = 0; ty < 30; ty++) { if(L.rowScr[ty] == -2) L.rowScr[ty] = prev; else prev = L.rowScr[ty]; }
		L.valid = true;
		for(int ty = 0; ty < 30; ty++) {
			if(ty && L.rowScr[ty] == L.rowScr[ty - 1]) { L.rowLo[ty] = L.rowLo[ty - 1]; L.rowHi[ty] = L.rowHi[ty - 1]; }
			else Strip(L.bank, L.s27, L.s28, L.s29, L.rowScr[ty], L.rowLo[ty], L.rowHi[ty]);
		}
		// view placement: follow the strip the player is heading into
		int refscr = L.cam;
		if(alt >= 0) { for(int ty = 0; ty < 30; ty++) if(L.rowScr[ty] == alt) { refscr = alt; break; } }
		int lo, hi;
		Strip(L.bank, L.s27, L.s28, L.s29, refscr, lo, hi);
		int wxr = L.worldx + ((refscr - L.cam) << 8);
		int tgt = ViewD(wxr, lo, hi);
		L.hudT = tgt - (wxr - (lo << 8));
		if(L.hudT < 0) L.hudT = 0;
		if(L.hudT > tgt) L.hudT = tgt;
		if(!L.dInit) { L.D = tgt; L.dInit = true; L.hudC = L.hudT; }
		else if(L.D < tgt) { L.D += 4; if(L.D > tgt) L.D = tgt; }
		else if(L.D > tgt) { L.D -= 4; if(L.D < tgt) L.D = tgt; }
		if(L.hudC < L.hudT) { L.hudC += 4; if(L.hudC > L.hudT) L.hudC = L.hudT; }
		else if(L.hudC > L.hudT) { L.hudC -= 4; if(L.hudC < L.hudT) L.hudC = L.hudT; }
	}

	// background of one line of the side areas (sx = x relative to the 4:3 window). Fills codes/opaque.
	struct TileCache { int key = -1; uint8_t lo = 0, hi = 0; int pal = 0; int dim = 0; bool none = true; };
	void MarginPixel(int sx, TileCache& tc, uint8_t& code, bool& opaque)
	{
		int cy = (P.v >> 5) & 31, fy = (P.v >> 12) & 7;
		code = 0x80; opaque = false;
		if(!L.valid || cy >= 30) return;
		int base = L.rowScr[cy];
		int X = L.worldx + ((base - L.cam) << 8) + sx, dim = 0;
		int lo = L.rowLo[cy], hi = L.rowHi[cy];
		if(X < (lo << 8)) { if(!_fill) return; X = (lo << 8) + (X & 31); dim = (_fill == 1) ? 0x40 : 0; }
		else if(X >= ((hi + 1) << 8)) { if(!_fill) return; X = ((hi + 1) << 8) - 32 + (X & 31); dim = (_fill == 1) ? 0x40 : 0; }
		int key = X >> 3;
		if(tc.key != key || tc.dim != dim) {
			int pal = 0;
			int t = LvTile(X >> 8, (X >> 3) & 31, cy, pal);
			tc.key = key; tc.dim = dim; tc.none = t < 0;
			if(t >= 0) {
				uint16_t pa = (uint16_t)(P.bgBase + t * 16 + fy);
				tc.lo = Vram(pa); tc.hi = Vram((uint16_t)(pa + 8)); tc.pal = pal;
			}
		}
		if(tc.none) return;
		int bit = 7 - (X & 7);
		int px = ((tc.lo >> bit) & 1) | (((tc.hi >> bit) & 1) << 1);
		uint8_t mask = P.grey ? 0x30 : 0x3F;
		opaque = px != 0;
		code = (uint8_t)((P.pal[px ? ((tc.pal << 2) | px) : 0] & mask) | dim);
	}

	bool WindowBgOpaque(int i, uint16_t t, uint8_t fx)
	{
		int nty = (P.v >> 11) & 1, cy = (P.v >> 5) & 31, fy = (P.v >> 12) & 7;
		int x0 = (((t >> 10) & 1) << 8) + ((t & 31) << 3) + fx;
		int X = (x0 + i) & 511, cx = (X >> 3) & 31, nt = (X >> 8) | (nty << 1);
		uint8_t tile = Vram((uint16_t)(0x2000 | (nt << 10) | (cy << 5) | cx));
		uint16_t pa = (uint16_t)(P.bgBase + tile * 16 + fy);
		int bit = 7 - (X & 7);
		return (((Vram(pa) >> bit) & 1) | ((Vram((uint16_t)(pa + 8)) >> bit) & 1)) != 0;
	}

	void RenderLine(int y, uint16_t t, uint8_t fx)
	{
		int W = _frameW, M = (W - 256) / 2;
		int D = _frameLevel ? _frameD : M;
		uint8_t* codes = _codes[y];
		uint8_t* ovl = _ovl[y];
		static uint8_t opq[MegaWide::MaxWidth], taken[MegaWide::MaxWidth];
		memset(ovl, 0xFF, 256);
		memset(taken, 0, W);
		if(P.bgOn && _frameLevel) {
			TileCache tc;
			for(int i = 0; i < D; i++) { bool o; MarginPixel(i - D, tc, codes[i], o); opq[i] = o; }
			tc.key = -1;
			for(int i = D + 256; i < W; i++) { bool o; MarginPixel(i - D, tc, codes[i], o); opq[i] = o; }
		} else {
			memset(codes, 0x80, W);
			memset(opq, 0, W);
		}
		if(!P.sprOn) return;
		int h = P.large ? 16 : 8;
		uint8_t mask = P.grey ? 0x30 : 0x3F;
		for(int n = 0; n < 64; n++) {
			int sy = _oam[n * 4] + 1, tile = _oam[n * 4 + 1], at = _oam[n * 4 + 2];
			int row = y - sy;
			if(row < 0 || row >= h) continue;
			int sx = _oam[n * 4 + 3];
			bool extra = _ox[n] != 0;   // hidden from the normal PPU, drawn here in full
			if(_ox[n] == 0x40) sx += (_frameLevel ? L.hudC : M) - D; else sx += 256 * (int8_t)_ox[n];
			if(!extra && sx <= 248) continue;   // entirely inside the window: the PPU draws it
			if(at & 0x80) row = h - 1 - row;
			uint16_t pa;
			if(h == 16) pa = (uint16_t)(((tile & 1) << 12) + ((tile & 0xFE) + (row >> 3)) * 16 + (row & 7));
			else pa = (uint16_t)(P.sprBase + tile * 16 + row);
			uint8_t lo = Vram(pa), hi = Vram((uint16_t)(pa + 8));
			for(int k = 0; k < 8; k++) {
				int bit = (at & 0x40) ? k : 7 - k;
				int px = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
				int xw = sx + k, X = xw + D;
				if(!px || X < 0 || X >= W) continue;
				bool inWin = xw >= 0 && xw < 256;
				if(inWin && !extra) continue;
				if(taken[X]) continue;
				taken[X] = 1;
				uint8_t c = P.pal[0x10 | ((at & 3) << 2) | px] & mask;
				if(inWin) {
					if((at & 0x20) && P.bgOn && WindowBgOpaque(xw, t, fx)) continue;
					ovl[xw] = c;
				} else {
					if((at & 0x20) && opq[X]) continue;
					codes[X] = c;
				}
			}
		}
	}

	inline void Cmp(uint8_t& ps, uint8_t r, uint8_t v)
	{
		ps &= ~(PSFlags::Carry | PSFlags::Zero | PSFlags::Negative);
		if(r >= v) ps |= PSFlags::Carry;
		uint8_t d = (uint8_t)(r - v);
		if(!d) ps |= PSFlags::Zero;
		if(d & 0x80) ps |= PSFlags::Negative;
	}
	inline void SetNZ(uint8_t& ps, uint8_t v)
	{
		ps &= ~(PSFlags::Zero | PSFlags::Negative);
		if(!v) ps |= PSFlags::Zero;
		if(v & 0x80) ps |= PSFlags::Negative;
	}
	inline int ObjX(int i) { return (_ram[0x460 + (i & 0x1F)] << 8) | _ram[0x480 + (i & 0x1F)]; }
	inline int Scroll() { return (_ram[0x1B] << 8) | _ram[0x1A]; }
}

void MegaWide::OnLoad(NesConsole* console)
{
	_con = console;
	_mapper = console->GetMapper();
	_ram = console->GetMemoryManager()->GetInternalRam();
	_prg = _mapper->_prgRom;
	uint32_t size = _mapper->_prgSize;
	_supported = false; _hooksOk = false;
	L = Lv();
	memset(_oxBuf, 0, sizeof(_oxBuf)); memset(_ox, 0, sizeof(_ox)); memset(_oxPending, 0, sizeof(_oxPending));
	_oxFresh = false; _dmaHappened = false; _frameUsed = false;
	if(_prg && _ram && size >= 0x20000 && (size & (size - 1)) == 0 && (size % 0x4000) == 0) {
		_bankMask = size / 0x4000 - 1;
		uint8_t* fixed = _prg + ((size_t)_bankMask << 14);
		_supported = memcmp(fixed + (0xCE58 & 0x3FFF), _sig, sizeof(_sig)) == 0 && console->GetMapper()->GetRomInfo().MapperID == 2;
		if(_supported) {
			_hooksOk = true;
			for(int i = 0; i < PatchCount; i++) {
				const Patch& p = _patches[i];
				if(memcmp(_prg + ((size_t)(p.bank & _bankMask) << 14) + (p.addr & 0x3FFF), p.exp, p.len) != 0) { _hooksOk = false; break; }
			}
		}
	}
	UpdateHookTable();
}

void MegaWide::OnUnload(NesConsole* console)
{
	if(_con == console) {
		_con = nullptr; _mapper = nullptr; _ram = nullptr; _prg = nullptr;
		_supported = false; _hooksOk = false;
		HookAt = nullptr;
	}
}

bool MegaWide::IsGameSupported() { return _supported; }

void MegaWide::SetOptions(int margin, int fill, bool wideObjects)
{
	if(margin < 0) margin = 0;
	if(margin > (MaxWidth - 256) / 2) margin = (MaxWidth - 256) / 2;
	if(margin != _margin) L.dInit = false;
	_margin = margin; _fill = fill; _wideObjects = wideObjects;
	UpdateHookTable();
}

int MegaWide::GetWidth() { return _supported ? CurW() : 256; }

void MegaWide::OnOamDma(uint8_t page)
{
	if(!_supported) return;
	if(HookAt && page == 2 && _oxFresh) memcpy(_oxPending, _oxBuf, 64); else memset(_oxPending, 0, 64);
	_oxFresh = false;
	_dmaHappened = true;
}

bool MegaWide::Hook(NesCpu* cpu)
{
	NesCpuState& s = cpu->_state;
	const Patch& p = _patches[HookAt[s.PC] - 1];
	// make sure the expected routine is really mapped here (banked code)
	if(p.bank != 7) {
		for(int k = 0; k < p.len; k++) if(_mapper->DebugReadRam((uint16_t)(p.addr + k)) != p.exp[k]) return false;
	}
	int l, r, x;
	uint16_t next = (uint16_t)(p.addr + p.len);
	switch(p.id) {
		case 0x01: LogicBounds(l, r); _ram[4] = (uint8_t)r; _ram[5] = (uint8_t)(r >> 8); break;
		case 0x02: LogicBounds(l, r); Cmp(s.PS, s.A, (uint8_t)(l >> 8)); break;
		case 0x03: LogicBounds(l, r); Cmp(s.PS, s.A, (uint8_t)l); break;
		case 0x05: LogicBounds(l, r); Cmp(s.PS, s.A, (uint8_t)(r >> 8)); break;
		case 0x06: LogicBounds(l, r); Cmp(s.PS, s.A, (uint8_t)r); break;
		case 0x07: {
			uint16_t ptr = (uint16_t)(_ram[6] | (_ram[7] << 8));
			s.A = Rd((uint16_t)(ptr + s.Y)); _ram[0] = s.A; _ram[5] = Rd((uint16_t)(ptr + s.Y - 1)); SetNZ(s.PS, s.A);
			break;
		}
		case 0x08: LogicBounds(l, r); x = ObjX(s.X);
			if(x >= l && x <= r) { s.A = 0; SetNZ(s.PS, 0); } else { s.A = 1; SetNZ(s.PS, 1); }
			break;
		case 0x09: LogicBounds(l, r); x = ObjX(s.X);
			next = (x >= l + 0x10 && x <= r - 8) ? 0xB95F : 0xB963;
			break;
		case 0x0a: x = ObjX(s.X) - Scroll(); _rel16 = (int16_t)x; s.A = (uint8_t)x; break;
		case 0x0b:
			x = _rel16 + (int8_t)s.A;
			if(x < -255 || x > 510 || (!_margin && (x < 0 || x > 255))) { next = 0xD269; break; }
			_ram[(0x207 + s.X) & 0x7FF] = (uint8_t)x; _oxBuf[((0x207 + s.X) >> 2) & 63] = (uint8_t)(x >> 8); next = 0xD293;
			break;
		case 0x0c: _colX = ObjX(s.X); _colW = _ram[1];
			x = ObjX(s.X) - Scroll(); _ram[0] = (uint8_t)(x - _colW); _ram[1] = (uint8_t)(x + _colW); s.A = _ram[1];
			break;
		case 0x0d: _colX = ObjX(0); _colW = 7;
			x = ObjX(0) - Scroll(); _ram[0] = (uint8_t)(x - 7); _ram[1] = (uint8_t)(x + 7); s.A = _ram[1];
			break;
		case 0x0e: {
			int d = ObjX(s.X) - _colX, w = _colW + _ram[0x0E];
			if(d < 0) d = -d;
			_ram[0x0C] = (uint8_t)(ObjX(s.X) - Scroll());
			next = (d <= w) ? 0xCA29 : 0xCA2B;
			break;
		}
		case 0x0f: _ram[(0x207 + s.Y) & 0x7FF] = s.A; _oxBuf[((0x207 + s.Y) >> 2) & 63] = _margin ? 0x40 : 0; break;
		case 0x10: LogicBounds(l, r); x = r - 7; _ram[0x480 + (s.X & 0x1F)] = (uint8_t)x; _ram[0x460 + (s.X & 0x1F)] = (uint8_t)(x >> 8); s.A = (uint8_t)(x >> 8); break;
		case 0x11: {
			int off = _ram[0];
			LogicBounds(l, r); x = Scroll() + off;
			if(off < 0x10) x = l + off; else if(off >= 0xF0) x = r - (255 - off);
			_ram[0] = (uint8_t)x; _ram[5] = (uint8_t)(x >> 8); s.A = (uint8_t)(x >> 8);
			break;
		}
		case 0x12: if(!(_ram[0x8D] & 1)) next = 0xD89A; else if(_margin) next = 0xD8BF; break;
		case 0x13: next = _margin ? 0xD921 : 0xC3B3; break;
		case 0x14: memset(_oxBuf, 0, 64); _oxFresh = true; return false;   // then run the original instruction
		default: return false;
	}
	s.PC = next;
	return true;
}

void MegaWide::OnScanline(BaseNesPpu* ppu)
{
	int y = ppu->_scanline;
	if(y >= 240 || !_supported) return;
	if(y == -1) {
		// take this frame's sprite table; hide from the normal PPU the sprites we draw ourselves
		if(_dmaHappened) {
			memcpy(_oam, ppu->_spriteRam, 256);
			memcpy(_ox, _oxPending, 64);
			_dmaHappened = false;
			if(_margin) { for(int i = 0; i < 64; i++) if(_ox[i]) ppu->_spriteRam[i * 4] = 0xF0; }
			else memset(_ox, 0, 64);
		}
		return;
	}
	P.v = ppu->_videoRamAddr;
	P.bgBase = ppu->_control.BackgroundPatternAddr; P.sprBase = ppu->_control.SpritePatternAddr; P.large = ppu->_control.LargeSprites;
	P.bgOn = ppu->_mask.BackgroundEnabled; P.sprOn = ppu->_mask.SpritesEnabled; P.grey = ppu->_mask.Grayscale;
	memcpy(P.pal, ppu->_paletteRam, 32);
	for(int i = 0x10; i < 0x20; i += 4) P.pal[i] = P.pal[i & 0x0F];
	uint16_t t = ppu->_tmpVideoRamAddr; uint8_t fx = ppu->_xScroll;
	if(y == 0) {
		_frameW = CurW();
		Latch(t, fx);
		_frameLevel = L.valid && _margin > 0;
		_frameD = L.D;
		_frameUsed = _margin > 0;
	}
	if(!_frameUsed) return;
	RenderLine(y, t, fx);
}

bool MegaWide::Compose(const uint32_t* src, uint32_t srcWidth, uint32_t srcHeight, const uint32_t* palette, std::vector<uint32_t>& out, uint32_t& outWidth)
{
	if(!_supported || !_frameUsed || srcWidth != 256 || srcHeight != 240) return false;
	int W = _frameW, M = (W - 256) / 2, D = _frameLevel ? _frameD : M;
	out.resize((size_t)W * 240);
	for(int y = 0; y < 240; y++) {
		uint32_t* o = out.data() + (size_t)y * W;
		const uint8_t* codes = _codes[y];
		for(int i = 0; i < W; i++) {
			if(i >= D && i < D + 256) continue;
			uint8_t c = codes[i];
			if(c & 0x80) o[i] = 0xFF000000;
			else if(c & 0x40) o[i] = 0xFF000000 | ((palette[c & 0x3F] >> 2) & 0x3F3F3F);
			else o[i] = 0xFF000000 | palette[c];
		}
		memcpy(o + D, src + (size_t)y * 256, 256 * sizeof(uint32_t));
		{
			const uint8_t* ovl = _ovl[y];
			for(int i = 0; i < 256; i++) if(ovl[i] != 0xFF) o[D + i] = 0xFF000000 | palette[ovl[i] & 0x3F];
		}
	}
	if(!_frameLevel) {
		// menus / cutscenes: if the picture's side edges are flat colour on every line, continue them outward
		for(int side = 0; side < 2; side++) {
			int x0 = side ? M + 248 : M, ok = 0;
			static uint32_t col[240]; static uint8_t uni[240];
			for(int y = 0; y < 240; y++) {
				const uint32_t* p = out.data() + (size_t)y * W + x0;
				bool u = true;
				for(int i = 1; i < 8; i++) if(p[i] != p[0]) { u = false; break; }
				uni[y] = u; col[y] = p[0]; ok += u;
			}
			if(ok < 240 * 9 / 10) continue;
			uint32_t last = 0xFF000000;
			for(int y = 0; y < 240; y++) if(uni[y]) { last = col[y]; break; }
			for(int y = 0; y < 240; y++) {
				uint32_t* p = out.data() + (size_t)y * W;
				if(uni[y]) last = col[y];
				if(side) { for(int i = M + 256; i < W; i++) p[i] = last; } else { for(int i = 0; i < M; i++) p[i] = last; }
			}
		}
	}
	outWidth = (uint32_t)W;
	return true;
}
