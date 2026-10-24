// The game above 30 FPS. The console held Nuts & Bolts to 30; three things in it assume that, and show
// at the 60 the target can ask for. Ported from reNut's game/frameHooks.cpp, whose research this is.
//
//  - Physics: Havok steps the world in fixed 1/60 s steps (renut_physics_whole_steps).
//  - Klungo's arcade: its games run their logic once a frame and play too fast.
//  - The Saucer of Peril Returns: the ride never ends.
//
// The last two hold the game at 30 FPS while they run (plume/frame_pacing.h, SetHold).

#include "guest_address.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "plume/frame_pacing.h"

REXCVAR_DEFINE_BOOL(renut_physics_whole_steps, true, "reNut",
                    "Advance physics by whole 1/60 s steps each frame, carrying the remainder, as the console's "
                    "30 fps did. Off: the game's own stepping, which flings Banjo about on ledges, tightropes "
                    "and ladders at 60 fps. At 30 fps both are the same.");

namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(renut::GuestPtr(base, address)));
}

void Hold(uint32_t reason, const char* what, bool on) {
  if (renut::pacing::SetHold(reason, on))
    REXLOG_INFO("Renut: {} {} the game at 30 fps (target {})", what, on ? "holds" : "releases",
                renut::pacing::UserTarget());
}

// ---------------------------------------------------------------------------------------------------
// Physics. N&B steps its Havok world in fixed 1/60 s steps on its own thread (sub_82386168): the frame
// sets a time marker to the current time plus the frame's delta (at most four steps) and the thread
// steps a whole step at a time until it reaches the marker. On the console's vsynced 30 fps every
// frame is two whole steps and ends on a step. At 60 fps a frame is one step give or take the timer's
// jitter: a frame a hair short still takes a step, one a hair long takes two, and the world drifts
// ahead of the marker it is shown at. Whatever the game pins in place each frame (Banjo on a ledge, a
// tightrope or a ladder) is then flung about. So the delta is sent as whole steps - the nearest
// number to it plus what earlier frames left over - and every frame ends on a step, as on the console.

constexpr float kPhysicsStep = 0.016666668f;  // the step sub_8227D3B8 sends
constexpr int kMaxPhysicsSteps = 4;           // the physics thread's cap
constexpr uint32_t kPhysicsWorldOffset = 60;
constexpr uint32_t kPhysicsDueOffset = 232;   // cleared when the step is sent
double g_physicsCarry = 0.0;

}  // namespace

// void physicsStepSend(game*, ?, float frameDelta): queues the physics thread's step (command 8: the
// delta and the 1/60 s step) from the game tick (sub_82261DD0, delta = time scale x frame delta) and
// from sub_8229B330.
extern "C" void __imp__sub_8227D3B8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8227D3B8) {
  const uint32_t game = ctx.r3.u32;
  const bool due = Load32(base, game + kPhysicsWorldOffset) && Load32(base, game + kPhysicsDueOffset);
  if (!due || !REXCVAR_GET(renut_physics_whole_steps)) {
    __imp__sub_8227D3B8(ctx, base);
    return;
  }
  const double wanted = ctx.f1.f64 + g_physicsCarry;
  const int steps = std::clamp(int(std::floor(wanted / kPhysicsStep + 0.5)), 0, kMaxPhysicsSteps);
  const double sent = double(float(steps * kPhysicsStep));
  ctx.f1.f64 = sent;
  __imp__sub_8227D3B8(ctx, base);
  // Sent unless the game had no physics world for this thread.
  if (!Load32(base, game + kPhysicsDueOffset))
    g_physicsCarry = std::clamp(wanted - sent, -double(kPhysicsStep), double(kPhysicsStep));
}

// ---------------------------------------------------------------------------------------------------
// The Saucer of Peril Returns (Terrarium of Terror, act 2 game 3) never ends above 30 fps: the saucer
// ride keeps going. The challenge manager (challengeManBegin, sub_8251FF78) keeps the running
// challenge's asset id at +12, and challengeManSetState (sub_82521018) clears it at state 0 (idle).
namespace {
constexpr uint32_t kSaucerOfPerilChallenge = 0x3DDFA82A;  // aid_challenge_banjox_terrorium_act2_game3
constexpr uint32_t kChallengeIdOffset = 12;
}  // namespace

// void challengeManTick(challengeMan*), once a frame from sub_82238360.
extern "C" void __imp__sub_821ED7F8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821ED7F8) {
  const uint32_t manager = ctx.r3.u32;
  __imp__sub_821ED7F8(ctx, base);
  Hold(renut::pacing::kHoldSaucerOfPeril, "The Saucer of Peril Returns",
       Load32(base, manager + kChallengeIdOffset) == kSaucerOfPerilChallenge);
}

// void challengeManTeardown(challengeMan*, bool). The tick may stop with the id still set (leaving
// the world), so the hold ends here too.
extern "C" void __imp__sub_825214E0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_825214E0) {
  __imp__sub_825214E0(ctx, base);
  Hold(renut::pacing::kHoldSaucerOfPeril, "The Saucer of Peril Returns", false);
}

// ---------------------------------------------------------------------------------------------------
// Klungo's arcade runs its games' logic once per frame, so they play too fast above 30 fps. Mid-function
// hooks (config/renut_android_hooks.toml) at the end of the arcade's constructor and of its destructor.
void renut_klungo_arcade_begin() {
  Hold(renut::pacing::kHoldKlungoArcade, "Klungo's arcade", true);
}

void renut_klungo_arcade_end() {
  Hold(renut::pacing::kHoldKlungoArcade, "Klungo's arcade", false);
}
