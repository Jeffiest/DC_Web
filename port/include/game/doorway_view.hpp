#pragma once

/**
 * @file
 * MoonMod (quick passage, O2 C2): a town's way into a dungeon and the dungeon's way out show each other. Each side keeps
 * a picture of itself as the party arrives (its ground and walls, before any character is drawn), taken while the held
 * picture still covers the screen, and each draws the other's picture in the doorway as a window, so the cave is seen
 * from the village and the village from the cave. A side knows where its door is from where the party left it or
 * arrived at it. Only with quick passage on: nothing is kept or drawn otherwise.
 */

namespace dc::game {

class CMapParts;

// A jump from from_map to to_map started a held picture: the side left keeps where its door is.
void DoorwayJumped(int from_map, int to_map);
// Called by a town's and a dungeon's draw once it has set its camera: on the frame after a jump (under the held
// picture) it moves the camera of the place being left to its door, looking back in, for that place's picture.
void DoorwayCamera();
// Called by a town's and a dungeon's draw after their ground and before their characters, with the player's position.
// `facing` (the way the party faces, in a dungeon) places the dungeon's door once walked in, the town behind it,
// and walks the party through it.
void DoorwayDraw(const float player_position[3], const float *facing = nullptr);
// Forgets every door and picture (a new game, a loaded save).
void DoorwayReset();

/**
 * MoonMod (O2 C1, L4): with both places kept, draws the other place live into the doorway's view, under this
 * tick's camera taken through the door; DoorwayDraw then shows it in the door. Called once the mode's camera is
 * set, before the mode draws its own world.
 */
void DoorwayLive();

// After walking out of a dungeon's way out: where the party stands in the town, just outside the town's door
// facing away from it, and its heading (front = sin, cos). True once per walk-through.
bool DoorwayArrival(float position[3], float &heading);
// The party is walking into town `map` through its dungeon's way out (L3: the town is gone back to, not loaded).
bool DoorwayWalkingInto(int map);
// The party is walking into dungeon `map` through its town's door, the dungeon's doorway already placed (L3: the floor
// is gone back to, not built again); DoorwayDungeonArrival then gives where it stands, inside that doorway, once.
bool DoorwayEnteringKept(int map);
bool DoorwayDungeonArrival(float position[3], float &heading);

// The cave seen through a town's door before the first visit (L4): the dungeon map whose first floor is to be built,
// unseen, as town `town_map` loads (-1: the town has no such door, or connected worlds are off; -2: its doorway is
// placed already). DoorwayPrebuilt then places both sides of it from where that build left the party.
int  DoorwayPrebuildWanted(int town_map);
// A prebuild that did not stand built (DunPrebuild false): the town does not try again until a save is loaded; its
// dungeon is built on the visit, and is connected from then on as any other.
void DoorwayPrebuildFailed(int town_map);
int  DoorwayTownDungeon(int town_map);
void DoorwayPrebuilt(int town_map, int dungeon_map, const float position[3], const float facing[3]);

// A town's door into its dungeon, once opened (its event run through to the jump), stands open after: the town's own
// draw slides its halves apart as it loads (DoorwayStandTownDoorOpen, with the town's fixed parts), and walking into
// it goes in (DoorwayOpenTownDoor: the dungeon map when the party in front of it faces in, -1 when it does not, -2
// when the door is not open and its event runs as ever). Only while the cave is there to be seen through it.
// DoorwayCloseTownDoors forgets them all, and the prebuilds that failed (a loaded save).
void DoorwayStandTownDoorOpen(int town_map, CMapParts *fixed_parts, int fixed_parts_count);
int  DoorwayOpenTownDoor(int town_map, const float facing[3]);
void DoorwayCloseTownDoors();

// A town's road out (the open world) where its own gate (the ground its program 1, the world map, runs from) is not
// where the world map sets the party down: Norune, Queens, and Yellow Drops, which has no program 1. The road is at that spot:
// standing within reach of it facing out of the town is true once, and true again only after the party has faced back
// into the town or walked off. False in a town not listed.
bool DoorwayTownRoad(int town_map, const float position[3], const float facing[3]);

// The places' own draws for the doorway, each under the camera mgRenderInfo holds (eye and look-at given for the
// place's own clipping), with the place's own lights; each clears the target to its own background first.
void EdDrawForDoorway(const float eye[3], const float ref[3]);
void DunDrawForDoorway(const float eye[3], const float ref[3]);
// The dungeon's own: builds `dungeon_map`'s first floor unseen (a town loading); true once it stands built.
bool DunPrebuild(int dungeon_map);
// The wall of the dungeon's grid behind a point, looking along `back`: the room's (or the corridor's) edge on the
// grid axis nearest `back`, at the point's row or column. False on a floor without the grid.
bool DunDoorwayWall(const float position[3], const float back[3], float wall[3], float axis[3]);
// The floor's own door at its way in (the entrance piece's "in_door", which rises and falls on the stairs walk-in),
// near a point, drawn no more: the way out is the town's door now, its sliding halves seen through it.
void DunHideWayOutDoor(const float position[3]);

} // namespace dc::game
