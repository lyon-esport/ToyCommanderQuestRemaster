/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "types.h"
#include "addrspace.h"
#include "hw/aica/aica_if.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/elan.h"
#include "rend/TexCache.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include <algorithm>
#include <cstring>

namespace memwatch
{

struct Page
{
	std::unique_ptr<u8[]> data = std::make_unique<u8[]>(PAGE_SIZE);
};
using PageMap = std::unordered_map<u32, Page>;

template<typename T>
class Watcher
{
	bool started;
	PageMap pages;

#ifdef __EMSCRIPTEN__
	// tcvr: the browser build has no memory protection (no mprotect, no fault handler), so
	// nothing would tell which pages the game wrote and a rollback restored none of them.
	// Instead a copy of the memory is kept as it was at the last save, and at each save the
	// pages that differ go into the delta with their old content, as hit() does natively.
	std::vector<u8> shadow;

	void snapshot()
	{
		const u32 size = static_cast<T&>(*this).memSize();
		const u8 *mem = size != 0 ? (const u8 *)static_cast<T&>(*this).getMemPage(0) : nullptr;
		shadow.assign(mem, mem + size);
	}

	void scan()
	{
		const u32 size = std::min<u32>(static_cast<T&>(*this).memSize(), (u32)shadow.size()) & ~PAGE_MASK;
		for (u32 offset = 0; offset < size; offset += PAGE_SIZE)
		{
			const u8 *now = (const u8 *)static_cast<T&>(*this).getMemPage(offset);
			u8 *old = &shadow[offset];
			if (memcmp(now, old, PAGE_SIZE) == 0)
				continue;
			auto rv = pages.try_emplace(offset);
			if (rv.second)
				memcpy(&rv.first->second.data[0], old, PAGE_SIZE);
			memcpy(old, now, PAGE_SIZE);
		}
	}
#endif

public:
	void protect()
	{
		if (!started)
		{
#ifdef __EMSCRIPTEN__
			snapshot();
#else
			static_cast<T&>(*this).protectMem(0, 0xffffffff);
#endif
			started = true;
		}
#ifndef __EMSCRIPTEN__
		else
		{
			for (const auto& pair : pages)
				static_cast<T&>(*this).protectMem(pair.first, PAGE_SIZE);
		}
#endif
	}

	void unprotect()
	{
		static_cast<T&>(*this).unprotectMem(0, 0xffffffff);
	}

	void reset()
	{
		started = false;
		pages.clear();
	}

	bool hit(void *addr)
	{
		u32 offset = static_cast<T&>(*this).getMemOffset(addr);
		if (offset == (u32)-1)
			return false;
		offset &= ~PAGE_MASK;
	    auto rv = pages.try_emplace(offset);
	    if (!rv.second)
	      // already saved
	      return true;
	    Page& page = rv.first->second;
	    memcpy(&page.data[0], static_cast<T&>(*this).getMemPage(offset), PAGE_SIZE);
		static_cast<T&>(*this).unprotectMem(offset, PAGE_SIZE);
		return true;
	}

	void getPages(PageMap& other)
	{
#ifdef __EMSCRIPTEN__
		if (started)
			scan();
#endif
		std::swap(pages, other);
		pages = PageMap();
	}
};

class VramWatcher : public Watcher<VramWatcher>
{
	friend class Watcher<VramWatcher>;

protected:
	void protectMem(u32 addr, u32 size)
	{
		addrspace::protectVram(addr, std::min(VRAM_SIZE - addr, size) & ~PAGE_MASK);
	}

	void unprotectMem(u32 addr, u32 size)
	{
		addrspace::unprotectVram(addr, std::min(VRAM_SIZE - addr, size) & ~PAGE_MASK);
	}

	u32 getMemOffset(void *p)
	{
		return addrspace::getVramOffset(p);
	}

public:
	u32 memSize() const { return VRAM_SIZE; }
	void *getMemPage(u32 addr)
	{
		return &vram[addr];
	}
};

class RamWatcher : public Watcher<RamWatcher>
{
	friend class Watcher<RamWatcher>;

protected:
	void protectMem(u32 addr, u32 size)
	{
		bm_LockPage(addr, std::min(RAM_SIZE - addr, size) & ~PAGE_MASK);
	}

	void unprotectMem(u32 addr, u32 size)
	{
		bm_UnlockPage(addr, std::min(RAM_SIZE - addr, size) & ~PAGE_MASK);
	}

	u32 getMemOffset(void *p)
	{
		return bm_getRamOffset(p);
	}

public:
	u32 memSize() const { return RAM_SIZE; }
	void *getMemPage(u32 addr)
	{
		return &mem_b[addr];
	}
};

class AicaRamWatcher : public Watcher<AicaRamWatcher>
{
	friend class Watcher<AicaRamWatcher>;

protected:
	void protectMem(u32 addr, u32 size);
	void unprotectMem(u32 addr, u32 size);
	u32 getMemOffset(void *p);

public:
	u32 memSize() const { return ARAM_SIZE; }
	void *getMemPage(u32 addr)
	{
		return &aica::aica_ram[addr];
	}
};

class ElanRamWatcher : public Watcher<ElanRamWatcher>
{
	friend class Watcher<ElanRamWatcher>;

protected:
	void protectMem(u32 addr, u32 size);
	u32 getMemOffset(void *p);

public:
	void unprotectMem(u32 addr, u32 size);
	u32 memSize() const { return settings.platform.isNaomi2() ? elan::ERAM_SIZE : 0; }
	void *getMemPage(u32 addr)
	{
		return &elan::RAM[addr];
	}
};

extern VramWatcher vramWatcher;
extern RamWatcher ramWatcher;
extern AicaRamWatcher aramWatcher;
extern ElanRamWatcher elanWatcher;

inline static bool writeAccess(void *p)
{
	if (!config::GGPOEnable)
		return false;
	if (ramWatcher.hit(p))
	{
		bm_RamWriteAccess(p);
		return true;
	}
	if (vramWatcher.hit(p))
	{
		VramLockedWrite((u8 *)p);
		return true;
	}
	if (settings.platform.isNaomi2() && elanWatcher.hit(p))
		return true;
	return aramWatcher.hit(p);
}

inline static void protect()
{
	if (!config::GGPOEnable)
		return;
	vramWatcher.protect();
	ramWatcher.protect();
	aramWatcher.protect();
	elanWatcher.protect();
}

inline static void unprotect()
{
	vramWatcher.unprotect();
	ramWatcher.unprotect();
	aramWatcher.unprotect();
	elanWatcher.unprotect();
}

inline static void reset()
{
	vramWatcher.reset();
	ramWatcher.reset();
	aramWatcher.reset();
	elanWatcher.reset();
}

}
