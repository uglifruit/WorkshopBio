// engines.h — the five BioMimicry physics engines.
//
// Each is a self-contained model with no allocation and no shared state; the
// card owns one instance of each and points at the active one.

#pragma once
#include "biomimicry.h"

namespace bio {

/// Mode 1 — one stride clock driving four hooves at real equine phase offsets.
/// Knob Main picks the gait (walk / trot / canter / gallop) and the stride rate
/// within that gait's own band. Chaos jitters each hoof's timing without
/// breaking the gait's phase relationships.
class HorsesEngine : public Engine
{
public:
	void reset(uint32_t seed) override;
	void tick(const Ctrl &c, EngineOut &out) override;
private:
	uint32_t stride_;                 // the single master stride clock
	int32_t  jitter_[kNumAgents];     // per-hoof timing offset, Q16
	uint8_t  lastStep_[kNumAgents];
	uint8_t  lastGait_;
	uint32_t rng_;
};

/// Mode 2 — cascading probability network. A node firing raises every other
/// node's probability via a decaying excitation, so one honk sets off the flock.
/// A refractory period after each fire keeps the cascade from self-sustaining.
class GeeseEngine : public Engine
{
public:
	void reset(uint32_t seed) override;
	void tick(const Ctrl &c, EngineOut &out) override;
private:
	// Runs kSwarmSize birds folded onto kNumAgents outputs — four geese never
	// sounded like a flock.
	int32_t  excite_[kSwarmSize];     // Q16 incoming excitation
	uint16_t refractory_[kSwarmSize]; // control ticks remaining
	uint32_t rng_;
};

/// Mode 3 — Kuramoto coupled oscillators. Each agent pulls the others toward
/// its phase with strength K; Knob Main inversely sets K, sweeping from locked
/// metronomic sync to fully independent chaos.
class FrogsEngine : public Engine
{
public:
	void reset(uint32_t seed) override;
	void tick(const Ctrl &c, EngineOut &out) override;
private:
	uint32_t phase_[kNumAgents];
	int32_t  natural_[kNumAgents];  // per-agent natural frequency increment
	uint32_t clockPhase_;           // phantom oscillator tracking Pulse In 2
	uint32_t rng_;
};

/// Mode 4 — leaky integrate-and-fire buckets. Noise fills each bucket, a fixed
/// leak drains it, and crossing the threshold fires and empties it. Staggered
/// thresholds and leak rates stop the four from locking together.
class RainEngine : public Engine
{
public:
	void reset(uint32_t seed) override;
	void tick(const Ctrl &c, EngineOut &out) override;
private:
	int32_t  level_[kNumAgents];    // Q16 accumulator
	uint32_t rng_;
};

/// Mode 5 — inhomogeneous Poisson process. A single hidden random-walk LFO sets
/// a shared firing density, so all agents cluster and fall silent together.
class MeteorsEngine : public Engine
{
public:
	void reset(uint32_t seed) override;
	void tick(const Ctrl &c, EngineOut &out) override;
private:
	int32_t  density_;      // Q16, the slewed "debris field" level
	int32_t  target_;       // Q16, current random-walk target
	uint16_t untilNewTarget_;
	uint32_t rng_;
};

/// Mode 6 — density-dependent chorus. Where Geese couple on excitation and Frogs
/// couple on phase, cicadas couple on AMPLITUDE: how loud the field already is
/// drives how fast each insect calls, and that feedback loop is what produces
/// the slow pulsing waves of loudness a real cicada field makes. Positive
/// feedback swells the chorus, fatigue drags it back down, and it breathes.
class CicadasEngine : public Engine
{
public:
	void reset(uint32_t seed) override;
	void tick(const Ctrl &c, EngineOut &out) override;
private:
	uint32_t phase_[kSwarmSize];    // each insect's call cycle
	int32_t  fatigue_[kSwarmSize];  // Q16, rises with calling, forces a rest
	int32_t  field_;                // Q16, the perceived loudness of the chorus
	uint32_t rng_;
};

} // namespace bio
