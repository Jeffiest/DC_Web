#pragma once

#include "common.h"

#include "dataalloc_fwd.hpp"

/**
 * @file
 * Declares the calls that build a model out of the data that was read.
 */

namespace dc::game {

class CFrame;

/**
 * Clears the global data arena and prepares its persistent scene banks.
 */
void InitializeDataBuffer();
// MoonMod (O2 C1): the next InitializeDataBuffer carves and clears the arenas of world `world` (platform/arena.hpp),
// leaving every other world's as they are; without this call it is world 0, the one everything shares.
void SetNextDataWorld(int world);
// MoonMod (O2 C1, L3): a world left for another is kept as it was; going back to it switches to it (its arenas,
// textures and buffers as they stood) instead of loading it again. False when the world was not kept.
bool DataWorldKept(int world);
// Forgets every kept world (the title: a save loaded next has places of its own).
void DiscardDataWorlds();
// Whether world `world` is current or kept (a place there is, whatever mode is up).
bool DataWorldAvailable(int world);
// An arena laid out by hand inside the current world's blocks (not carved), kept and restored with the world's: the
// event scripts the other world runs point some of them elsewhere.
void KeepDataArena(CDataAlloc2<1> *arena);
bool ResumeDataWorld(int world);

/**
 * Reserves the read, packet, and scratch buffers from the global data arena.
 */
void SetPacketReadBuffer(int packet_quads, int read_quads);

/**
 * Gives every data buffer back, so the next area starts from an empty one.
 */
void BufferAllClear();

/**
 * Applies the attribute codes after "__" in a frame's name to its draw
 * attributes, and to its children's as well when recurse is set.
 */
void SetFrameAttr(CFrame *frame, int recurse);

/**
 * Receives the files that synchronous game-data reads load.
 */
extern u_int *read_buffer;

/**
 * Points at the shared scratch allocator used while loading and transforming data.
 */
extern CDataAlloc2<1> *WorkBuffer;

/**
 * Points at whichever per-frame VU data arena the current frame builds into.
 */
extern CDataAlloc2<1> *ActiveData;

} // namespace dc::game
