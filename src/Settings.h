#pragma once

namespace Settings
{
	inline float PlayerHeightOffset = 150.0f;
	inline float FanRadiusH = 20.0f;
	inline float FanRadiusV = 6.0f;
	inline float StripLingerSeconds = 0.3f;
	inline float HitLingerSeconds = 2.0f;

	// Raycast fan budget. The ring rotates every frame and hits linger, so a
	// subset of the ring per frame converges on the same object set.
	inline float RingRaysPerFrame   = 2.0f;   // of 8; center ray is always cast
	inline float MaxMarchHits       = 4.0f;   // sequential hits per ray
	inline float MinCameraDistance  = 48.0f;  // below this there is nothing to clip

	// A planemarker is suppressed for a frame when its world AABB, grown by this
	// margin, intersects the camera->player segment. Smaller = more selective.
	inline float PlaneSuppressMargin = 512.0f;
	// 1 = delete every planemarker ref at interior cell load instead of the
	// per-frame plane hooks. Loses all plane occlusion in the cell; for A/B or on AE/VR.
	inline float UseLegacyStrip = 0.0f;

	// Room seeding, consulted for each room the engine says does not contain the camera:
	//   0 = vanilla (rooms vanish when the camera leaves all of them)
	//   1 = the player's room                            (1 test)
	//   2 = rooms the camera->player sightline crosses   (4 tests, default)
	//   3 = every room within RoomSeedDistance of the player, only while
	//       see-through is active; seeds most of a large dungeon, costs ~30fps
	inline float RoomSeedMode     = 2.0f;
	inline float RoomSeedDistance = 1024.0f;

	// Log=1 enables the once-a-second [perf] line and the hooks' first-N
	// diagnostic lines. Install lines and warnings always print.
	inline float DebugLog = 0.0f;

	void Load();
}
