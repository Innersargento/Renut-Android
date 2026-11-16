// Cheat hooks of the recompiled game. config/renut_android_hooks.toml makes the
// generated code call these at fixed guest addresses; the Android app turns them
// on through renut.toml (Jogar > Cheats), so they apply from the next start.

#include <atomic>
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/guest.h"
#include <cstring>
#include <string>

#include <rex/ppc/context.h>












namespace {

#define RENUT_CHEAT_ON(flag) (renut::GetSettings().cheats.Get() && renut::GetSettings().flag.Get())

// The guest strings the skin hook points r3 at. Checked before use: an address
// that does not hold the expected name is left alone instead of spawning garbage.
constexpr uint32_t kRobotBanjoActor = 0x8216B690;
constexpr uint32_t kTuxedoBanjoActor = 0x8216B6A4;

// Damage multiplier of the super punch.
constexpr double kSuperPunchFactor = 1000.0;
// Type byte of a health component's owner actor.
constexpr uint32_t kOwnerTypeOffset = 0xBE0;
constexpr int kBanjoOwnerType = 2;

int GuestByte(uint32_t address) {
  const uint8_t* bytes = renut::host::VirtualBase();
  return bytes ? bytes[address] : -1;
}

uint32_t GuestWord(uint32_t address) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value = value << 8 | static_cast<uint32_t>(GuestByte(address + i));
  return value;
}

bool GuestStringIs(uint32_t address, const char* expected) {
  const uint8_t* bytes = renut::host::VirtualBase();
  return bytes && std::strcmp(reinterpret_cast<const char*>(bytes + address), expected) == 0;
}

}  // namespace

// For the runtime report: which cheats this start applies, "" when none.
std::string renutCheatsSummary() {
  if (!renut::GetSettings().cheats.Get()) return {};
  std::string list;
  auto add = [&](bool on, const char* name) {
    if (!on) return;
    if (!list.empty()) list += ", ";
    list += name;
  };
  add(renut::GetSettings().cheat_infinite_health.Get(), "vida");
  add(renut::GetSettings().cheat_infinite_fuel_ammo.Get(), "combustível/munição");
  add(renut::GetSettings().cheat_infinite_parts.Get(), "peças");
  add(renut::GetSettings().cheat_no_timer.Get(), "sem tempo");
  add(renut::GetSettings().cheat_no_notes_spent.Get(), "notas");
  add(renut::GetSettings().cheat_vehicles_anywhere.Get(), "veículos em qualquer lugar");
  add(renut::GetSettings().cheat_extended_build_range.Get(), "área de montagem");
  add(renut::GetSettings().cheat_super_punch.Get(), "golpe forte");
  add(renut::GetSettings().cheat_banjo_skin.Get() != "default", ("visual " + renut::GetSettings().cheat_banjo_skin.Get()).c_str());
  return list.empty() ? "ligados, nenhum selecionado" : list;
}

// Hooks are declared by the generated code with C++ linkage, exactly as below.

bool renut_cheat_infinite_health() { return RENUT_CHEAT_ON(cheat_infinite_health); }

bool renut_cheat_infinite_fuel_ammo() { return RENUT_CHEAT_ON(cheat_infinite_fuel_ammo); }

void renut_cheat_infinite_parts(PPCRegister& r10) {
  if (RENUT_CHEAT_ON(cheat_infinite_parts)) r10.u32 = 100;
}

bool renut_cheat_no_timer() { return RENUT_CHEAT_ON(cheat_no_timer); }

bool renut_cheat_no_notes_spent() { return RENUT_CHEAT_ON(cheat_no_notes_spent); }

void renut_cheat_vehicles_anywhere(PPCRegister& r23, PPCRegister& r26) {
  if (!RENUT_CHEAT_ON(cheat_vehicles_anywhere)) return;
  r23.u32 = 1;  // Build Vehicle
  r26.u8 = 1;   // Change Vehicle
}

bool renut_cheat_extended_build_range() {
  return RENUT_CHEAT_ON(cheat_extended_build_range);
}

// Entry of sub_823F4720: f1 is the damage about to be subtracted from the component in r3.
// The component's owner (+0x30) has a type byte at +0xBE0; 2 is Banjo (it took damage in the
// logs, objects and enemies were 4), who is left alone.
void renut_cheat_super_punch(PPCRegister& f1, PPCRegister& r3) {
  if (!r3.u32 || !RENUT_CHEAT_ON(cheat_super_punch)) return;
  const uint32_t owner = GuestWord(r3.u32 + 0x30);
  if (owner && GuestByte(owner + kOwnerTypeOffset) == kBanjoOwnerType) return;
  f1.f64 *= kSuperPunchFactor;
}

// True keeps the game's own choice of actor; false jumps past it with the
// outfit's actor already in r3.
bool renut_cheat_banjo_skin(PPCRegister& r3, PPCRegister& r5) {
  if (!renut::GetSettings().cheats.Get()) return true;
  const std::string& skin = renut::GetSettings().cheat_banjo_skin.Get();
  uint32_t actor = 0;
  const char* name = nullptr;
  if (skin == "robot") {
    actor = kRobotBanjoActor;
    name = "robotbanjo_actor";
  } else if (skin == "tuxedo") {
    actor = kTuxedoBanjoActor;
    name = "tuxedobanjo_actor";
  } else {
    return true;
  }
  if (!GuestStringIs(actor, name)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      REXLOG_CAT_WARN(rex::log::renut, "Renut: cheat skin '{}' skipped, {:08X} does not hold \"{}\"", skin, actor, name);
    return true;
  }
  r3.u32 = actor;
  r5.u32 = 0;
  return false;
}
