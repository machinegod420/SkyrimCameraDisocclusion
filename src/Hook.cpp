#include "Hook.h"
#include "Settings.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <cstring>
#include <Windows.h>
#include <MinHook.h>

namespace Hooks
{
	struct HitEntry
	{
		RE::NiPointer<RE::NiAVObject> keepalive;
		float                         secondsSinceSeen;
		float                         distFromCamera;
	};
	static std::unordered_map<RE::NiAVObject*, HitEntry> g_hitObjects;

	using HitSet = std::unordered_set<RE::NiAVObject*>;
	static std::atomic<std::shared_ptr<const HitSet>> g_publishedHits;
	static std::atomic<float>                         g_clipDistance{ 0.0f };
	// Player eye, published by Update (main thread), read from cull threads.
	static std::atomic<float>                         g_eyeX{ 0.0f };
	static std::atomic<float>                         g_eyeY{ 0.0f };
	static std::atomic<float>                         g_eyeZ{ 0.0f };
	// per-frame perf counters, reset once a second by the [perf] log in Update
	static std::atomic<std::uint32_t>                 g_roomSeedCalls{ 0 };
	static std::atomic<std::uint32_t>                 g_roomSeedClaimed{ 0 };
	static std::atomic<std::uint32_t>                 g_clippedPasses{ 0 };

	static void UpdateCameraData()
	{
		using func_t = decltype(&UpdateCameraData);
		static REL::Relocation<func_t> func{ RELOCATION_ID(75472, 77258) };   // SE 1.5.97: 0x140d6b210
		func();
	}

	using ClearRTV_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const float[4]);
	static ClearRTV_t s_origClearRTV = nullptr;

	static void STDMETHODCALLTYPE HookedClearRTV(ID3D11DeviceContext* a_ctx, ID3D11RenderTargetView* a_rtv, const float a_color[4])
	{
		if (g_clipDistance.load(std::memory_order_relaxed) > 0.0f && a_rtv) {
			if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				auto& mainTarget = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
				if (a_rtv == mainTarget.RTV) {
					const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
					s_origClearRTV(a_ctx, a_rtv, black);
					return;
				}
			}
		}
		s_origClearRTV(a_ctx, a_rtv, a_color);
	}

	static void InstallClearRTVHook()
	{
		static bool s_installed = false;
		if (s_installed) {
			return;
		}
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			return;
		}
		auto* ctx = renderer->GetRuntimeData().context;
		if (!ctx) {
			return;
		}
		void** vtbl = *reinterpret_cast<void***>(ctx);
		constexpr std::size_t kClearRTVIndex = 50;
		DWORD oldProtect = 0;
		if (!VirtualProtect(&vtbl[kClearRTVIndex], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
			return;
		}
		s_origClearRTV = reinterpret_cast<ClearRTV_t>(vtbl[kClearRTVIndex]);
		vtbl[kClearRTVIndex] = reinterpret_cast<void*>(&HookedClearRTV);
		VirtualProtect(&vtbl[kClearRTVIndex], sizeof(void*), oldProtect, &oldProtect);
		s_installed = true;
		logs::info("ClearRenderTargetView hook installed");
	}

	void HookBSLightingShader::BSBatchRenderer_RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
	{
		bool        shouldClip = false;
		const float clipDist   = g_clipDistance.load(std::memory_order_relaxed);
		auto        hits       = g_publishedHits.load(std::memory_order_acquire);
		if (a_pass && a_pass->geometry && clipDist > 0.0f && hits && !hits->empty()) {
			RE::NiAVObject* cur = a_pass->geometry;
			while (cur) {
				if (hits->count(cur) != 0) {
					shouldClip = true;
					break;
				}
				cur = cur->parent;
			}
		}

		if (shouldClip) {
			g_clippedPasses.fetch_add(1, std::memory_order_relaxed);
			auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			if (shadowState) {
				auto& data = shadowState->GetRuntimeData();
				auto& cameraData = *reinterpret_cast<RE::BSGraphics::ViewData*>(&data.cameraData);

				if (std::abs(cameraData.projMat._34 - 1.0f) < 0.01f && std::abs(cameraData.projMat._43) < 50.0f) {
					float origNear = -cameraData.projMat._43;
					if (origNear > 0.0f && clipDist > origNear) {
						auto origProj = cameraData.projMat;
						auto origViewProj = cameraData.viewProjMat;
						auto origProjUnj = cameraData.projMatrixUnjittered;
						auto origViewProjUnj = cameraData.viewProjMatrixUnjittered;

						auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
						std::uint32_t numViewports = 1;
						REX::W32::D3D11_VIEWPORT viewport;
						context->RSGetViewports(&numViewports, &viewport);

						const float origMinDepth = viewport.minDepth;
						const float minDepthShift = 1.0f - (origNear / clipDist);
						viewport.minDepth = origMinDepth + minDepthShift * (viewport.maxDepth - origMinDepth);
						context->RSSetViewports(1, &viewport);

						// z_ndc = _33 + _43/z. Moving the near plane via _43 alone leaves a constant-term
						// error of (newMin - origMin) * (1 - _33) for a finite far plane; scaling
						// (_33 - 1) by clipDist/origNear cancels it.
						const float projRatio = clipDist / origNear;

						cameraData.projMat._43 = -clipDist;
						cameraData.projMat._33 = 1.0f + (origProj._33 - 1.0f) * projRatio;
						cameraData.viewProjMat = cameraData.viewMat * cameraData.projMat;
						cameraData.projMatrixUnjittered._43 = -clipDist;
						cameraData.projMatrixUnjittered._33 = 1.0f + (origProjUnj._33 - 1.0f) * projRatio;
						cameraData.viewProjMatrixUnjittered = cameraData.viewMat * cameraData.projMatrixUnjittered;

						UpdateCameraData();
						func(a_pass, a_technique, a_alphaTest, a_renderFlags);

						// Re-read current D3D viewport so we don't clobber any field
						// func() may have updated mid-pass; restore only minDepth.
						context->RSGetViewports(&numViewports, &viewport);
						viewport.minDepth = origMinDepth;
						context->RSSetViewports(1, &viewport);

						cameraData.projMat = origProj;
						cameraData.viewProjMat = origViewProj;
						cameraData.projMatrixUnjittered = origProjUnj;
						cameraData.viewProjMatrixUnjittered = origViewProjUnj;
						UpdateCameraData();
						return;
					}
				}
			}
		}
		func(a_pass, a_technique, a_alphaTest, a_renderFlags);
	}

	static bool IsFinite(const RE::NiPoint3& a_pos)
	{
		return std::isfinite(a_pos.x) && std::isfinite(a_pos.y) && std::isfinite(a_pos.z);
	}

	// Clears all see-through state. Every early return on Update's player path must
	// go through here, or g_clipDistance latches and the world renders permanently clipped.
	static void ResetSeeThrough()
	{
		g_hitObjects.clear();
		g_publishedHits.store(std::make_shared<const HitSet>(), std::memory_order_release);
		g_clipDistance.store(0.0f, std::memory_order_release);
		OcclusionStripper::SetStripped(false);
		// The sky is culled during interior see-through; restore it here too.
		if (auto* sky = RE::Sky::GetSingleton()) {
			if (auto* skyRoot = sky->root.get()) {
				skyRoot->SetAppCulled(false);
			}
		}
	}

	void HookPlayerCharacter::Update(RE::Actor* a_this, float a_delta)
	{
		_Update(a_this, a_delta);
		if (!a_this || !a_this->IsPlayerRef()) {
			return;  // not the player: no state of ours to touch
		}
		if (!a_this->Is3DLoaded()) {
			ResetSeeThrough();  // mid-transition; do not latch
			return;
		}

		auto camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			ResetSeeThrough();
			return;
		}

		RE::NiPoint3 cameraPos = camera->GetRuntimeData2().pos;
		RE::NiPoint3 playerPos = a_this->GetPosition();
		playerPos.z += Settings::PlayerHeightOffset;
		g_eyeX.store(playerPos.x, std::memory_order_relaxed);
		g_eyeY.store(playerPos.y, std::memory_order_relaxed);
		g_eyeZ.store(playerPos.z, std::memory_order_relaxed);

		auto* parentCell = a_this->GetParentCell();
		auto* bhkWorld = parentCell ? parentCell->GetbhkWorld() : nullptr;
		if (!bhkWorld || !IsFinite(cameraPos) || !IsFinite(playerPos)) {
			ResetSeeThrough();  // physics world not ready (e.g. during coc)
			return;
		}

		const bool seeThroughActive = g_clipDistance.load(std::memory_order_acquire) > 0.0f;

		if (auto* sky = RE::Sky::GetSingleton()) {
			if (auto* skyRoot = sky->root.get()) {
				const bool shouldHide = seeThroughActive && parentCell->IsInteriorCell();
				skyRoot->SetAppCulled(shouldHide);
			}
		}

		float scale = RE::bhkWorld::GetWorldScale();
		RE::CFilter colFilter;
		a_this->GetCollisionFilterInfo(colFilter);
		for (auto& [obj, entry] : g_hitObjects) {
			entry.secondsSinceSeen += a_delta;
		}

		const RE::NiPoint3 ray = playerPos - cameraPos;
		const float totalDist = ray.Length();
		if (totalDist < Settings::MinCameraDistance) {
			ResetSeeThrough();
			return;
		}
		const RE::NiPoint3 dir = ray * (1.0f / totalDist);

		RE::NiPoint3 u = dir.Cross(RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
		if (const float uLen = u.Length(); uLen < 1e-3f) {
			u = { 1.0f, 0.0f, 0.0f };
		} else {
			u = u * (1.0f / uLen);
		}
		const RE::NiPoint3 vBase = dir.Cross(u);

		static std::uint32_t s_fanFrame = 0;
		constexpr float      kGoldenAngle = 2.39996323f;
		const float          phase        = s_fanFrame++ * kGoldenAngle;

		auto* selfRoot = a_this->Get3D();
		bool  hadHitThisFrame = false;
		const int       kMaxHits     = static_cast<int>(Settings::MaxMarchHits);
		constexpr float kStepEpsilon = 2.0f;

		auto ellipse = [&](float a_baseAngle) {
			const float a = a_baseAngle + phase;
			return u * (std::cos(a) * Settings::FanRadiusH) + vBase * (std::sin(a) * Settings::FanRadiusV);
		};
		constexpr float kPi = 3.14159265f;
		constexpr int   kRingCount = 8;

		// Center ray every frame plus RingRaysPerFrame of the 8 ring rays, rotating.
		static std::uint32_t s_ringCursor = 0;
		const int ringPerFrame = std::clamp(static_cast<int>(Settings::RingRaysPerFrame), 0, kRingCount);

		RE::NiPoint3 offsets[1 + kRingCount];
		int offsetCount = 0;
		offsets[offsetCount++] = { 0.0f, 0.0f, 0.0f };
		for (int k = 0; k < ringPerFrame; ++k) {
			const auto idx = (s_ringCursor + static_cast<std::uint32_t>(k)) % kRingCount;
			offsets[offsetCount++] = ellipse(kPi * 0.25f * static_cast<float>(idx));
		}
		s_ringCursor = (s_ringCursor + static_cast<std::uint32_t>(ringPerFrame)) % kRingCount;

		const bool hadLingeringHits = !g_hitObjects.empty();

		{
		RE::BSReadLockGuard worldLock(bhkWorld->worldLock);
		for (int oi = 0; oi < offsetCount; ++oi) {
			if (oi == 1 && !hadHitThisFrame && !hadLingeringHits) {
				break;  // center ray clean and nothing lingering: player is unobstructed
			}
			const RE::NiPoint3& offset = offsets[oi];
			const RE::NiPoint3 rayFromBase = cameraPos + offset;
			const RE::NiPoint3 rayToBase   = playerPos + offset;
			float start = 0.0f;

			for (int i = 0; i < kMaxHits; ++i) {
				const RE::NiPoint3 from = rayFromBase + dir * start;
				RE::bhkPickData pick;
				pick.rayInput.from       = { from.x * scale, from.y * scale, from.z * scale, 0 };
				pick.rayInput.to         = { rayToBase.x * scale, rayToBase.y * scale, rayToBase.z * scale, 0 };
				pick.rayInput.filterInfo = colFilter;

				if (!bhkWorld->PickObject(pick) || !pick.rayOutput.HasHit()) {
					break;
				}

				const float segLen            = totalDist - start;
				const float hitDistFromCamera = start + segLen * pick.rayOutput.hitFraction;

				auto* refr = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
				RE::NiAVObject* root = refr ? refr->Get3D() : nullptr;
				if (!root) {
					root = RE::TESHavokUtilities::FindCollidableObject(*pick.rayOutput.rootCollidable);
				}
				const bool isFloorish = std::abs(pick.rayOutput.normal.Dot3({ 0.0f, 0.0f, 1.0f, 0.0f })) > 0.7f;

				const bool isLightFixture = false;

				if (!isFloorish && !isLightFixture && root && root != selfRoot && refr != a_this) {
					const auto colLayer = pick.rayOutput.rootCollidable->GetCollisionLayer();
					if (colLayer == RE::COL_LAYER::kStatic || colLayer == RE::COL_LAYER::kAnimStatic) {
						auto [it, inserted] = g_hitObjects.try_emplace(root, HitEntry{ RE::NiPointer<RE::NiAVObject>(root), 0.0f, hitDistFromCamera });
						auto& entry = it->second;
						entry.secondsSinceSeen = 0.0f;
						if (!inserted && hitDistFromCamera > entry.distFromCamera) {
							entry.distFromCamera = hitDistFromCamera;
						}
						hadHitThisFrame = true;
					}
				}

				start = hitDistFromCamera + kStepEpsilon;
				if (start >= totalDist) {
					break;
				}
			}
		}
		}

		std::erase_if(g_hitObjects, [](const auto& kv) {
			return kv.second.secondsSinceSeen > Settings::HitLingerSeconds;
		});

		auto  snap     = std::make_shared<HitSet>();
		float furthest = 0.0f;
		snap->reserve(g_hitObjects.size());
		for (const auto& [obj, entry] : g_hitObjects) {
			snap->insert(obj);
			if (entry.distFromCamera > furthest) {
				furthest = entry.distFromCamera;
			}
		}
		g_publishedHits.store(std::shared_ptr<const HitSet>(std::move(snap)), std::memory_order_release);
		g_clipDistance.store(g_hitObjects.empty() ? 0.0f : furthest + 15.0f, std::memory_order_release);

		static float s_secondsSinceHit = 1e6f;
		if (hadHitThisFrame) {
			s_secondsSinceHit = 0.0f;
		} else {
			s_secondsSinceHit += a_delta;
		}
		OcclusionStripper::SetStripped(s_secondsSinceHit < Settings::StripLingerSeconds);

		// Once-a-second [perf] line (Debug.Log=1): room-seed claims and clipped passes per frame.
		{
			static float         s_accum  = 0.0f;
			static std::uint32_t s_frames = 0;
			s_accum += a_delta;
			++s_frames;
			if (s_accum >= 1.0f && s_frames > 0) {
				const auto calls   = g_roomSeedCalls.exchange(0, std::memory_order_relaxed);
				const auto claimed = g_roomSeedClaimed.exchange(0, std::memory_order_relaxed);
				const auto passes  = g_clippedPasses.exchange(0, std::memory_order_relaxed);
				if (Settings::DebugLog > 0.0f) {
					logs::info("[perf] {:.0f}fps | roomseed calls/f={} claimed/f={} | clippedPasses/f={} hits={} clip={:.0f}",
						s_frames / s_accum, calls / s_frames, claimed / s_frames, passes / s_frames,
						g_hitObjects.size(), g_clipDistance.load(std::memory_order_relaxed));
				}
				s_accum  = 0.0f;
				s_frames = 0;
			}
		}
	}

	namespace
	{
		// this is a grotesque reinterpret cast because the pointerlist in commonlib i dont think is correct
		struct RawNode
		{
			RawNode*              next;
			RawNode*              prev;
			RE::BSOcclusionShape* shape;
		};
		static_assert(sizeof(RawNode) == 0x18);

		struct RawList
		{
			RawNode*      head;
			RawNode*      tail;
			std::uint32_t size;
			std::uint32_t pad;
		};
		static_assert(sizeof(RawList) == 0x18);

		constexpr float kStripRadius = 512.f;

		bool ShapeWorldAABB(const RE::BSOcclusionShape* a_shape, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
		{
			if (!a_shape) {
				return false;
			}
			RE::NiPoint3 e{ 0.0f, 0.0f, 0.0f };
			if (a_shape->IsOcclusionBox()) {
				const auto* box = static_cast<const RE::BSOcclusionBox*>(a_shape);
				e = { box->size.x, box->size.y, box->size.z };
			} else if (a_shape->IsOcclusionPlane()) {
				const auto* plane = static_cast<const RE::BSOcclusionPlane*>(a_shape);
				e = { plane->size.x, plane->size.y, 0.0f };
			} else {
				return false;
			}
			const auto& r = a_shape->rotation;
			const RE::NiPoint3 worldHalf{
				std::abs(r.entry[0][0]) * e.x + std::abs(r.entry[0][1]) * e.y + std::abs(r.entry[0][2]) * e.z,
				std::abs(r.entry[1][0]) * e.x + std::abs(r.entry[1][1]) * e.y + std::abs(r.entry[1][2]) * e.z,
				std::abs(r.entry[2][0]) * e.x + std::abs(r.entry[2][1]) * e.y + std::abs(r.entry[2][2]) * e.z,
			};
			a_min = a_shape->translation - worldHalf;
			a_max = a_shape->translation + worldHalf;
			return true;
		}

		bool SegmentIntersectsAABB(const RE::NiPoint3& a_A, const RE::NiPoint3& a_B,
		                           const RE::NiPoint3& a_min, const RE::NiPoint3& a_max)
		{
			const RE::NiPoint3 D = a_B - a_A;
			float tmin = 0.0f, tmax = 1.0f;
			for (int i = 0; i < 3; ++i) {
				const float a  = (&a_A.x)[i];
				const float d  = (&D.x)[i];
				const float lo = (&a_min.x)[i];
				const float hi = (&a_max.x)[i];
				if (std::abs(d) < 1e-6f) {
					if (a < lo || a > hi) {
						return false;
					}
					continue;
				}
				float t1 = (lo - a) / d;
				float t2 = (hi - a) / d;
				if (t1 > t2) {
					std::swap(t1, t2);
				}
				tmin = max(tmin, t1);
				tmax = min(tmax, t2);
				if (tmin > tmax) {
					return false;
				}
			}
			return true;
		}

		// True when the plane's world AABB, grown by PlaneSuppressMargin, crosses the
		// a_eye -> player-eye segment. The one criterion all three plane hooks share.
		bool PlaneOnSightline(const RE::BSOcclusionShape* a_shape, const RE::NiPoint3& a_eye)
		{
			const RE::NiPoint3 player{
				g_eyeX.load(std::memory_order_relaxed),
				g_eyeY.load(std::memory_order_relaxed),
				g_eyeZ.load(std::memory_order_relaxed) };
			RE::NiPoint3 mn, mx;
			if (!IsFinite(player) || !IsFinite(a_eye) || !ShapeWorldAABB(a_shape, mn, mx)) {
				return false;
			}
			const float        m = Settings::PlaneSuppressMargin;
			const RE::NiPoint3 r{ m, m, m };
			return SegmentIntersectsAABB(a_eye, player, mn - r, mx + r);
		}

		struct PositionSnapshot
		{
			RE::NiPointer<RE::BSOcclusionShape> keepalive;
			RE::NiPoint3                        saved;
		};

		std::unordered_map<RE::BSOcclusionShape*, PositionSnapshot>& Positions()
		{
			static std::unordered_map<RE::BSOcclusionShape*, PositionSnapshot> s_map;
			return s_map;
		}

		bool& StrippedFlag()
		{
			static bool s_stripped = false;
			return s_stripped;
		}

		constexpr RE::NiPoint3 kFarawayPosition{ 1.0e8f, 1.0e8f, 1.0e8f };

		void DisplaceList(RawList* a_list,
		                  const RE::NiPoint3& a_segA, const RE::NiPoint3& a_segB,
		                  float a_radius)
		{
			if (!a_list) {
				return;
			}
			auto& map = Positions();
			for (auto* node = a_list->head; node; node = node->next) {
				auto* shape = node->shape;
				if (!shape) {
					continue;
				}
				if (shape->IsOcclusionPlane()) {
					continue;   // planes ignore translation, so displacing them does nothing
				}
				if (map.contains(shape)) {
					continue;
				}
				RE::NiPoint3 mn, mx;
				if (!ShapeWorldAABB(shape, mn, mx)) {
					continue;
				}
				const RE::NiPoint3 r{ a_radius, a_radius, a_radius };
				if (!SegmentIntersectsAABB(a_segA, a_segB, mn - r, mx + r)) {
					continue;
				}
				map.emplace(shape, PositionSnapshot{
					RE::NiPointer<RE::BSOcclusionShape>(shape),
					shape->translation
				});
				shape->translation = kFarawayPosition;
			}
		}

		void RestoreAllPositions()
		{
			auto& map = Positions();
			for (auto& [rawPtr, snap] : map) {
				if (auto* live = snap.keepalive.get()) {
					live->translation = snap.saved;
				}
			}
			map.clear();
		}

	}


	struct HookQPointWithin
	{
		// QPointWithin is the engine's "does this room contain the viewer" test that
		// seeds the portal flood. When the camera is outside every room (a doorway,
		// inside a wall) nothing seeds and rooms vanish. Consulted only when vanilla
		// says no; modes are documented in Settings.h.
		static bool thunk(RE::BSMultiBoundNode* a_this, RE::NiPoint3& a_point)
		{
			const bool vanilla = func(a_this, a_point);
			g_roomSeedCalls.fetch_add(1, std::memory_order_relaxed);
			if (vanilla) {
				return true;
			}
			const int mode = static_cast<int>(Settings::RoomSeedMode);
			if (mode <= 0) {
				return false;
			}
			if (mode == 3 && g_clipDistance.load(std::memory_order_acquire) <= 0.0f) {
				return false;   // distance mode is a blast radius: only while clipping
			}
			const RE::NiPoint3 player{
				g_eyeX.load(std::memory_order_relaxed),
				g_eyeY.load(std::memory_order_relaxed),
				g_eyeZ.load(std::memory_order_relaxed) };
			if (!IsFinite(player)) {
				return false;
			}

			bool claim = false;
			if (mode == 1) {
				RE::NiPoint3 p = player;
				claim = func(a_this, p);
			} else if (mode == 2) {
				// t=1 is the player eye; the rest walk back toward the camera.
				for (float t : { 1.0f, 0.75f, 0.5f, 0.25f }) {
					RE::NiPoint3 p{ a_point.x + (player.x - a_point.x) * t,
						            a_point.y + (player.y - a_point.y) * t,
						            a_point.z + (player.z - a_point.z) * t };
					if (func(a_this, p)) {
						claim = true;
						break;
					}
				}
			} else {
				const auto& b  = a_this->worldBound;
				const float dx = b.center.x - player.x, dy = b.center.y - player.y, dz = b.center.z - player.z;
				const float reach = Settings::RoomSeedDistance + b.radius;
				claim = (dx * dx + dy * dy + dz * dz) < reach * reach;
			}
			if (claim) {
				g_roomSeedClaimed.fetch_add(1, std::memory_order_relaxed);
			}
			return claim;
		}
		static inline REL::Relocation<decltype(thunk)> func;

		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSMultiBoundRoom[0] };
			func = vtbl.write_vfunc(0x3F, thunk);
			logs::info("[install] BSMultiBoundRoom::QPointWithin hook installed at vtbl[0x3F]");
		}
	};

	OcclusionStripper* OcclusionStripper::GetSingleton()
	{
		static OcclusionStripper inst;
		return &inst;
	}

	namespace
	{
		void DisplaceForFrame()
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc) {
				return;
			}
			auto* cam = RE::PlayerCamera::GetSingleton();
			const RE::NiPoint3 segB = pc->GetPosition();
			const RE::NiPoint3 segA = cam ? cam->GetRuntimeData2().pos : segB;

			auto* cell = pc->GetParentCell();
			if (cell) {
				if (auto* loaded = cell->GetRuntimeData().loadedData) {
					if (auto* g = loaded->portalGraph.get()) {
						DisplaceList(reinterpret_cast<RawList*>(&g->occlusionShapes),
						             segA, segB, kStripRadius);
					}
				}
			}
			if (auto* ws = pc->GetWorldspace()) {
				if (auto* g = ws->portalGraph.get()) {
					DisplaceList(reinterpret_cast<RawList*>(&g->occlusionShapes),
					             segA, segB, kStripRadius);
				}
			}
		}
	}

	namespace
	{
		// Legacy path (Occlusion.UseLegacyStrip=1): delete every planemarker ref in an
		// interior cell at load. Removes all plane occlusion in the cell.
		constexpr RE::FormID kPlaneMarkerFormID = 0x17;

		int StripPlanemarkersFromCell(RE::TESObjectCELL* a_cell)
		{
			if (!a_cell) {
				return 0;
			}
			auto& runtime = a_cell->GetRuntimeData();
			int   removed = 0;

			// Collect-then-erase from both ref containers to avoid iterator
			// invalidation surprises on BSTSet / BSTArray.
			std::vector<RE::TESObjectREFR*> hits;

			for (auto& nipRef : runtime.references) {
				auto* ref = nipRef.get();
				if (ref) {
					auto* base = ref->GetBaseObject();
					if (base && base->GetFormID() == kPlaneMarkerFormID) {
						hits.push_back(ref);
					}
				}
			}
			for (auto* ref : hits) {
				runtime.references.erase(RE::NiPointer<RE::TESObjectREFR>(ref));
				++removed;
			}
			hits.clear();

			for (auto* ref : runtime.objectList) {
				if (ref) {
					auto* base = ref->GetBaseObject();
					if (base && base->GetFormID() == kPlaneMarkerFormID) {
						hits.push_back(ref);
					}
				}
			}
			for (auto* ref : hits) {
				auto it = std::find(runtime.objectList.begin(), runtime.objectList.end(), ref);
				if (it != runtime.objectList.end()) {
					runtime.objectList.erase(it);
					++removed;
				}
			}
			return removed;
		}

		[[maybe_unused]] void StripAllPlanemarkers()
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				logs::warn("[strip] no TESDataHandler");
				return;
			}
			int totalCells    = 0;
			int affectedCells = 0;
			int totalRemoved  = 0;

			auto stripOne = [&](RE::TESObjectCELL* cell) {
				if (!cell) {
					return;
				}
				++totalCells;
				const int removed = StripPlanemarkersFromCell(cell);
				if (removed > 0) {
					++affectedCells;
					totalRemoved += removed;
				}
			};

			for (auto* cell : dh->interiorCells) {
				stripOne(cell);
			}
			auto& worldspaces = dh->GetFormArray<RE::TESWorldSpace>();
			for (auto* ws : worldspaces) {
				if (!ws) {
					continue;
				}
				stripOne(ws->persistentCell);
				for (auto& [coords, cell] : ws->cellMap) {
					stripOne(cell);
				}
			}

			logs::info("[strip] {} planemarkers removed from {}/{} cells",
				totalRemoved, affectedCells, totalCells);
		}
	}

	void OcclusionStripper::SetStripped(bool a_stripped)
	{
		constexpr float kRefreshInterval = 0.25f;
		static float*   g_DeltaTime      = (float*)RELOCATION_ID(523661, 410200).address();   // SE 1.5.97: 0x142f6b94c
		static float    s_secondsSinceRefresh = 0.0f;

		if (a_stripped == StrippedFlag()) {
			if (a_stripped) {
				s_secondsSinceRefresh += *g_DeltaTime;
				if (s_secondsSinceRefresh >= kRefreshInterval) {
					RestoreAllPositions();
					DisplaceForFrame();
					s_secondsSinceRefresh = 0.0f;
				}
			}
			return;
		}
		if (a_stripped) {
			DisplaceForFrame();
			StrippedFlag() = true;
			s_secondsSinceRefresh = 0.0f;
		} else {
			RestoreAllPositions();
			StrippedFlag() = false;
		}
	}

	RE::BSEventNotifyControl OcclusionStripper::ProcessEvent(
		const RE::TESCellFullyLoadedEvent*                a_event,
		RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*)
	{
		// Legacy path only; the default handles planes per-frame in the hooks.
		if (Settings::UseLegacyStrip > 0.0f && a_event && a_event->cell && a_event->cell->IsInteriorCell()) {
			const int removed = StripPlanemarkersFromCell(a_event->cell);
			if (removed > 0) {
				logs::info("[strip] cell-load {}: {} planemarkers removed",
					a_event->cell->GetFormEditorID(), removed);
			}
		}
		if (StrippedFlag()) {
			RestoreAllPositions();
			DisplaceForFrame();
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl OcclusionStripper::ProcessEvent(
		const RE::TESLoadGameEvent*,
		RE::BSTEventSource<RE::TESLoadGameEvent>*)
	{
		// Full reset including g_hitObjects, which hold NiAVObjects from the old world.
		ResetSeeThrough();
		RestoreAllPositions();
		StrippedFlag() = false;
		return RE::BSEventNotifyControl::kContinue;
	}

	void OcclusionStripper::Install()
	{
		if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
			src->AddEventSink<RE::TESCellFullyLoadedEvent>(GetSingleton());
			src->AddEventSink<RE::TESLoadGameEvent>(GetSingleton());
			logs::info("occlusion stripper sinks registered");
		}
	}

	// only have SE 1.5.97 IDs atm because i dont have AE
	static bool PlaneHooksSupported()
	{
		static const bool s_ok = [] {
			const bool ok = REL::Module::IsSE();
			if (!ok) {
				logs::warn("plane occlusion hooks are SE-only (ids 75067/75066/74699); skipping on this runtime, interior planemarker fix inactive");
			}
			return ok;
		}();
		return s_ok;
	}

	// Per-occluder compound-frustum build step (SE 1.5.97 id 75067, 0x140d5a9e0),
	// once per occluder from ShadowSceneNode frame prep (id 99734). Sees every plane
	// before the shape+0x40 mode branch (0 untested, 1 added last frame, 2 rejected).
	// Suppressing mirrors a mode-0 plane that fails the engine's test: return 0, mode 0, flag untouched.
	struct HookOcclusionBuildStep
	{
		using Fn = std::uint32_t (*)(void* a_frustum, RE::BSOcclusionShape* a_shape, const float* a_refPlane, std::uint8_t* a_flag);
		static inline Fn s_original = nullptr;

		static inline std::atomic<std::uint32_t> s_suppressed{ 0 };

		static std::int32_t ModeOf(const RE::BSOcclusionShape* a_shape)
		{
			std::int32_t m = 0;
			std::memcpy(&m, reinterpret_cast<const std::uint8_t*>(a_shape) + 0x40, sizeof(m));
			return m;
		}
		// Mode 0 makes every consumer re-test instead of trusting a stale mode 1.
		static void SetMode(RE::BSOcclusionShape* a_shape, std::int32_t a_mode)
		{
			std::memcpy(reinterpret_cast<std::uint8_t*>(a_shape) + 0x40, &a_mode, sizeof(a_mode));
		}

		static std::uint32_t thunk(void* a_frustum, RE::BSOcclusionShape* a_shape, const float* a_refPlane, std::uint8_t* a_flag)
		{
			if (a_shape && a_shape->IsOcclusionPlane()) {
				// The frustum's camera is the eye this build is about to use.
				auto* frustum = reinterpret_cast<RE::BSCompoundFrustum*>(a_frustum);
				auto* cam     = frustum ? frustum->camera : nullptr;
				if (cam && PlaneOnSightline(a_shape, cam->world.translate)) {
					const std::uint32_t k = s_suppressed.fetch_add(1, std::memory_order_relaxed);
					if (Settings::DebugLog > 0.0f && k < 8) {
						const auto& eye = cam->world.translate;
						logs::info("[occl-build] suppressed #{} shape={:#x} mode={} eye=({:.0f},{:.0f},{:.0f})",
							k, reinterpret_cast<std::uintptr_t>(a_shape), ModeOf(a_shape), eye.x, eye.y, eye.z);
					}
					SetMode(a_shape, 0);
					return 0;
				}
			}
			return s_original(a_frustum, a_shape, a_refPlane, a_flag);
		}

		static void Install()
		{
			if (!PlaneHooksSupported()) {
				return;
			}
			REL::Relocation<std::uintptr_t> target{ REL::ID(75067) };   // SE 1.5.97: 0x140d5a9e0
			const std::uintptr_t rva = target.address() - REL::Module::get().base();
			const auto init = MH_Initialize();
			if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
				logs::warn("[occl-build] MH_Initialize failed: {}", static_cast<int>(init));
				return;
			}
			auto* tgt = reinterpret_cast<void*>(target.address());
			if (MH_CreateHook(tgt, reinterpret_cast<void*>(&thunk), reinterpret_cast<void**>(&s_original)) != MH_OK ||
				MH_EnableHook(tgt) != MH_OK) {
				logs::warn("[occl-build] MinHook create/enable failed");
				return;
			}
			logs::info("[install] occlusion build-step detour at {:#x} (RVA {:#x}) via MinHook",
				target.address(), rva);
		}
	};

	// Second plane-add path (SE 1.5.97 id 75066, 0x140d5a920): takes mode-1 planes
	// straight to AddPlaneToCompound, bypassing the build step, so it needs the same
	// test. Returning 0 matches its own "not visible" exit (eax is unset there too).
	struct HookOcclusionAdd2
	{
		using Fn = std::uint32_t (*)(void* a_target, RE::BSOcclusionShape* a_shape, void* a_view);
		static inline Fn s_original = nullptr;

		static inline std::atomic<std::uint32_t> s_calls{ 0 };
		static inline std::atomic<std::uint32_t> s_suppressed{ 0 };

		static std::uint32_t thunk(void* a_target, RE::BSOcclusionShape* a_shape, void* a_view)
		{
			const std::uint32_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
			if (Settings::DebugLog > 0.0f && n < 4) {
				logs::info("[occl-add2] call n={} suppressedSoFar={}", n, s_suppressed.load());
			}
			if (a_shape && a_shape->IsOcclusionPlane()) {
				auto* f   = reinterpret_cast<RE::BSCompoundFrustum*>(a_target ? a_target : a_view);
				auto* cam = f ? f->camera : nullptr;
				if (cam && PlaneOnSightline(a_shape, cam->world.translate)) {
					const std::uint32_t k = s_suppressed.fetch_add(1, std::memory_order_relaxed);
					if (Settings::DebugLog > 0.0f && k < 8) {
						logs::info("[occl-add2] suppressed #{} shape={:#x}", k,
							reinterpret_cast<std::uintptr_t>(a_shape));
					}
					HookOcclusionBuildStep::SetMode(a_shape, 0);
					return 0;   // == the "plane not visible" exit
				}
			}
			return s_original(a_target, a_shape, a_view);
		}

		static void Install()
		{
			if (!PlaneHooksSupported()) {
				return;
			}
			REL::Relocation<std::uintptr_t> target{ REL::ID(75066) };   // SE 1.5.97: 0x140d5a920
			const std::uintptr_t rva = target.address() - REL::Module::get().base();
			const auto init = MH_Initialize();
			if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
				logs::warn("[occl-add2] MH_Initialize failed: {}", static_cast<int>(init));
				return;
			}
			auto* tgt = reinterpret_cast<void*>(target.address());
			if (MH_CreateHook(tgt, reinterpret_cast<void*>(&thunk), reinterpret_cast<void**>(&s_original)) != MH_OK ||
				MH_EnableHook(tgt) != MH_OK) {
				logs::warn("[occl-add2] MinHook create/enable failed");
				return;
			}
			logs::info("[install] second plane-add detour at {:#x} (RVA {:#x}) via MinHook", target.address(), rva);
		}
	};

	// Per-plane eye test (SE 1.5.97 id 74699, 0x140d4c3e0): (shape, eye) -> bool.
	// Called by the per-frame pre-pass that sets shape mode, and by the per-object
	// occluder query (0x1412c1384) that dynamic refs (NPCs) go through. False is the
	// engine's own "plane rejected" result: mode 2, never built, never tested.
	struct HookPlaneTest1
	{
		using Fn = bool (*)(RE::BSOcclusionShape* a_shape, const RE::NiPoint3* a_eye);
		static inline Fn s_original = nullptr;
		static inline std::atomic<std::uint32_t> s_calls{ 0 };
		static inline std::atomic<std::uint32_t> s_rejected{ 0 };

		static bool thunk(RE::BSOcclusionShape* a_shape, const RE::NiPoint3* a_eye)
		{
			const std::uint32_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
			if (Settings::DebugLog > 0.0f && n < 4) {
				logs::info("[plane-test1] call n={} rejectedSoFar={}", n, s_rejected.load());
			}
			if (a_shape && a_eye && PlaneOnSightline(a_shape, *a_eye)) {
				const std::uint32_t k = s_rejected.fetch_add(1, std::memory_order_relaxed);
				if (Settings::DebugLog > 0.0f && k < 4) {
					logs::info("[plane-test1] rejected #{} shape={:#x} eye=({:.0f},{:.0f},{:.0f})",
						k, reinterpret_cast<std::uintptr_t>(a_shape), a_eye->x, a_eye->y, a_eye->z);
				}
				return false;
			}
			return s_original(a_shape, a_eye);
		}

		static void Install()
		{
			if (!PlaneHooksSupported()) {
				return;
			}
			REL::Relocation<std::uintptr_t> target{ REL::ID(74699) };   // SE 1.5.97: 0x140d4c3e0
			const std::uintptr_t rva = target.address() - REL::Module::get().base();
			const auto init = MH_Initialize();
			if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
				logs::warn("[plane-test1] MH_Initialize failed: {}", static_cast<int>(init));
				return;
			}
			auto* tgt = reinterpret_cast<void*>(target.address());
			if (MH_CreateHook(tgt, reinterpret_cast<void*>(&thunk), reinterpret_cast<void**>(&s_original)) != MH_OK ||
				MH_EnableHook(tgt) != MH_OK) {
				logs::warn("[plane-test1] MinHook create/enable failed");
				return;
			}
			logs::info("[install] plane eye-test detour at {:#x} (RVA {:#x}) via MinHook", target.address(), rva);
		}
	};

	void Install()
	{
		HookPlayerCharacter::Install();
		HookBSLightingShader::Install();
		HookQPointWithin::Install();
		if (Settings::UseLegacyStrip > 0.0f) {
			logs::info("legacy planemarker strip selected; plane hooks not installed");
		} else {
			HookOcclusionBuildStep::Install();  // every plane, before the mode branch
			HookOcclusionAdd2::Install();       // mode-1 planes re-added each frame
			HookPlaneTest1::Install();          // mode-setter test; also the NPC path
		}
		OcclusionStripper::Install();
	}

	void OnDataLoaded()
	{
		InstallClearRTVHook();
	}
}
