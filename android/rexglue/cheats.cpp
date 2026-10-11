// Cheat hooks of the recompiled game. config/renut_android_hooks.toml makes the
// generated code call these at fixed guest addresses; the Android app turns them
// on through renut.toml (Jogar > Cheats), so they apply from the next start.

#include <atomic>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(renut_cheats, false, "reNut/Cheats",
                    "Master switch of the cheats below; none applies while it is off");
REXCVAR_DEFINE_BOOL(renut_cheat_infinite_health, false, "reNut/Cheats", "Health never decreases");
REXCVAR_DEFINE_BOOL(renut_cheat_infinite_fuel_ammo, false, "reNut/Cheats",
                    "Vehicle fuel and ammo are not consumed");
REXCVAR_DEFINE_BOOL(renut_cheat_infinite_parts, false, "reNut/Cheats",
                    "Using a vehicle part never lowers its count");
REXCVAR_DEFINE_BOOL(renut_cheat_no_timer, false, "reNut/Cheats",
                    "Mission timers never run out");
REXCVAR_DEFINE_BOOL(renut_cheat_no_notes_spent, false, "reNut/Cheats", "Spending notes costs none");
REXCVAR_DEFINE_BOOL(renut_cheat_vehicles_anywhere, false, "reNut/Cheats",
                    "Change and build vehicles from the pause menu outside Showdown Town");
REXCVAR_DEFINE_BOOL(renut_cheat_extended_build_range, false, "reNut/Cheats",
                    "A larger area to build in at Mumbo's Motors");
REXCVAR_DEFINE_BOOL(renut_disable_cao, false, "reNut/Graphics",
                    "Do not draw the dark contact patches under characters (the game's CAO pass)");
REXCVAR_DEFINE_STRING(renut_cheat_banjo_skin, "default", "reNut/Cheats", "Banjo's outfit")
    .allowed({"default", "robot", "tuxedo"});

namespace {

#define RENUT_CHEAT_ON(flag) (REXCVAR_GET(renut_cheats) && REXCVAR_GET(flag))

// The guest strings the skin hook points r3 at. Checked before use: an address
// that does not hold the expected name is left alone instead of spawning garbage.
constexpr uint32_t kRobotBanjoActor = 0x8216B690;
constexpr uint32_t kTuxedoBanjoActor = 0x8216B6A4;

bool GuestStringIs(uint32_t address, const char* expected) {
  auto* runtime = rex::Runtime::instance();
  const uint8_t* bytes = runtime ? runtime->virtual_membase() : nullptr;
  return bytes && std::strcmp(reinterpret_cast<const char*>(bytes + address), expected) == 0;
}

}  // namespace

// For the runtime report: which cheats this start applies, "" when none.
std::string renutCheatsSummary() {
  if (!REXCVAR_GET(renut_cheats)) return {};
  std::string list;
  auto add = [&](bool on, const char* name) {
    if (!on) return;
    if (!list.empty()) list += ", ";
    list += name;
  };
  add(REXCVAR_GET(renut_cheat_infinite_health), "vida");
  add(REXCVAR_GET(renut_cheat_infinite_fuel_ammo), "combustível/munição");
  add(REXCVAR_GET(renut_cheat_infinite_parts), "peças");
  add(REXCVAR_GET(renut_cheat_no_timer), "sem tempo");
  add(REXCVAR_GET(renut_cheat_no_notes_spent), "notas");
  add(REXCVAR_GET(renut_cheat_vehicles_anywhere), "veículos em qualquer lugar");
  add(REXCVAR_GET(renut_cheat_extended_build_range), "área de montagem");
  add(REXCVAR_GET(renut_cheat_banjo_skin) != "default", ("visual " + REXCVAR_GET(renut_cheat_banjo_skin)).c_str());
  return list.empty() ? "ligados, nenhum selecionado" : list;
}

// Hooks are declared by the generated code with C++ linkage, exactly as below.

// A graphics option, not a cheat: true jumps past the draw of one contact patch.
bool renut_disable_cao() { return REXCVAR_GET(renut_disable_cao); }

bool renut_cheat_infinite_health() { return RENUT_CHEAT_ON(renut_cheat_infinite_health); }

bool renut_cheat_infinite_fuel_ammo() { return RENUT_CHEAT_ON(renut_cheat_infinite_fuel_ammo); }

void renut_cheat_infinite_parts(PPCRegister& r10) {
  if (RENUT_CHEAT_ON(renut_cheat_infinite_parts)) r10.u32 = 100;
}

bool renut_cheat_no_timer() { return RENUT_CHEAT_ON(renut_cheat_no_timer); }

bool renut_cheat_no_notes_spent() { return RENUT_CHEAT_ON(renut_cheat_no_notes_spent); }

void renut_cheat_vehicles_anywhere(PPCRegister& r23, PPCRegister& r26) {
  if (!RENUT_CHEAT_ON(renut_cheat_vehicles_anywhere)) return;
  r23.u32 = 1;  // Build Vehicle
  r26.u8 = 1;   // Change Vehicle
}

bool renut_cheat_extended_build_range() {
  return RENUT_CHEAT_ON(renut_cheat_extended_build_range);
}

// True keeps the game's own choice of actor; false jumps past it with the
// outfit's actor already in r3.
bool renut_cheat_banjo_skin(PPCRegister& r3, PPCRegister& r5) {
  if (!REXCVAR_GET(renut_cheats)) return true;
  const std::string& skin = REXCVAR_GET(renut_cheat_banjo_skin);
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
      REXLOG_WARN("Renut: cheat skin '{}' skipped, {:08X} does not hold \"{}\"", skin, actor, name);
    return true;
  }
  r3.u32 = actor;
  r5.u32 = 0;
  return false;
}
