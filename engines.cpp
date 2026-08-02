// engines.cpp — the five physics models.
//
// All five run at kCtrlRate (1500Hz). Integer-only; see fastmath.h for the
// Q15/Q16/phase conventions. Every engine fills EngineOut::state[] with a
// continuous 0..65536 view of its internals, which the Summed routing mode
// puts out as CV.

#include "engines.h"

namespace bio {

// ===========================================================================
// Mode 1 — Horses: phase-drifting clocks + gaits
// ===========================================================================

// Leg-length offsets. Deliberately close to 1.0 and mutually irrational-ish so
// the four clocks take a long time to realign — that slow drift is the sound.
static const int32_t kHorseRatio[kNumAgents] = {
	65536,        // 1.00x
	64225,        // 0.98x
	67502,        // 1.03x
	62259         // 0.95x
};

// Gait step tables: gait[g][step] = bitmask of agents firing on that step.
// Bit i = agent i.
static const uint8_t kGait[4][4] = {
	// Walk: 1 - 2 - 3 - 4, evenly spaced.
	{ 0b0001, 0b0010, 0b0100, 0b1000 },
	// Trot: diagonal pairs together.
	{ 0b0011, 0b0000, 0b1100, 0b0000 },
	// Canter: 1 - [2+3] - 4 - rest, asymmetric three-beat.
	{ 0b0001, 0b0110, 0b1000, 0b0000 },
	// Gallop: four-hoof burst then suspension.
	{ 0b0001, 0b0010, 0b0100, 0b1000 }
};

// Gallop compresses its four hits into the first half of the cycle, leaving a
// silent "suspension" phase — the step index advances twice as fast and then
// rests. This table says which steps are live per gait quadrant.
static const uint8_t kGaitSteps[4] = { 4, 4, 4, 4 };

void HorsesEngine::reset(uint32_t seed)
{
	rng_ = seed | 1u;
	for (int i = 0; i < kNumAgents; i++)
	{
		phase_[i] = 0;
		lastStep_[i] = 0xFF;
	}
}

void HorsesEngine::tick(const Ctrl &c, EngineOut &out)
{
	out.triggers = 0;

	// Pulse In = reset every phase to 0: all legs land together, then drift
	// apart again. The big unified flam is the point.
	if (c.spook)
	{
		for (int i = 0; i < kNumAgents; i++) { phase_[i] = 0; lastStep_[i] = 0xFF; }
	}

	// Knob Main picks the gait (quadrant) and the tempo within it.
	int gait = c.physics >> 14;               // 0..3
	if (gait > 3) gait = 3;

	// Tempo: roughly 0.6Hz (plod) to 9Hz (gallop) per cycle. Each gait band
	// sweeps its own range, so the knob accelerates continuously across the
	// whole sweep rather than jumping at the boundaries.
	int32_t within = (c.physics & 0x3FFF) << 2;              // 0..65535 in-band
	int32_t hz_q8  = 150 + ((gait * 480) + mul_q16(within, 480)); // Q8 Hz
	uint32_t inc   = static_cast<uint32_t>(
		(static_cast<int64_t>(hz_q8) * 4294967296LL) / (256LL * kCtrlRate));

	// Chaos adds a little per-agent speed wobble so a machine-perfect gait
	// still breathes.
	for (int i = 0; i < kNumAgents; i++)
	{
		uint32_t agentInc = static_cast<uint32_t>(
			(static_cast<int64_t>(inc) * kHorseRatio[i]) >> 16);

		if (c.chaos > 0)
		{
			// Wobble each leg's speed by up to +/-25% of the base increment,
			// scaled by chaos. Applied as a proportion of agentInc so the
			// swing stays musical at every tempo.
			int32_t wob = rand_bipolar(rng_);                    // +/-16384
			int32_t amt = mul_q16(wob, c.chaos);                 // +/-16384
			int32_t delta = static_cast<int32_t>(
				(static_cast<int64_t>(agentInc) * amt) >> 16);   // +/-25%
			agentInc = static_cast<uint32_t>(static_cast<int32_t>(agentInc) + delta);
		}

		phase_[i] += agentInc;

		// Which quarter of the cycle are we in? Crossing into a new step fires
		// whichever hooves that gait puts on that step.
		uint8_t step = static_cast<uint8_t>(phase_[i] >> 30);  // 0..3
		if (step != lastStep_[i])
		{
			lastStep_[i] = step;
			if (kGait[gait][step] & (1 << i)) out.triggers |= (1 << i);
		}

		// State CV: the raw phase ramp, so patching it out gives a per-leg LFO.
		out.state[i] = static_cast<int32_t>(phase_[i] >> 16);
	}

	// Global: agent 0's phase, a master clock reference.
	out.global = static_cast<int32_t>(phase_[0] >> 16);
	(void)kGaitSteps;
}

// ===========================================================================
// Mode 2 — Geese: stochastic contagion
// ===========================================================================

// Refractory period after a honk: no re-fire for ~60ms. Without this the
// mutual excitation would latch into a continuous tone instead of a cascade.
static constexpr uint16_t kGooseRefractory = kCtrlRate / 16; // ~94 ticks

void GeeseEngine::reset(uint32_t seed)
{
	rng_ = seed | 1u;
	for (int i = 0; i < kNumAgents; i++) { excite_[i] = 0; refractory_[i] = 0; }
}

void GeeseEngine::tick(const Ctrl &c, EngineOut &out)
{
	out.triggers = 0;

	// Pulse In = spook the flock: a huge excitation spike into every node at
	// once, guaranteeing a cascade.
	if (c.spook)
		for (int i = 0; i < kNumAgents; i++) excite_[i] = kQ16One;

	// Baseline spontaneous spark chance per tick, scaled up as the flock shrinks.
	// A cascade needs someone to honk first; with four birds a spark somewhere
	// is quick, but with one or two the wait for ignition would stretch to tens
	// of seconds and the mode would read as broken. Chaos raises it further.
	int32_t spark = (2 + (c.chaos >> 12)) * (kNumAgents + 1 - c.population);

	// Contagion strength from Knob Main. Mildly shaped (x^1.5-ish via a blend of
	// linear and squared) so the knob keeps resolution down low without going
	// dead across the middle of its travel.
	int32_t contagion = (c.physics + mul_q16(c.physics, c.physics)) >> 1;

	uint8_t fired = 0;
	for (int i = 0; i < kNumAgents; i++)
	{
		if (i >= c.population) { out.state[i] = 0; continue; }

		if (refractory_[i] > 0)
		{
			refractory_[i]--;
		}
		else
		{
			// Probability = baseline spark + whatever excitation the flock has
			// pushed into this node. The >>2 keeps a fully-excited node at a
			// ~25% per-tick chance rather than a certainty, so a cascade stays
			// a flurry of distinct honks instead of a solid buzz.
			int32_t p = spark + (mul_q16(excite_[i], contagion) >> 2);
			if (rand_q16(rng_) < p)
			{
				fired |= (1 << i);
				refractory_[i] = kGooseRefractory;
			}
		}

		out.state[i] = excite_[i];
	}

	// A node that fired excites all the *others* — never itself, or it would
	// just retrigger the moment its refractory period ended.
	if (fired)
	{
		for (int i = 0; i < kNumAgents; i++)
		{
			if (!(fired & (1 << i))) continue;
			for (int j = 0; j < kNumAgents; j++)
			{
				if (j == i) continue;
				// A partial nudge, not a slam to full scale: excitement has to
				// ACCUMULATE across several honks to tip the flock over. That
				// build-up is what makes the cascade audible as a cascade.
				excite_[j] += kQ16One / 3;
				if (excite_[j] > kQ16One) excite_[j] = kQ16One;
			}
		}
	}
	out.triggers = fired;

	// Excitation decays fast — a honk's influence lasts a couple hundred ms.
	int32_t sum = 0;
	for (int i = 0; i < kNumAgents; i++)
	{
		excite_[i] = fast_exp_decay(excite_[i], 6);
		sum += excite_[i];
	}
	out.global = sum >> 2;   // mean flock agitation
}

// ===========================================================================
// Mode 3 — Frogs: Kuramoto coupled oscillators
// ===========================================================================
//
//   dTheta_i/dt = omega_i + (K/N) * sum_j sin(Theta_j - Theta_i)
//
// Phases live in uint32_t so the wrap is free and sin() is a table lookup.

void FrogsEngine::reset(uint32_t seed)
{
	rng_ = seed | 1u;
	for (int i = 0; i < kNumAgents; i++)
	{
		phase_[i] = xorshift32(rng_);
		natural_[i] = 0;
	}
}

void FrogsEngine::tick(const Ctrl &c, EngineOut &out)
{
	out.triggers = 0;

	// Pulse In = splash: scramble every phase. Sync shatters, then rebuilds at
	// whatever rate K allows — the most satisfying gesture on the card.
	if (c.spook)
		for (int i = 0; i < kNumAgents; i++) phase_[i] = xorshift32(rng_);

	// Base chorus rate ~1.2Hz to ~6Hz on Knob Main's lower influence; the knob
	// here is coupling, so tempo comes from a fixed base plus chaos spread.
	constexpr int32_t kBaseHz_q8 = 400;   // ~1.56 Hz
	int32_t baseInc = static_cast<int32_t>(
		(static_cast<int64_t>(kBaseHz_q8) * 4294967296LL) / (256LL * kCtrlRate));

	// Knob Main INVERSELY controls coupling: 0.0 = maximum K (locked sync),
	// 1.0 = K of zero (every frog for itself).
	//
	// Cubed, because sync is stubborn: even a little coupling drags the chorus
	// into lock, so a linear knob would stay synced until the very last hair of
	// travel and then snap. Cubing pushes the sync/chaos transition out into the
	// middle of the sweep where it can be played.
	int32_t inv = kQ16One - c.physics;
	int32_t K = mul_q16(mul_q16(inv, inv), inv);

	// Chaos spreads the natural frequencies apart, making sync harder to reach.
	for (int i = 0; i < kNumAgents; i++)
	{
		// Detune each frog by up to ~+/-12% scaled by chaos, fixed per agent.
		static const int32_t kDetune[kNumAgents] = { 0, 3000, -2200, 5000 };
		natural_[i] = baseInc + ((baseInc >> 4) * mul_q16(kDetune[i], c.chaos) >> 12);
	}

	int n = c.population;
	if (n < 1) n = 1;

	for (int i = 0; i < kNumAgents; i++)
	{
		if (i >= n) { out.state[i] = 0; continue; }

		// Coupling term: pull toward every other active frog's phase.
		// sin(theta_j - theta_i) is positive when j is ahead, so a lagging frog
		// speeds up and a leading one slows down — that is the whole model.
		// Mean of the neighbours, kept in Q15 (+/-32767).
		int32_t coupling = 0;
		for (int j = 0; j < n; j++)
		{
			if (j == i) continue;
			coupling += fast_sin(phase_[j] - phase_[i]);
		}
		if (n > 1) coupling /= (n - 1);

		// Modulate this frog's RATE by up to +/-100% at full K. That much
		// authority is what actually drags the chorus into lock; the old
		// version could only bend the rate ~2% and never synchronised.
		int32_t adjust = static_cast<int32_t>(
			(static_cast<int64_t>(natural_[i]) * mul_q15(coupling, K >> 1)) >> 16);

		uint32_t before = phase_[i];
		phase_[i] += static_cast<uint32_t>(natural_[i] + adjust);

		// Wrap = one full croak cycle completed.
		if (phase_[i] < before) out.triggers |= (1 << i);

		out.state[i] = static_cast<int32_t>(phase_[i] >> 16);
	}

	// Global = order parameter, cheaply approximated: how tightly the phases
	// cluster around agent 0. 65536 = perfect sync, 0 = evenly scattered.
	int32_t coh = 0;
	for (int i = 1; i < n; i++)
		coh += fast_sin((phase_[i] - phase_[0]) / 2 + 0x40000000);
	out.global = (n > 1) ? ((coh / (n - 1)) + kQ15One) : kQ16One;
}

// ===========================================================================
// Mode 4 — Rain: leaky integrate-and-fire
// ===========================================================================

// Staggered thresholds. If all four matched, identical noise statistics would
// make them fire in lockstep; the spread is what makes it a drum circle.
static const int32_t kRainThreshold[kNumAgents] = {
	kQ16One, (kQ16One * 115) / 100, (kQ16One * 88) / 100, (kQ16One * 103) / 100
};
// ...and staggered leak shifts (larger = leakier bucket drains slower).
static const uint8_t kRainLeak[kNumAgents] = { 9, 8, 10, 9 };

void RainEngine::reset(uint32_t seed)
{
	rng_ = seed | 1u;
	for (int i = 0; i < kNumAgents; i++) level_[i] = 0;
}

void RainEngine::tick(const Ctrl &c, EngineOut &out)
{
	out.triggers = 0;

	// Pulse In = wind gust: dump a big random slug into every bucket, tipping
	// several over the edge at once.
	if (c.spook)
		for (int i = 0; i < kNumAgents; i++)
			level_[i] += (kQ16One >> 1) + (rand_q16(rng_) >> 1);

	// Downpour: how hard it's raining into the buckets. The leak has to be
	// overcome before anything fires at all, so the knob is offset to put the
	// silence-to-drizzle threshold near the bottom of its travel rather than
	// halfway up. Mildly shaped to keep resolution in the sparse region, which
	// is where the mode is most interesting.
	// A modest offset lifts the bottom of the knob just over the leak, then the
	// response stays linear-ish to the top so the last quarter of travel still
	// adds intensity instead of flattening out.
	int32_t x = c.physics;
	int32_t downpour = (x >> 2) + ((x + mul_q16(x, x)) >> 2);
	if (downpour > kQ16One) downpour = kQ16One;

	int32_t sum = 0;
	for (int i = 0; i < kNumAgents; i++)
	{
		if (i >= c.population) { out.state[i] = 0; continue; }

		// Noisy inflow. The chaos knob widens the variance of each drop.
		// The >>5 sets the ceiling: full downpour lands around 25 drips/sec per
		// bucket, a dense torrent that is still a rhythm rather than a buzz.
		int32_t drop = mul_q16(rand_q16(rng_), downpour) >> 5;
		if (c.chaos > 0)
			drop += mul_q16(rand_q16(rng_) - (kQ16One / 2), c.chaos) >> 4;
		if (drop < 0) drop = 0;

		level_[i] += drop;
		level_[i] = fast_exp_decay(level_[i], kRainLeak[i]);   // constant leak

		if (level_[i] >= kRainThreshold[i])
		{
			out.triggers |= (1 << i);
			level_[i] = 0;
		}

		// State CV = the bucket filling and emptying: a natural ramp LFO.
		out.state[i] = (level_[i] > kQ16One) ? kQ16One : level_[i];
		sum += out.state[i];
	}
	out.global = sum >> 2;
}

// ===========================================================================
// Mode 5 — Meteors: inhomogeneous Poisson process
// ===========================================================================

void MeteorsEngine::reset(uint32_t seed)
{
	rng_ = seed | 1u;
	density_ = 0;
	target_ = 0;
	untilNewTarget_ = 1;
}

void MeteorsEngine::tick(const Ctrl &c, EngineOut &out)
{
	out.triggers = 0;

	// The hidden debris field: a slow random walk, re-aimed every 1-4 seconds
	// and slewed toward smoothly. This is what makes activity arrive in waves
	// rather than at a constant rate.
	if (--untilNewTarget_ == 0)
	{
		target_ = rand_q16(rng_);
		untilNewTarget_ = static_cast<uint16_t>(
			kCtrlRate + (xorshift32(rng_) % (3u * kCtrlRate)));
	}
	density_ = slew(density_, target_, 9);

	// Pulse In = bolide: slam the density to maximum for an immediate burst.
	// It then slews back down to the underlying walk on its own.
	if (c.spook) density_ = kQ16One;

	// Knob Main sets both the floor and how much the walk is allowed to swing.
	// Low: rare isolated hits. Mid: long silences swelling into dense waves.
	// High: locked-high barrage.
	int32_t depth, floorLevel;
	if (c.physics < kQ16One / 2)
	{
		// Lower half: swing grows from nothing, floor stays at zero.
		depth = c.physics * 2;
		floorLevel = 0;
	}
	else
	{
		// Upper half: swing shrinks as the floor rises to a constant barrage.
		int32_t t = (c.physics - kQ16One / 2) * 2;
		depth = kQ16One - t;
		floorLevel = t;
	}

	int32_t effective = floorLevel + mul_q16(density_, depth);

	// Convert the 0..1 density into a per-tick firing probability. Squared for
	// a natural swell, then shifted so even a locked-high field tops out around
	// 12 strikes/sec per voice — a heavy barrage that still reads as discrete
	// meteors rather than a continuous hiss.
	int32_t p = mul_q16(effective, effective) >> 7;
	if (c.chaos > 0) p += mul_q16(rand_q16(rng_), c.chaos) >> 10;

	for (int i = 0; i < kNumAgents; i++)
	{
		if (i >= c.population) { out.state[i] = 0; continue; }
		if (rand_q16(rng_) < p) out.triggers |= (1 << i);
		out.state[i] = effective;
	}
	out.global = effective;
}

} // namespace bio
