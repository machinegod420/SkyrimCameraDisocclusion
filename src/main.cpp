#include "Hook.h"
#include "Settings.h"

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	Settings::Load();
	SKSE::AllocTrampoline(1 << 8);
	Hooks::Install();

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Hooks::OnDataLoaded();
		}
	});

	return true;
}
