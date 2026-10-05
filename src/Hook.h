#pragma once


namespace Hooks
{
	class HookPlayerCharacter
	{
	public:
		static void Install()
		{
			REL::Relocation<std::uintptr_t> PlayerCharacterVtbl{ RE::VTABLE_PlayerCharacter[0] };
			_Update = PlayerCharacterVtbl.write_vfunc(0xAD, Update);
			logs::info("hook playercharacter");
		}

		static void Update(RE::Actor* a_this, float a_delta);

		static inline REL::Relocation<decltype(Update)> _Update;
	};

	class HookBSLightingShader
	{
	public:
		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			auto& trampoline = SKSE::GetTrampoline();
			BSBatchRenderer_RenderPassImmediately::func = trampoline.write_call<5>(
				REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F),   // SE 1.5.97: fn 0x141308030, call site +0x29E = 0x1413082ce
				reinterpret_cast<uintptr_t>(BSBatchRenderer_RenderPassImmediately::thunk)
				);
			logs::info("hook renderpassimmediately");
		}
	};

	class OcclusionStripper :
		public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>,
		public RE::BSTEventSink<RE::TESLoadGameEvent>
	{
	public:
		static OcclusionStripper* GetSingleton();
		static void               Install();

		RE::BSEventNotifyControl ProcessEvent(
			const RE::TESCellFullyLoadedEvent*                a_event,
			RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override;

		RE::BSEventNotifyControl ProcessEvent(
			const RE::TESLoadGameEvent*                a_event,
			RE::BSTEventSource<RE::TESLoadGameEvent>*) override;

		static void SetStripped(bool a_stripped);
	};

	void Install();
	void OnDataLoaded();
}