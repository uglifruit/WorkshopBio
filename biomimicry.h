// biomimicry.h — shared types for the BioMimicry card.
//
// Five physics engines drive four agents. Every engine implements the same
// contract: given the control state, advance the model by one control tick and
// return a bitmask of which agents fired.

#pragma once
#include <stdint.h>
#include "fastmath.h"

namespace bio {

/// Number of independent agents (voices/nodes/buckets) in every engine.
constexpr int kNumAgents = 4;

/// ProcessSample() runs at 48kHz; the physics run every kCtrlDiv samples.
/// 48000/32 = 1500Hz control rate — 0.67ms timing granularity, far finer than
/// the ear resolves for triggers, and 32x cheaper than running physics at
/// audio rate.
constexpr int   kCtrlDiv  = 32;
constexpr int   kSampleRate = 48000;
constexpr int   kCtrlRate = kSampleRate / kCtrlDiv; // 1500 Hz

/// The five ecosystems, in cycling order.
enum class Mode : uint8_t
{
	Horses = 0,
	Geese,
	Frogs,
	Rain,
	Meteors,
	Count
};
constexpr int kNumModes = static_cast<int>(Mode::Count);

/// Gate routing, selected by the toggle position.
enum class Routing : uint8_t
{
	Discrete, // Switch Up:     agents -> individual outputs
	Summed    // Switch Middle: agents OR'd -> Pulse 1, CV outs carry state
};

/// Control state, resampled once per control tick and handed to the engine.
/// All the Q16 fields are 0..65536.
struct Ctrl
{
	int32_t physics;    // Knob Main (+ CV In 1): the per-mode physics variable
	int32_t chaos;      // Knob Y: global randomness / spread
	int     population; // Knob X (+ CV In 2): 1..kNumAgents active agents
	bool    spook;      // Pulse In 1 rising edge this tick
	bool    clock;      // Pulse In 2 rising edge this tick
};

/// What an engine produces each control tick.
struct EngineOut
{
	uint8_t triggers;              // bit i set = agent i fired this tick
	int32_t state[kNumAgents];     // Q16 0..65536, continuous internal state,
	                               // exposed on the CV outs in Summed routing
	int32_t global;                // Q16, a whole-ecosystem value (density,
	                               // sync coherence, ...) for CV Out 2
};

/// Common base for the five engines. Virtual dispatch happens once per control
/// tick (1500Hz), not per sample, so the indirect call is irrelevant.
class Engine
{
public:
	virtual ~Engine() {}

	/// Re-seed / reset to a sane starting state. Called on mode change.
	virtual void reset(uint32_t seed) = 0;

	/// Advance one control tick.
	virtual void tick(const Ctrl &c, EngineOut &out) = 0;
};

} // namespace bio
