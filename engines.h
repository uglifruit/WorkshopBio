// engines.h — the five BioMimicry physics engines.
//
// Each is a self-contained model with no allocation and no shared state; the
// card owns one instance of each and points at the active one.

#pragma once
#include "biomimicry.h"

namespace bio {

/// Mode 1 — polyrhythmic clocks with per-leg speed offsets and gait patterns.
/// Four phase accumulators run at fixed ratios (1.00/0.98/1.03/0.95) so they
/// drift in and out of alignment forever. Knob Main sets tempo and picks one of
/// four gait step-tables (walk/trot/canter/gallop).
class HorsesEngine : public Engine
{
public:
	void reset(uint32_t seed) override;
	void tick(const Ctrl &c, EngineOut &out) override;
private:
	uint32_t phase_[kNumAgents];
	uint8_t  lastStep_[kNumAgents];
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
	int32_t  excite_[kNumAgents];   // Q16 incoming excitation
	uint16_t refractory_[kNumAgents]; // control ticks remaining
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

} // namespace bio
