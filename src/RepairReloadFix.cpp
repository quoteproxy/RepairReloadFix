// RepairReloadFix - Kingdom Come: Deliverance II
//
// Problem: repair kits refill an item up to the health cap of its current quality. For a quality 1 item of a
// four-quality class that cap is 0.1 (stored as the float 0.100000001). After a save and a load the same item
// comes back with 0.099999994, one float step below the cap. The inventory still shows 100 %, but the repair
// kit compares "cap > health" and offers the item again, after every load.
//
// Shops do not use the repair-kit check: they list an item while its condition (health mapped from the quality's
// range [low, cap] to 0..1) is below 1, and 0.099999994 maps to 0.99999994.
//
// "Just below the cap" means less than fTolerance (default 0.0000001 of the 0..1 health range) below it. A load
// leaves the value one float step below the cap (about 0.0000000075 at 0.1). The default is kept this small so that
// real wear, even a single small step of it, is never taken back.
//
// Fix:
//   1. Item manager, condition from health (shops, item lists, condition display): a health just below the cap
//      is mapped as the cap, i.e. 100 %.
//   2. Item manager, health from condition (the inverse): a result just below the cap is returned as the cap.
//   3. The cap of an item and the condition of an item also put the item's own health back to the cap when it is
//      just below it (bSnapToCap), so checks on the raw health (repair prices and the like) agree.
//   4. The two repair-kit checks shared by all kit types (weapon, armor, clothes, shoes, bow, gun) treat an item
//      just below the cap as fully repaired:
//        - the check that decides whether an item can be repaired with the kit (cap > health)
//        - the check that reports an item as fully repaired (health >= cap)
// How much a repair restores, prices of damaged items and quality are not changed. bLogCallers=1 writes each
// distinct caller of the hooked functions to the log (diagnostics).
//
// Loading: WHGame.dll imports functions from rstrtmgr.dll (Restart Manager) and dinput8.dll, and Windows looks for
// both in the game's folder first. This file is named rstrtmgr.dll and placed next to WHGame.dll; dinput8.dll is
// used by other mods (ASI loaders), rstrtmgr.dll is not. The same file also works when renamed to dinput8.dll.
// Every exported function of both DLLs is passed on to the system DLL (RmStubs.asm for the Restart Manager).
//
// Every place is found by byte pattern in WHGame.dll. If a pattern is not found exactly once, that place is not
// patched and the reason is logged (RepairReloadFix.log next to this DLL).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	FILE* g_log = nullptr;
	HMODULE g_self = nullptr;
	float g_tolerance = 0.0000001f;
	bool g_snapToCap = true;
	bool g_logCallers = false;
	std::uint8_t* g_gameBase = nullptr;

	void Log(const char* fmt, ...)
	{
		if (!g_log) {
			return;
		}
		va_list args;
		va_start(args, fmt);
		std::vfprintf(g_log, fmt, args);
		va_end(args);
		std::fputc('\n', g_log);
		std::fflush(g_log);
	}

	std::wstring SelfDir()
	{
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(g_self, path, MAX_PATH);
		std::wstring s(path);
		const auto slash = s.find_last_of(L"\\/");
		return slash == std::wstring::npos ? L"." : s.substr(0, slash);
	}

	void OpenLog()
	{
		const std::wstring path = SelfDir() + L"\\RepairReloadFix.log";
		g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
	}

	void ReadConfig(bool& enabled)
	{
		const std::wstring ini = SelfDir() + L"\\RepairReloadFix.ini";
		enabled = GetPrivateProfileIntW(L"General", L"bEnabled", 1, ini.c_str()) != 0;
		g_snapToCap = GetPrivateProfileIntW(L"General", L"bSnapToCap", 1, ini.c_str()) != 0;
		g_logCallers = GetPrivateProfileIntW(L"General", L"bLogCallers", 0, ini.c_str()) != 0;
		wchar_t buf[64]{};
		GetPrivateProfileStringW(L"General", L"fTolerance", L"", buf, 64, ini.c_str());
		if (buf[0]) {
			const float v = static_cast<float>(_wtof(buf));
			if (v >= 0.0f && v <= 0.01f) {
				g_tolerance = v;
			} else {
				Log("fTolerance %ls is out of range (0 - 0.01); using %g", buf, g_tolerance);
			}
		}
	}

	// ---------------------------------------------------------------- pattern scan in the loaded game DLL
	struct Pattern
	{
		std::vector<std::uint8_t> bytes;
		std::vector<bool> mask;
	};

	Pattern ParsePattern(const char* text)
	{
		Pattern p;
		for (const char* s = text; *s;) {
			while (*s == ' ') {
				++s;
			}
			if (!*s) {
				break;
			}
			if (s[0] == '?') {
				p.bytes.push_back(0);
				p.mask.push_back(false);
				s += (s[1] == '?') ? 2 : 1;
			} else {
				p.bytes.push_back(static_cast<std::uint8_t>(std::strtoul(std::string(s, 2).c_str(), nullptr, 16)));
				p.mask.push_back(true);
				s += 2;
			}
		}
		return p;
	}

	bool TextSection(HMODULE module, std::uint8_t*& begin, std::size_t& size)
	{
		auto* base = reinterpret_cast<std::uint8_t*>(module);
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		auto* sec = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
			if (std::memcmp(sec->Name, ".text", 6) == 0) {
				begin = base + sec->VirtualAddress;
				size = sec->Misc.VirtualSize;
				return true;
			}
		}
		return false;
	}

	std::vector<std::uint8_t*> FindAll(HMODULE module, const Pattern& p)
	{
		std::vector<std::uint8_t*> hits;
		std::uint8_t* text = nullptr;
		std::size_t size = 0;
		if (!TextSection(module, text, size) || size < p.bytes.size()) {
			return hits;
		}
		const std::size_t n = p.bytes.size();
		const std::uint8_t first = p.bytes[0];
		for (std::size_t i = 0; i + n <= size; ++i) {
			if (text[i] != first) {
				continue;
			}
			std::size_t k = 1;
			while (k < n && (!p.mask[k] || text[i + k] == p.bytes[k])) {
				++k;
			}
			if (k == n) {
				hits.push_back(text + i);
				if (hits.size() > 1) {
					break;  // only "exactly once" matters
				}
			}
		}
		return hits;
	}

	std::uint8_t* RelTarget(std::uint8_t* rel32At)
	{
		std::int32_t rel = 0;
		std::memcpy(&rel, rel32At, 4);
		return rel32At + 4 + rel;
	}

	// ---------------------------------------------------------------- code stubs near the game DLL
	std::uint8_t* AllocNear(std::uint8_t* target, std::size_t size)
	{
		SYSTEM_INFO si{};
		GetSystemInfo(&si);
		const auto gran = static_cast<std::uintptr_t>(si.dwAllocationGranularity);
		const auto t = reinterpret_cast<std::uintptr_t>(target);
		for (std::uintptr_t d = gran; d < 0x70000000u; d += gran) {
			for (const std::uintptr_t a : { (t - d) & ~(gran - 1), (t + d) & ~(gran - 1) }) {
				void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (p) {
					return static_cast<std::uint8_t*>(p);
				}
			}
		}
		return nullptr;
	}

	struct Emitter
	{
		std::uint8_t* p;
		void bytes(std::initializer_list<std::uint8_t> b)
		{
			for (auto x : b) {
				*p++ = x;
			}
		}
		void rel32To(const std::uint8_t* target)  // writes rel32 relative to the end of the 4 bytes
		{
			const auto rel = static_cast<std::int32_t>(target - (p + 4));
			std::memcpy(p, &rel, 4);
			p += 4;
		}
		void jmpAbs(const void* target)  // FF 25 00000000 <addr64>
		{
			bytes({ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 });
			const auto a = reinterpret_cast<std::uint64_t>(target);
			std::memcpy(p, &a, 8);
			p += 8;
		}
	};

	bool WriteJump(std::uint8_t* site, std::size_t length, const std::uint8_t* stub)
	{
		DWORD old = 0;
		if (!VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &old)) {
			return false;
		}
		site[0] = 0xE9;
		const auto rel = static_cast<std::int32_t>(stub - (site + 5));
		std::memcpy(site + 1, &rel, 4);
		for (std::size_t i = 5; i < length; ++i) {
			site[i] = 0x90;
		}
		VirtualProtect(site, length, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, length);
		return true;
	}

	// Check "can be repaired": original
	//   call cap ; comiss xmm0, [rbx+54h] ; jbe notRepairable
	// stub
	//   subss xmm0, [tol] ; comiss xmm0, [rbx+54h] ; jbe notRepairable ; jmp continue
	bool PatchCanRepair(HMODULE game, std::uint8_t* stubs, float* tol)
	{
		const auto hits = FindAll(game, ParsePattern("48 8B 5E 08 48 8B CB E8 ?? ?? ?? ?? 0F 2F 43 54 0F 86 ?? ?? ?? ??"));
		if (hits.size() != 1) {
			Log("repair-kit check: pattern found %zu times, not patched", hits.size());
			return false;
		}
		std::uint8_t* site = hits[0] + 12;              // comiss xmm0, [rbx+54h]
		std::uint8_t* notRepairable = RelTarget(site + 6);
		std::uint8_t* cont = site + 10;

		Emitter e{ stubs };
		e.bytes({ 0xF3, 0x0F, 0x5C, 0x05 });               // subss xmm0, [rip+tol]
		e.rel32To(reinterpret_cast<std::uint8_t*>(tol));
		e.bytes({ 0x0F, 0x2F, 0x43, 0x54 });               // comiss xmm0, [rbx+54h]
		e.bytes({ 0x76, 0x0E });                           // jbe +14 (over the next jmp)
		e.jmpAbs(cont);                                    // repairable: continue with the skill check
		e.jmpAbs(notRepairable);                           // full: same path as before
		FlushInstructionCache(GetCurrentProcess(), stubs, static_cast<SIZE_T>(e.p - stubs));

		if (!WriteJump(site, 10, stubs)) {
			Log("repair-kit check: VirtualProtect failed, not patched");
			return false;
		}
		Log("patched repair-kit check at WHGame.dll+%llX", static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game)));
		return true;
	}

	// Check "fully repaired": original
	//   call cap ; comiss xmm6, xmm0 ; movaps xmm6, [rsp+20h] ; setae al
	// stub
	//   subss xmm0, [tol] ; comiss xmm6, xmm0 ; movaps xmm6, [rsp+20h] ; setae al ; jmp continue
	bool PatchIsFull(HMODULE game, std::uint8_t* stubs, float* tol)
	{
		const auto hits = FindAll(game, ParsePattern("48 8B 4B 08 F3 0F 10 71 54 E8 ?? ?? ?? ?? 0F 2F F0 0F 28 74 24 20 0F 93 C0"));
		if (hits.size() != 1) {
			Log("fully-repaired check: pattern found %zu times, not patched", hits.size());
			return false;
		}
		std::uint8_t* site = hits[0] + 14;              // comiss xmm6, xmm0
		std::uint8_t* cont = site + 11;

		Emitter e{ stubs };
		e.bytes({ 0xF3, 0x0F, 0x5C, 0x05 });               // subss xmm0, [rip+tol]
		e.rel32To(reinterpret_cast<std::uint8_t*>(tol));
		e.bytes({ 0x0F, 0x2F, 0xF0 });                     // comiss xmm6, xmm0
		e.bytes({ 0x0F, 0x28, 0x74, 0x24, 0x20 });         // movaps xmm6, [rsp+20h]
		e.bytes({ 0x0F, 0x93, 0xC0 });                     // setae al
		e.jmpAbs(cont);
		FlushInstructionCache(GetCurrentProcess(), stubs, static_cast<SIZE_T>(e.p - stubs));

		if (!WriteJump(site, 11, stubs)) {
			Log("fully-repaired check: VirtualProtect failed, not patched");
			return false;
		}
		Log("patched fully-repaired check at WHGame.dll+%llX", static_cast<unsigned long long>(site - reinterpret_cast<std::uint8_t*>(game)));
		return true;
	}

	// ---------------------------------------------------------------- function entry hooks
	// Each hooked function starts with position-independent register saves (mov rax,rsp / mov [rax+x],reg /
	// push / sub rsp). Those first `stolen` bytes are copied to a trampoline that then jumps back; the function
	// start becomes a 14-byte absolute jump to the hook (plus NOP padding).
	std::uint8_t* HookEntry(HMODULE game, const char* name, const char* pattern, std::size_t stolen, const void* hook,
		std::uint8_t* tramp)
	{
		const auto hits = FindAll(game, ParsePattern(pattern));
		if (hits.size() != 1) {
			Log("%s: pattern found %zu times, not hooked", name, hits.size());
			return nullptr;
		}
		std::uint8_t* fn = hits[0];
		std::memcpy(tramp, fn, stolen);
		Emitter e{ tramp + stolen };
		e.jmpAbs(fn + stolen);
		FlushInstructionCache(GetCurrentProcess(), tramp, stolen + 14);

		DWORD old = 0;
		if (!VirtualProtect(fn, stolen, PAGE_EXECUTE_READWRITE, &old)) {
			Log("%s: VirtualProtect failed, not hooked", name);
			return nullptr;
		}
		Emitter p{ fn };
		p.jmpAbs(hook);  // 14 bytes
		while (p.p < fn + stolen) {
			*p.p++ = 0x90;
		}
		VirtualProtect(fn, stolen, old, &old);
		FlushInstructionCache(GetCurrentProcess(), fn, stolen);
		Log("hooked %s at WHGame.dll+%llX", name, static_cast<unsigned long long>(fn - g_gameBase));
		return tramp;
	}

	// ---------------------------------------------------------------- diagnostics
	enum HookKind { kCap, kCond, kItemCond, kHealthFromCond, kKinds };
	const char* const kKindName[kKinds] = { "cap", "condition", "item condition", "health from condition" };
	std::mutex g_callerMutex;
	void* volatile g_callers[kKinds][128]{};
	volatile long g_callerCount[kKinds]{};
	volatile long g_snapCount[kKinds]{};

	unsigned long long Rva(void* p)
	{
		return static_cast<unsigned long long>(static_cast<std::uint8_t*>(p) - g_gameBase);
	}

	bool KnownCaller(HookKind kind, void* ret)
	{
		const long n = g_callerCount[kind];  // entries are only appended, and written before the count
		for (long i = 0; i < n; ++i) {
			if (g_callers[kind][i] == ret) {
				return true;
			}
		}
		return false;
	}

	void Note(HookKind kind, void* ret, bool snapped, float before, float after)
	{
		long n = 0;
		if (snapped) {
			n = InterlockedIncrement(&g_snapCount[kind]);
		}
		const bool logSnap = snapped && n <= 20;
		const bool newCaller = g_logCallers && !KnownCaller(kind, ret);
		if (!newCaller && !logSnap) {
			return;
		}
		std::lock_guard<std::mutex> lock(g_callerMutex);
		if (newCaller && !KnownCaller(kind, ret) && g_callerCount[kind] < 128) {
			g_callers[kind][g_callerCount[kind]] = ret;
			_WriteBarrier();
			g_callerCount[kind] = g_callerCount[kind] + 1;
			Log("%s called from WHGame.dll+%llX", kKindName[kind], Rva(ret));
		}
		if (logSnap) {
			Log("  %s: %.9f -> %.9f (caller WHGame.dll+%llX)%s", kKindName[kind], before, after, Rva(ret),
				n == 20 ? " (further snaps of this kind are not logged)" : "");
		}
	}

	bool JustBelow(float value, float cap)
	{
		return value < cap && value >= cap - g_tolerance;
	}

	// ---------------------------------------------------------------- cap of an item (WHGame.dll+469988)
	// float cap(Item*): the health cap of the item's current quality. Also puts the item's health back to the cap
	// when it is just below it.
	using CapFn = float(__fastcall*)(std::uint8_t* item);
	CapFn g_origCap = nullptr;

	float SnapItem(std::uint8_t* item, float cap)
	{
		float* health = reinterpret_cast<float*>(item + 0x54);
		const float before = *health;
		if (JustBelow(before, cap)) {
			*health = cap;
		}
		return before;
	}

	float __fastcall CapHook(std::uint8_t* item)
	{
		const float cap = g_origCap(item);
		bool snapped = false;
		float before = 0.0f;
		if (g_snapToCap && item) {
			before = SnapItem(item, cap);
			snapped = JustBelow(before, cap);
		}
		Note(kCap, _ReturnAddress(), snapped, before, cap);
		return cap;
	}

	// ---------------------------------------------------------------- item manager: condition from health
	// (WHGame.dll+466DB0, vtable slot +0x68) float condition(mgr, itemClass, health, quality): maps health from the
	// quality's range [low, cap] to 0..1. Used by shops, item lists and the condition display. A health just below
	// the cap is mapped as the cap, i.e. 100 %.
	using CondFn = float(__fastcall*)(void* mgr, void* itemClass, float value, unsigned quality);
	using MgrCapFn = float(__fastcall*)(void* mgr, void* itemClass, unsigned quality);
	CondFn g_origCond = nullptr;
	CondFn g_origHealthFromCond = nullptr;

	float MgrCap(void* mgr, void* itemClass, unsigned quality)
	{
		auto fn = (*reinterpret_cast<MgrCapFn**>(mgr))[0x40 / 8];  // slot +0x40: cap of a quality
		return fn(mgr, itemClass, quality);
	}

	float __fastcall CondHook(void* mgr, void* itemClass, float health, unsigned quality)
	{
		const float cap = MgrCap(mgr, itemClass, quality);
		const bool snapped = JustBelow(health, cap);
		Note(kCond, _ReturnAddress(), snapped, health, cap);
		return g_origCond(mgr, itemClass, snapped ? cap : health, quality);
	}

	// ---------------------------------------------------------------- item manager: health from condition
	// (WHGame.dll+96EE58) float health(mgr, itemClass, condition, quality): the inverse mapping. A result just below
	// the cap is returned as the cap.
	float __fastcall HealthFromCondHook(void* mgr, void* itemClass, float condition, unsigned quality)
	{
		const float health = g_origHealthFromCond(mgr, itemClass, condition, quality);
		const float cap = MgrCap(mgr, itemClass, quality);
		const bool snapped = JustBelow(health, cap);
		Note(kHealthFromCond, _ReturnAddress(), snapped, health, cap);
		return snapped ? cap : health;
	}

	// ---------------------------------------------------------------- item manager: condition of an item
	// (WHGame.dll+465CC0, vtable slot +0x70) float condition(mgr, Item*). Puts the item's health back to the cap
	// first when it is just below it, so later checks on the raw health (repair prices and the like) agree.
	using ItemCondFn = float(__fastcall*)(void* mgr, std::uint8_t* item);
	ItemCondFn g_origItemCond = nullptr;

	float __fastcall ItemCondHook(void* mgr, std::uint8_t* item)
	{
		bool snapped = false;
		float before = 0.0f;
		float cap = 0.0f;
		if (g_snapToCap && item && g_origCap) {
			cap = g_origCap(item);
			before = SnapItem(item, cap);
			snapped = JustBelow(before, cap);
		}
		Note(kItemCond, _ReturnAddress(), snapped, before, cap);
		return g_origItemCond(mgr, item);
	}

	bool InstallHooks(HMODULE game, std::uint8_t* mem)
	{
		int ok = 0;
		if (auto t = HookEntry(game, "cap function",
				"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
				"48 8B CF 4C 8B F0 48 8B 10 48 8B 72 40 E8",
				15, reinterpret_cast<const void*>(&CapHook), mem)) {
			g_origCap = reinterpret_cast<CapFn>(t);
			++ok;
		}
		if (auto t = HookEntry(game, "condition from health",
				"48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 50 0F 29 70 E8 45 8B C1 0F 29 78 D8 41 8B F1 48 8B 01 "
				"0F 28 FA 48 8B DA 48 8B F9 FF 50 38 48 8B 07 44 8B C6 48 8B D3 48 8B CF 0F 28 F0 FF 50 40 F3 0F 10 0D",
				16, reinterpret_cast<const void*>(&CondHook), mem + 0x40)) {
			g_origCond = reinterpret_cast<CondFn>(t);
			++ok;
		}
		if (auto t = HookEntry(game, "health from condition",
				"48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 50 0F 29 70 E8 45 8B C1 0F 29 78 D8 41 8B F1 48 8B 01 "
				"0F 28 FA 48 8B DA 48 8B F9 FF 50 38 48 8B 07 44 8B C6 48 8B D3 48 8B CF 0F 28 F0 FF 50 40 F3 0F 10 15",
				16, reinterpret_cast<const void*>(&HealthFromCondHook), mem + 0x80)) {
			g_origHealthFromCond = reinterpret_cast<CondFn>(t);
			++ok;
		}
		if (auto t = HookEntry(game, "item condition",
				"48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 41 56 48 83 EC 30 48 8B F2 0F 29 70 E8 4C 8B F1 "
				"BA 1A 00 00 00 48 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 49 8B 06 48 8B CE 48 8B 78 68",
				15, reinterpret_cast<const void*>(&ItemCondHook), mem + 0xC0)) {
			g_origItemCond = reinterpret_cast<ItemCondFn>(t);
			++ok;
		}
		Log("function hooks: %d of 4 installed (snap to cap %s, caller log %s)", ok, g_snapToCap ? "on" : "off",
			g_logCallers ? "on" : "off");
		return ok == 4;
	}

	void Install()
	{
		bool enabled = true;
		ReadConfig(enabled);
		if (!enabled) {
			Log("bEnabled=0: nothing patched");
			return;
		}
		HMODULE game = GetModuleHandleW(L"WHGame.dll");
		if (!game) {
			Log("WHGame.dll is not loaded (%s); nothing patched",
				GetModuleHandleW(L"WHGameArm.dll") ? "WHGameArm.dll is used, which this version does not support" : "unknown host");
			return;
		}
		std::uint8_t* mem = AllocNear(reinterpret_cast<std::uint8_t*>(game), 0x1000);
		if (!mem) {
			Log("could not allocate memory near WHGame.dll; nothing patched");
			return;
		}
		g_gameBase = reinterpret_cast<std::uint8_t*>(game);
		auto* tol = reinterpret_cast<float*>(mem);       // data at the start, code after it
		*tol = g_tolerance;
		const bool a = PatchCanRepair(game, mem + 0x100, tol);
		const bool b = PatchIsFull(game, mem + 0x200, tol);
		const bool c = InstallHooks(game, mem + 0x300);
		Log("tolerance %g; repair-kit checks: %s; function hooks: %s", g_tolerance,
			(a && b) ? "both patched" : (a || b) ? "only one patched" : "not patched", c ? "all installed" : "not all installed");
	}

	// ---------------------------------------------------------------- pass-through to the system DLLs
	// The same file works as rstrtmgr.dll (the default) and as dinput8.dll. Both export sets are present; each
	// function is passed on to the system DLL it belongs to, which is loaded on its first call.
	FARPROC SystemFunction(HMODULE& module, const wchar_t* file, const char* name)
	{
		if (!module) {
			wchar_t sys[MAX_PATH]{};
			GetSystemDirectoryW(sys, MAX_PATH);
			module = LoadLibraryW((std::wstring(sys) + L"\\" + file).c_str());
			if (!module) {
				Log("could not load the system %ls (error %lu)", file, GetLastError());
				return nullptr;
			}
		}
		const FARPROC fn = GetProcAddress(module, name);
		if (!fn) {
			Log("the system %ls has no %s", file, name);
		}
		return fn;
	}

	HMODULE g_dinput8 = nullptr;

	FARPROC Real(const char* name)
	{
		return SystemFunction(g_dinput8, L"dinput8.dll", name);
	}

	HMODULE g_rstrtmgr = nullptr;
	// The order must match the stubs in RmStubs.asm.
	const char* const kRmNames[] = { "RmAddFilter", "RmCancelCurrentTask", "RmEndSession", "RmGetFilterList",
		"RmGetList", "RmJoinSession", "RmRegisterResources", "RmRemoveFilter", "RmReserveHeap", "RmRestart",
		"RmShutdown", "RmStartSession" };
	constexpr unsigned kRmCount = sizeof(kRmNames) / sizeof(kRmNames[0]);
	FARPROC volatile g_rmFns[kRmCount]{};

	DWORD WINAPI RmMissing()
	{
		return ERROR_PROC_NOT_FOUND;
	}

	// One instance per process does the patching: a second copy under the other name (rstrtmgr.dll and dinput8.dll
	// both present) only passes calls on.
	bool FirstInstance()
	{
		wchar_t name[64]{};
		swprintf_s(name, L"Local\\RepairReloadFix.%lu", GetCurrentProcessId());
		const HANDLE mutex = CreateMutexW(nullptr, FALSE, name);  // kept open for the life of the process
		return mutex && GetLastError() != ERROR_ALREADY_EXISTS;
	}
}

extern "C"
{
	// Called by the stubs in RmStubs.asm with the function's index; returns the function to jump to.
	FARPROC ResolveRm(unsigned index)
	{
		if (index >= kRmCount) {
			return reinterpret_cast<FARPROC>(&RmMissing);
		}
		FARPROC fn = g_rmFns[index];
		if (!fn) {
			fn = SystemFunction(g_rstrtmgr, L"rstrtmgr.dll", kRmNames[index]);
			if (!fn) {
				fn = reinterpret_cast<FARPROC>(&RmMissing);
			}
			g_rmFns[index] = fn;
		}
		return fn;
	}

	HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, void* outer)
	{
		using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
		static auto fn = reinterpret_cast<Fn>(Real("DirectInput8Create"));
		return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
	}

	HRESULT WINAPI Proxy_DllCanUnloadNow()
	{
		using Fn = HRESULT(WINAPI*)();
		static auto fn = reinterpret_cast<Fn>(Real("DllCanUnloadNow"));
		return fn ? fn() : S_FALSE;
	}

	HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
	{
		using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
		static auto fn = reinterpret_cast<Fn>(Real("DllGetClassObject"));
		return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
	}

	HRESULT WINAPI Proxy_DllRegisterServer()
	{
		using Fn = HRESULT(WINAPI*)();
		static auto fn = reinterpret_cast<Fn>(Real("DllRegisterServer"));
		return fn ? fn() : E_FAIL;
	}

	HRESULT WINAPI Proxy_DllUnregisterServer()
	{
		using Fn = HRESULT(WINAPI*)();
		static auto fn = reinterpret_cast<Fn>(Real("DllUnregisterServer"));
		return fn ? fn() : E_FAIL;
	}
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = inst;
		DisableThreadLibraryCalls(inst);
		if (!FirstInstance()) {
			return TRUE;  // another copy is already loaded and patching; this one only passes calls on
		}
		OpenLog();
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(inst, path, MAX_PATH);
		const wchar_t* file = wcsrchr(path, L'\\');
		Log("RepairReloadFix 1.0.0 (loaded as %ls)", file ? file + 1 : path);
		Install();
	}
	return TRUE;
}
