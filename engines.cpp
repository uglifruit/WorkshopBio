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
// Real gaits are PHASE-LOCKED: a trotting horse's diagonal pair stays a pair,
// and that lock is what makes it a trot. So ONE master clock per stride drives
// all four hooves, each landing at a fixed offset within the stride. Leg
// asymmetry becomes small per-hoof jitter on top — not four free-running clocks,
// which pulled every gait apart within seconds.
//
// Agent order is the standard equine convention:
//   0 = LH (left hind)    1 = LF (left fore)
//   2 = RH (right hind)   3 = RF (right fore)
//
// Each entry is the fraction of the stride at which that hoof LANDS, Q16
// (65536 = one full stride). A hoof fires when (stride - offset) wraps, so the
// numbers below read directly as footfall times — 0 is the start of the stride.
static const int32_t kGaitOffset[4][kNumAgents] = {
	//                LH       LF       RH       RF
	// WALK — 4-beat lateral, evenly spaced. The two legs of ONE SIDE follow each
	// other: LH, LF, RH, RF. (The original table fired both forelegs back to
	// back, which no horse does.)
	//   LH 0 · LF 0.25 · RH 0.50 · RF 0.75
	{                  0,   16384,   32768,   49152 },

	// TROT — 2-beat diagonal. Diagonal pairs land together, half a stride apart:
	// LH+RF at 0, LF+RH at 1/2.
	{                  0,   32768,   32768,       0 },

	// CANTER — 3-beat with suspension, on the right lead: leading hind, then the
	// diagonal pair, then the leading fore, then all four airborne. The beats
	// CLUSTER into the first ~45% and leave a real float; even spacing is what
	// made an earlier version sound like a waltz instead of a canter.
	//   LH 0 · RH+LF 0.22 · RF 0.44 · float 0.44-1.0
	{                  0,   14418,   14418,   28836 },

	// GALLOP — 4-beat rotary with a long suspension. The two HINDS drive first,
	// then the forelegs catch the fall, then all four are airborne. Hinds before
	// fores is exactly what separates a gallop from a fast walk.
	//   LH 0 · RH 0.10 · LF 0.21 · RF 0.31 · float 0.31-1.0
	{                  0,   13763,    6554,   20315 }
};

// Stride rate range per gait, Q8 Hz. A real horse doesn't just do the same
// tempo faster — each gait occupies its own band, and a gallop stride is only
// ~2.5x a walk stride even though it covers far more ground.
static const int32_t kGaitHzMin[4] = {  115,  205,  300,  380 }; // 0.45..1.5 Hz
static const int32_t kGaitHzMax[4] = {  205,  330,  420,  640 }; // 0.8 ..2.5 Hz

// Per-horse speed offsets. Real animals travelling together never quite match
// pace, so the herd continuously slides in and out of step — that slow phasing
// between whole gaits is the sound the mode is after, and it is what the old
// per-LEG drift was trying (and failing) to do.
static const int32_t kHorseSpeed[kNumAgents] = {
	65536,        // 1.000x
	63700,        // 0.972x
	67600,        // 1.031x
	62000         // 0.946x
};

void HorsesEngine::reset(uint32_t seed)
{
	rng_ = seed | 1u;
	lastGait_ = 0xFF;
	for (int h = 0; h < kNumAgents; h++)
	{
		// Stagger the herd's starting positions so they don't begin in unison.
		stride_[h] = static_cast<uint32_t>(h) * 0x30000000u;
		speed_[h] = kHorseSpeed[h];
		for (int i = 0; i < kNumAgents; i++)
		{
			lastStep_[h][i] = 0xFF;
			jitter_[h][i] = 0;
		}
	}
}

void HorsesEngine::tick(const Ctrl &c, EngineOut &out)
{
	out.triggers = 0;

	// Pulse In = the herd startles into step: every horse resets to the top of
	// its stride at once, so they land together and then drift apart again.
	if (c.spook)
		for (int h = 0; h < kNumAgents; h++)
		{
			stride_[h] = 0;
			for (int i = 0; i < kNumAgents; i++) lastStep_[h][i] = 0xFF;
		}

	// Knob Main picks the gait (quadrant) and the stride rate within it.
	int gait = c.physics >> 14;               // 0..3
	if (gait > 3) gait = 3;

	// Changing gait re-rolls the step tracking so the new gait starts clean.
	if (gait != lastGait_)
	{
		lastGait_ = static_cast<uint8_t>(gait);
		for (int h = 0; h < kNumAgents; h++)
			for (int i = 0; i < kNumAgents; i++) lastStep_[h][i] = 0xFF;
	}

	// Each gait has its own stride-rate band; the knob sweeps within it.
	int32_t within = (c.physics & 0x3FFF) << 2;              // 0..65535 in-band
	int32_t hz_q8  = kGaitHzMin[gait] +
		mul_q16(within, kGaitHzMax[gait] - kGaitHzMin[gait]);

	// Pulse In 2 entrains the herd: every horse falls in step with the clock.
	if (c.clock && c.clockPeriod > 0)
	{
		int32_t clockHz_q8 = (256 * kCtrlRate) / c.clockPeriod;
		if (clockHz_q8 > 32) hz_q8 = clockHz_q8;
		for (int h = 0; h < kNumAgents; h++)
		{
			stride_[h] = 0;
			for (int i = 0; i < kNumAgents; i++) lastStep_[h][i] = 0xFF;
		}
	}

	uint32_t baseInc = static_cast<uint32_t>(
		(static_cast<int64_t>(hz_q8) * 4294967296LL) / (256LL * kCtrlRate));

	// One horse per output channel. Each keeps ALL FOUR hooves — a horse with
	// three legs is not a smaller herd — and runs its own stride clock, so the
	// animals phase against each other while every individual gait stays intact.
	for (int h = 0; h < kNumAgents; h++)
	{
		if (h >= c.population) { out.state[h] = 0; continue; }

		uint32_t inc = static_cast<uint32_t>(
			(static_cast<int64_t>(baseInc) * speed_[h]) >> 16);
		stride_[h] += inc;

		for (int i = 0; i < kNumAgents; i++)
		{
			// Where this hoof sits within its horse's stride. Subtracting the
			// offset means a hoof with offset 0.25 wraps a quarter of a stride
			// AFTER the stride start — so the table reads as landing times.
			uint32_t hoofPhase = stride_[h] -
				(static_cast<uint32_t>(kGaitOffset[gait][i]) << 16);

			// Chaos = per-hoof timing jitter, a real animal's unevenness.
			// Applied as a phase offset rather than a rate change, so legs keep
			// their gait relationship instead of drifting out of it.
			if (c.chaos > 0)
			{
				jitter_[h][i] = slew(jitter_[h][i],
					mul_q16(rand_bipolar(rng_), c.chaos), 5);
				hoofPhase += static_cast<uint32_t>(jitter_[h][i] << 10);
			}

			// A hoof lands when its phase wraps past the top of the stride.
			// Tracked in 16ths so the fine canter/gallop offsets resolve.
			uint8_t step = static_cast<uint8_t>(hoofPhase >> 28);   // 0..15
			if (step == 0 && lastStep_[h][i] != 0)
			{
				out.triggers |= (1 << h);
				// Tell the voice WHICH hoof this was, so it plays that hoof's
				// own sound — hinds lower and heavier than fores.
				out.member[h] = static_cast<uint8_t>(i);
			}
			lastStep_[h][i] = step;
		}

		// State CV: this horse's position in its stride — a per-animal LFO.
		out.state[h] = static_cast<int32_t>(stride_[h] >> 16);
	}

	// Global: the lead horse's stride, a reference for the whole herd.
	out.global = static_cast<int32_t>(stride_[0] >> 16);
}

// ===========================================================================
// Mode 2 — Geese: stochastic contagion
// ===========================================================================

// Refractory period after a honk: no re-fire for ~170ms. Without this the mutual
// excitation would latch into a continuous tone instead of a cascade. It also
// sets the hard ceiling on how fast one bird can honk — and since three birds
// share each output channel, that ceiling matters three times over.
static constexpr uint16_t kGooseRefractory = kCtrlRate / 6;  // ~250 ticks

void GeeseEngine::reset(uint32_t seed)
{
	rng_ = seed | 1u;
	for (int i = 0; i < kSwarmSize; i++) { excite_[i] = 0; refractory_[i] = 0; }
}

void GeeseEngine::tick(const Ctrl &c, EngineOut &out)
{
	out.triggers = 0;

	// The flock is kSwarmSize birds folded onto kNumAgents outputs: with the
	// population knob at 4, all 12 are awake and each output channel carries 3
	// of them. Four birds never sounded like a flock; twelve do.
	int flock = c.population * kSwarmPerAgent;

	// Pulse In = spook the flock: a huge excitation spike into every bird at
	// once, guaranteeing a cascade.
	if (c.spook)
		for (int i = 0; i < flock; i++) excite_[i] = kQ16One;

	// Pulse In 2 = the clock nudges the whole flock, so honks lean toward the
	// beat without being locked to it.
	if (c.clock)
		for (int i = 0; i < flock; i++)
		{
			excite_[i] += kQ16One / 3;
			if (excite_[i] > kQ16One) excite_[i] = kQ16One;
		}

	// Baseline spontaneous spark chance per tick, scaled up as the flock shrinks.
	// A cascade needs someone to honk first; with a full flock a spark somewhere
	// is quick, but with three birds the wait for ignition would stretch to tens
	// of seconds and the mode would read as broken. Chaos raises it further.
	int32_t spark = (2 + (c.chaos >> 12)) * (kNumAgents + 1 - c.population);

	// Contagion strength from Knob Main. Mildly shaped (x^1.5-ish via a blend of
	// linear and squared) so the knob keeps resolution down low without going
	// dead across the middle of its travel.
	int32_t contagion = (c.physics + mul_q16(c.physics, c.physics)) >> 1;

	// How much one honk raises every other bird's excitement. Small, because in
	// a flock of twelve each honk lands on eleven neighbours — the cascade comes
	// from many birds accumulating, not from any single honk being decisive.
	constexpr int32_t nudge = kQ16One / 16;

	uint32_t fired = 0;
	for (int i = 0; i < flock; i++)
	{
		if (refractory_[i] > 0)
		{
			refractory_[i]--;
		}
		else
		{
			// Probability = baseline spark + whatever excitation the flock has
			// pushed into this bird. The >>6 is what keeps the contagion knob
			// GRADUAL: at a smaller shift, any working contagion drove every
			// bird straight to its refractory ceiling and the knob became a
			// switch between silence and a solid 16Hz buzz.
			int32_t p = spark + (mul_q16(excite_[i], contagion) >> 6);
			if (rand_q16(rng_) < p)
			{
				fired |= (1u << i);
				refractory_[i] = kGooseRefractory;
			}
		}
	}

	// A bird that honked excites all the *others* — never itself, or it would
	// just retrigger the moment its refractory period ended.
	if (fired)
	{
		for (int i = 0; i < flock; i++)
		{
			if (!(fired & (1u << i))) continue;
			for (int j = 0; j < flock; j++)
			{
				if (j == i) continue;
				// A partial nudge, not a slam to full scale: excitement has to
				// ACCUMULATE across several honks to tip the flock over. That
				// build-up is what makes the cascade audible as a cascade.
				excite_[j] += nudge;
				if (excite_[j] > kQ16One) excite_[j] = kQ16One;
			}
		}
	}

	// Fold the flock down onto the output channels: a channel fires if any of
	// its birds did, and carries the mean excitation of its group as state CV.
	out.triggers = 0;
	for (int a = 0; a < kNumAgents; a++)
	{
		if (a >= c.population) { out.state[a] = 0; continue; }
		int32_t groupExcite = 0;
		for (int k = 0; k < kSwarmPerAgent; k++)
		{
			int i = a * kSwarmPerAgent + k;
			if (fired & (1u << i)) out.triggers |= (1 << a);
			groupExcite += excite_[i];
		}
		out.state[a] = groupExcite / kSwarmPerAgent;
	}

	// Excitation decays over roughly a second — slow enough that honks from
	// several birds ACCUMULATE into a cascade, fast enough that the flock
	// settles back to quiet between outbursts. (At the old faster decay the
	// excitation drained before it could ever build, so the contagion knob had
	// almost no effect across its range.)
	int32_t sum = 0;
	for (int i = 0; i < flock; i++)
	{
		excite_[i] = fast_exp_decay(excite_[i], 9);
		sum += excite_[i];
	}
	out.global = (flock > 0) ? (sum / flock) : 0;   // mean flock agitation
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
	clockPhase_ = 0;
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

	// Base chorus rate. The knob here is coupling, so tempo comes from a fixed
	// base plus the chaos spread.
	constexpr int32_t kBaseHz_q8 = 400;   // ~1.56 Hz
	int32_t baseHz_q8 = kBaseHz_q8;

	// Pulse In 2 entrains the chorus to an external clock. This is the most
	// musically useful thing on the card: the frogs pull toward YOUR tempo with
	// exactly the coupling strength Knob Main is set to, so you can dial
	// anywhere from locked-to-the-clock to completely indifferent to it.
	if (c.clockPeriod > 0)
	{
		int32_t clockHz_q8 = (256 * kCtrlRate) / c.clockPeriod;
		if (clockHz_q8 > 32 && clockHz_q8 < 8192) baseHz_q8 = clockHz_q8;
	}

	int32_t baseInc = static_cast<int32_t>(
		(static_cast<int64_t>(baseHz_q8) * 4294967296LL) / (256LL * kCtrlRate));

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

	// The external clock joins the pond as a phantom frog that never listens to
	// anyone. Matching its RATE alone would leave the chorus running at the right
	// tempo but landing anywhere in the bar; coupling to its PHASE is what makes
	// the frogs actually arrive on the beat. Its pull is the same K as everyone
	// else's, so Knob Main dials the whole range from locked-to-clock to
	// completely indifferent.
	bool haveClock = (c.clockPeriod > 0);
	if (haveClock)
	{
		clockPhase_ += static_cast<uint32_t>(baseInc);
		if (c.clock) clockPhase_ = 0;   // a pulse re-anchors the downbeat
	}

	for (int i = 0; i < kNumAgents; i++)
	{
		if (i >= n) { out.state[i] = 0; continue; }

		// Coupling term: pull toward every other active frog's phase.
		// sin(theta_j - theta_i) is positive when j is ahead, so a lagging frog
		// speeds up and a leading one slows down — that is the whole model.
		// Mean of the neighbours, kept in Q15 (+/-32767).
		int32_t coupling = 0;
		int     partners = 0;
		for (int j = 0; j < n; j++)
		{
			if (j == i) continue;
			coupling += fast_sin(phase_[j] - phase_[i]);
			partners++;
		}
		if (haveClock)
		{
			coupling += fast_sin(clockPhase_ - phase_[i]);
			partners++;
		}
		if (partners > 1) coupling /= partners;

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
// ...and staggered leak offsets, added to the Knob Y leak rate (higher shift =
// slower drain, so a bucket that leaks less fills sooner).
static const int8_t kRainLeakBias[kNumAgents] = { 0, -1, 1, 0 };

// When a bucket overflows it SPLASHES into its neighbours. This is what makes
// Rain more than a Poisson process with extra steps: one drip loads the buckets
// either side, so overflows pull each other along into the rushing-and-dragging
// clusters you hear off a real gutter, then fall apart again.
//
// The splash runs DOWNSTREAM only (i -> i+1), like water finding its way down a
// leaf. A symmetric splash locked all four buckets into firing together within
// seconds, which flattened the mode into a single voice; a one-way cascade
// keeps them staggered while still passing energy along.
static constexpr int32_t kRainSplash = kQ16One / 12;

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

	// Downpour: how hard it's raining into the buckets. A modest offset lifts the
	// bottom of the knob just over the leak, then the response stays linear-ish
	// to the top so the last quarter of travel still adds intensity instead of
	// flattening out.
	int32_t x = c.physics;
	int32_t downpour = (x >> 2) + ((x + mul_q16(x, x)) >> 2);
	if (downpour > kQ16One) downpour = kQ16One;

	// Pulse In 2 entrains the rain: each clock pulse tops every bucket up a
	// little, so buckets nearest their threshold tip on the beat while the rest
	// keep running free. The rhythm leans on the clock without being quantised
	// to it — the "dragging" stays.
	if (c.clock)
		for (int i = 0; i < kNumAgents; i++)
			level_[i] += kQ16One / 6;

	// Knob Y sets the LEAK rate rather than noise variance. That is the control
	// that actually shapes this model: a slow leak lets buckets accumulate into
	// heavy irregular drips, a fast leak keeps only the strongest bursts alive.
	// (Chaos 0 = shift 10, slow drain; chaos 1 = shift 7, fast drain.)
	int32_t leakBase = 10 - (c.chaos * 3 / kQ16One);

	int32_t splash[kNumAgents] = { 0, 0, 0, 0 };
	int32_t sum = 0;

	for (int i = 0; i < kNumAgents; i++)
	{
		if (i >= c.population) { out.state[i] = 0; continue; }

		// Noisy inflow. The >>5 sets the ceiling: full downpour lands around 25
		// drips/sec per bucket, a dense torrent that is still a rhythm.
		int32_t drop = mul_q16(rand_q16(rng_), downpour) >> 5;
		if (drop < 0) drop = 0;

		level_[i] += drop;

		int32_t shift = leakBase + kRainLeakBias[i];
		if (shift < 5)  shift = 5;
		if (shift > 12) shift = 12;
		level_[i] = fast_exp_decay(level_[i], static_cast<uint8_t>(shift));

		if (level_[i] >= kRainThreshold[i])
		{
			out.triggers |= (1 << i);
			level_[i] = 0;

			// Splash downstream into the next bucket, collected and applied after
			// the loop so every bucket sees the same instant rather than the
			// low-numbered ones getting a head start. The last bucket spills
			// back to the first, so the cascade wraps around the leaf.
			int next = (i + 1 < c.population) ? (i + 1) : 0;
			if (next != i) splash[next] += kRainSplash;
		}

		// State CV = the bucket filling and emptying: a natural ramp LFO.
		out.state[i] = (level_[i] > kQ16One) ? kQ16One : level_[i];
		sum += out.state[i];
	}

	for (int i = 0; i < c.population; i++) level_[i] += splash[i];

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

	// Pulse In 2 = a smaller swell on each clock pulse, so the debris field
	// breathes with an external tempo instead of wandering entirely free.
	if (c.clock)
	{
		density_ += kQ16One / 4;
		if (density_ > kQ16One) density_ = kQ16One;
	}

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
	// The extra >>1 accounts for the swarm: kSwarmPerAgent members now roll
	// against this probability for every output channel, so without it the
	// perceived rate would triple against the tuning done at 4 agents.
	int32_t p = mul_q16(effective, effective) >> 8;
	if (c.chaos > 0) p += mul_q16(rand_q16(rng_), c.chaos) >> 11;

	// Like Geese, Meteors runs a swarm folded onto the four outputs. Individual
	// meteors are independent, so a channel strikes if any of its members does —
	// which is what gives a dense field its overlapping, uncountable texture.
	for (int a = 0; a < kNumAgents; a++)
	{
		if (a >= c.population) { out.state[a] = 0; continue; }
		for (int k = 0; k < kSwarmPerAgent; k++)
			if (rand_q16(rng_) < p) { out.triggers |= (1 << a); break; }
		out.state[a] = effective;
	}
	out.global = effective;
}

// ===========================================================================
// Mode 6 — Cicadas: density-dependent chorus
// ===========================================================================
//
// The other modes couple on phase (Frogs) or on excitation events (Geese).
// Cicadas couple on AMPLITUDE: each insect calls faster when the field around
// it is already loud, and the field is just the sum of everyone calling. That
// is a positive feedback loop, so on its own it would saturate — fatigue is
// what turns it into a wave. Insects that have been calling hard get tired,
// drop out, the field quietens, they recover, and it swells again.

void CicadasEngine::reset(uint32_t seed)
{
	rng_ = seed | 1u;
	field_ = 0;
	for (int i = 0; i < kSwarmSize; i++)
	{
		phase_[i] = xorshift32(rng_);
		fatigue_[i] = 0;
	}
}

void CicadasEngine::tick(const Ctrl &c, EngineOut &out)
{
	out.triggers = 0;

	int swarm = c.population * kSwarmPerAgent;

	// Pulse In = a footstep in the grass: the whole field falls silent at once,
	// then creeps back in as the insects regain their nerve. The inverse of the
	// other modes' spooks, and the thing every cicada field actually does.
	if (c.spook)
		for (int i = 0; i < swarm; i++) fatigue_[i] = kQ16One;

	// Pulse In 2 nudges the field louder on each beat, so the swells lean
	// toward an external tempo.
	if (c.clock) field_ = (field_ + kQ16One / 4 > kQ16One) ? kQ16One
	                                                       : field_ + kQ16One / 4;

	// Knob Main = COUPLING DEPTH, and it scales both halves of the feedback loop
	// at once: how much the field speeds insects up, and how much being in a
	// loud field tires them out. Scaling both is what makes the knob mean
	// something — with fatigue fixed, the field surged just as hard at zero
	// coupling and the control did nothing.
	//   0.0 = independent insects, a steady even drone
	//   1.0 = the field drives itself into surges that collapse into silence
	int32_t coupling = c.physics;

	// Base call rate, sped up by however loud the field already is. This is the
	// positive half of the loop.
	constexpr int32_t kBaseHz_q8 = 600;   // ~2.3 Hz
	int32_t rateScale = kQ16One + mul_q16(mul_q16(field_, coupling), kQ16One) * 6;
	int32_t hz_q8 = mul_q16(kBaseHz_q8 << 4, rateScale) >> 4;

	int32_t called = 0;
	uint32_t fired = 0;
	for (int i = 0; i < swarm; i++)
	{
		// Fatigue slows an insect right down rather than silencing it outright,
		// so the field thins out gradually instead of switching off.
		int32_t tired = kQ16One - (fatigue_[i] * 3 / 4);
		int32_t myHz = mul_q16(hz_q8, tired);

		// Chaos spreads the natural rates so the swarm never sounds like one
		// insect multiplied.
		if (c.chaos > 0)
		{
			int32_t spread = mul_q16(rand_bipolar(rng_) << 1, c.chaos);
			myHz += (myHz >> 3) * spread >> 15;
		}
		if (myHz < 16) myHz = 16;

		uint32_t inc = static_cast<uint32_t>(
			(static_cast<int64_t>(myHz) * 4294967296LL) / (256LL * kCtrlRate));

		uint32_t before = phase_[i];
		phase_[i] += inc;

		if (phase_[i] < before)          // wrapped: this insect called
		{
			called++;
			fired |= (1u << i);
			fatigue_[i] += mul_q16(kQ16One / 3, coupling);   // calling is work
			if (fatigue_[i] > kQ16One) fatigue_[i] = kQ16One;
		}

		// The negative half of the loop: simply BEING in a loud field is
		// tiring, whether or not this insect called. Without this the swarm
		// spread itself evenly and never surged — collective exhaustion is what
		// turns feedback into a wave rather than a runaway.
		fatigue_[i] += mul_q16(mul_q16(field_, coupling), kQ16One / 150);
		if (fatigue_[i] > kQ16One) fatigue_[i] = kQ16One;

		// Recovery is slow — slower than the build-up, which is what sets the
		// period of the swell.
		fatigue_[i] = fast_exp_decay(fatigue_[i], 12);
	}

	// The field is the perceived loudness of the whole chorus: it rises quickly
	// as insects join and falls away slowly, so the swarm hears its own recent
	// past rather than only the current instant.
	int32_t instant = (swarm > 0)
		? (called * kQ16One * 8 / swarm) : 0;
	if (instant > kQ16One) instant = kQ16One;
	field_ = (instant > field_) ? slew(field_, instant, 4)
	                            : slew(field_, instant, 8);

	// Fold the swarm onto the outputs: a channel fires if any of its insects
	// called, and carries its group's mean fatigue as state CV.
	for (int a = 0; a < kNumAgents; a++)
	{
		if (a >= c.population) { out.state[a] = 0; continue; }
		int32_t groupFatigue = 0;
		for (int k = 0; k < kSwarmPerAgent; k++)
		{
			int i = a * kSwarmPerAgent + k;
			if (fired & (1u << i)) out.triggers |= (1 << a);
			groupFatigue += fatigue_[i];
		}
		out.state[a] = groupFatigue / kSwarmPerAgent;
	}

	out.global = field_;
}

} // namespace bio
