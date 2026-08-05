// voices.cpp — voice rendering at 48kHz.
//
// Each mode gets a timbre that suggests its creature without pretending to be a
// recording, plus a round-robin variant system and a stereo strategy that comes
// from the ecosystem rather than from a knob.

#include "voices.h"
#include "pico.h"   // __not_in_flash_func
#include "samplestore.h"       // user flash region, with baked fallback

namespace bio {

// Per-mode envelope decay shifts (larger = longer tail at 48kHz).
static const uint8_t kDecay[kNumModes] = {
	9,   // Horses  — tight clop
	11,  // Geese   — honk with some length
	11,  // Frogs   — ribbit body
	8,   // Rain    — very short drip
	13,  // Meteors — long swoosh
	10   // Cicadas — short buzzing chirp
};

// Per-mode base pitch as a 48kHz phase increment (~ Hz * 89478).
static const uint32_t kBaseInc[kNumModes] = {
	 16000000u,  // Horses  ~180Hz thump
	 39000000u,  // Geese   ~440Hz honk
	 27000000u,  // Frogs   ~300Hz ribbit
	124000000u,  // Rain    ~1.4kHz drip
	  9000000u,  // Meteors ~100Hz rumble under the noise
	340000000u   // Cicadas ~3.8kHz stridulation
};

// Round-robin pitch offsets, Q16, used by the SYNTH voices — real recordings
// already differ from each other and are played at pitch. In Horses the first
// four are the hooves (hinds heavier and lower than fores); elsewhere they are
// simply individuals.
static const int32_t kVariantPitch[kNumModes][kNumVariants] = {
	// Horses: LH, LF, RH, RF, then repeats — only the first four are ever used.
	{  56000,  74000,  60000,  79000,  56000,  74000,  60000,  79000 },
	// Geese: birds of different size.
	{  65536,  52000,  78000,  44000,  71000,  48000,  85000,  58000 },
	// Frogs: species in the chorus.
	{  65536,  49000,  88000,  61000,  55000,  75000,  42000,  95000 },
	// Rain: drip sizes.
	{  65536,  81000,  53000,  92000,  70000,  59000, 104000,  47000 },
	// Meteors: distances.
	{  65536,  48000,  86000,  57000,  72000,  41000,  95000,  63000 },
	// Cicadas: a tight spread — a real field is fairly uniform.
	{  65536,  69000,  62000,  72000,  67000,  59000,  75000,  64000 }
};

// --- PCM playback-rate variation -------------------------------------------
//
// Per-agent body size: a signed position in the spread, -1.0..+1.0 in Q15.
// Fixed per agent so an animal keeps its identity — agent 0 is always the
// biggest and deepest, agent 3 the smallest.
static const int32_t kPcmAgentOffset[kNumAgents] = { -32768, 16384, -12000, 32767 };

// Full pitch spread per mode, Q16, applied as +/- this much of the playback
// rate. 3800 is about 1 semitone. A herd varies audibly; a field of cicadas
// barely at all, because they are the same insect many times over.
static const int32_t kPcmAgentSpread[kNumModes] = {
	2600,   // Horses  — ~0.7 semitones, distinct animals but still all horses
	4400,   // Geese   — birds differ noticeably in size
	6600,   // Frogs   — different species in one pond
	5100,   // Rain    — drip size varies with where it fell
	8800,   // Meteors — distance, so the widest spread of all
	1500    // Cicadas — near-uniform
};

// Extra per-EVENT jitter, Q16 peak deviation. Zero for Horses: a horse is one
// animal and its clop must not change pitch hit to hit. For the crowd modes
// this is what stops two overlapping calls fusing into one doubled sound.
static const int32_t kPcmPerHit[kNumModes] = {
	0,      // Horses  — MUST stay fixed; see above
	1800,   // Geese
	2400,   // Frogs
	2000,   // Rain
	3200,   // Meteors
	1200    // Cicadas
};

// Stereo strategy per mode. The pan of a hit says something about the ecosystem:
//   FIXED  — the agent always sits in the same place (a stable image; you are
//            standing beside the animal and its legs do not move around you).
//   SPREAD — each swarm member has its own fixed spot, so a cascade sweeps
//            across the field.
//   RANDOM — every hit lands somewhere new, because every hit is a new object.
enum class PanMode : uint8_t { Fixed, Spread, Random };
static const PanMode kPanMode[kNumModes] = {
	PanMode::Fixed,   // Horses  — a stable animal in front of you
	PanMode::Spread,  // Geese   — birds placed around you
	PanMode::Spread,  // Frogs   — a pond, voices at fixed spots
	PanMode::Random,  // Rain    — drips land wherever they land
	PanMode::Random,  // Meteors — each strike is a new object crossing the sky
	PanMode::Spread   // Cicadas — a dense field all around
};

/// Place a voice in the stereo field. `pos17` is 0 (hard left) to 16 (hard
/// right), 8 = centre. Both gains keep a floor so a hard-panned hit still has
/// presence in the other ear rather than vanishing — with only two outputs,
/// fully-dead channels make a patch feel broken on a mono system.
static inline void setPan(Voice &v, int pos17)
{
	if (pos17 < 0) pos17 = 0;
	if (pos17 > 16) pos17 = 16;
	int32_t r = pos17 * 2048;              // 0..32768
	int32_t l = 32768 - r;
	constexpr int32_t kFloor = 6000;
	v.panL = kFloor + ((l * (32767 - kFloor)) >> 15);
	v.panR = kFloor + ((r * (32767 - kFloor)) >> 15);
}

void VoiceBank::init(bool usePcm, bool tuned)
{
	tuned_ = tuned;
	// AnySamples() covers both sources: a user upload alone is enough, even in
	// a build with nothing baked in.
	usePcm_ = usePcm && AnySamples();

	// Resolve the variant tables once, here, instead of walking the user header
	// in flash on every note-on. Uploading samples reboots the card, so this can
	// never go stale while the card is playing.
	for (int m = 0; m < kNumModes; m++)
	{
		// Resolve every slot's pointer and length FIRST, so note() never touches
		// the flash header again — and so the duplicate test below can compare
		// pointers rather than re-reading flash.
		for (int v = 0; v < kNumVariants; v++)
		{
			SampleRef sr = ResolveSample(m, v);
			sampleData_[m][v] = sr.data;
			sampleLen_[m][v]  = sr.len;
		}

		int n = 0;
		if (ModeIsUserLoaded(m))
		{
			const UserSampleHeader *h = UserHeader();
			for (int v = 0; v < kNumVariants; v++)
				if (h->size[m][v] > 0) variantSlot_[m][n++] = static_cast<uint8_t>(v);
		}
		else if (kHaveSamples)
		{
			// Only count slots holding DISTINCT audio. mksamples.py pads a mode
			// with fewer recordings than slots by repeating earlier ones, so
			// Meteors' five swooshes fill eight slots as 1,2,3,4,5,1,2,3 — and a
			// blind round robin over all eight then played 1-3 twice as often as
			// 4-5. An unintended weighting, invisible because the padding is
			// deliberate and commented. Duplicates share a flash pointer, so
			// identity is the test.
			for (int v = 0; v < kNumVariants; v++)
			{
				bool dup = false;
				for (int k = 0; k < n; k++)
					if (sampleData_[m][variantSlot_[m][k]] == sampleData_[m][v])
						{ dup = true; break; }
				if (!dup) variantSlot_[m][n++] = static_cast<uint8_t>(v);
			}
		}
		variantCount_[m] = static_cast<uint8_t>(n);
	}

	// The voice POOL is larger than the agent count, so it gets its own loop.
	for (int i = 0; i < kNumVoices; i++)
	{
		Voice &v = v_[i];
		v.env = 0; v.phase = 0; v.phase2 = 0; v.inc = 0; v.inc2 = 0;
		v.pitchEnv = 0;
		v.noiseRng = 0x1234567u + static_cast<uint32_t>(i) * 2654435761u;
		v.filt = 0; v.filt2 = 0; v.bp = 0;
		v.decayShift = 10; v.mode = 0; v.variant = 0;
		v.ksLen = 64; v.ksPos = 0;
		for (int k = 0; k < 128; k++) v.ks[k] = 0;
		v.pcm = nullptr; v.pcmLen = 0; v.pcmIdx = 0; v.pcmFrac = 0; v.pcmInc = 65536;
		v.agent = -1;              // free
		v.release = kQ16One;
		v.fadeOut = 0;
		setPan(v, 2 + (i % kNumAgents) * 4);
	}

	for (int i = 0; i < kNumAgents; i++)
	{
		lastVariant_[i] = 0xFF;

		DroneVoice &d = d_[i];
		d.level = 0; d.density = 0; d.spread = 0;
		d.countdown = 1 + i * 7;      // stagger so grains do not all start together
		d.next = 0;
		d.rng = 0x2468ACEu + static_cast<uint32_t>(i) * 40503u;
		for (int k = 0; k < kNumGrains; k++)
		{
			d.g[k].pcm = nullptr; d.g[k].len = 0; d.g[k].pos = 0;
			d.g[k].inc = 65536; d.g[k].level = 0; d.g[k].active = false;
		}
	}
}

void __not_in_flash_func(VoiceBank::selectVariantAndPan)(Voice &v, int agent, Mode m, uint8_t member)
{
	int mi = static_cast<int>(m);

	if (m == Mode::Horses)
	{
		// The variant IS the hoof that landed, reported by the engine. A gait
		// only sounds like an animal when each leg has its own voice — and with
		// a herd, each horse plays all four of its own hooves.
		v.variant = static_cast<uint8_t>(member % kHoofVariants);
	}
	else
	{
		// Round robin with no immediate repeat, so you never hear the same
		// honk or drip twice running.
		//
		// Bounded by what this mode actually HAS, not by the eight slots it could
		// have. Uploading two geese used to leave the other six baked recordings
		// in the rotation, so you heard your two mixed with six of the card's —
		// never what replacing a mode is meant to do.
		// No divide, and no retry loop. `% n` compiled to an __aeabi_uidivmod
		// call — a software divide on a core with no divider — INSIDE a loop
		// that can spin, and this runs per note-on. Together with the two
		// flash-walking helpers it made a single note() cost ~2148 cycles,
		// 82% of the whole sample budget.
		//
		// Advance-and-wrap instead: it gives the same "never twice running"
		// guarantee by construction rather than by rejection, and costs an add
		// and a compare.
		int n = variantCount_[mi];
		if (n < 1) n = 1;
		uint8_t pick;
		if (n == 1)
		{
			pick = 0;
		}
		else
		{
			// Step 1..n-1 places on from the last one, so it always differs.
			uint32_t adv = 1u + (xorshift32(rng_) & 7u);
			while (adv >= static_cast<uint32_t>(n)) adv -= static_cast<uint32_t>(n);
			if (adv == 0) adv = 1;
			int next = lastVariant_[agent] + static_cast<int>(adv);
			while (next >= n) next -= n;
			pick = static_cast<uint8_t>(next);
		}
		lastVariant_[agent] = pick;
		// Map onto a slot that actually holds audio, so uploading to slots 1 and
		// 5 works as well as 1 and 2 — the browser does not have to police order.
		v.variant = variantSlot_[mi][pick];
	}

	switch (kPanMode[mi])
	{
	case PanMode::Fixed:
		// Each horse holds its own place in the field, and its four hooves sit
		// just around that spot — near/off side. The animal stays put; you hear
		// its legs, not four wandering sounds.
		setPan(v, 2 + agent * 4 + ((v.variant & 1) ? 1 : -1));
		break;
	case PanMode::Spread:
		// Agent's spot, nudged by which individual this is, so the members of a
		// flock occupy their own places rather than stacking up.
		//
		// `& 3` used to mask this to four positions, so variants 4-7 landed on
		// exactly the same spots as 0-3 — eight round robins produced four
		// placements. That is most of why Cicadas, whose variants differ little
		// in timbre by design, sounded like four insects rather than eight.
		// Halved and offset so eight variants still fit the agent's own region
		// of the field rather than sprawling across it.
		setPan(v, 1 + agent * 4 + (v.variant >> 1));
		break;
	case PanMode::Random:
	{
		// Anywhere — every drip and every meteor is a new object.
		//
		// `% 17` is a software divide on this core, per note-on. Masking to 0..31
		// and folding the top half back gives 0..16 with only a compare and a
		// subtract; the slight bias toward the middle positions is inaudible in a
		// pan position that is meant to be arbitrary anyway.
		uint32_t p = xorshift32(rng_) & 31u;
		if (p > 16u) p -= 17u;
		setPan(v, static_cast<int>(p));
		break;
	}
	}
}

void __not_in_flash_func(VoiceBank::note)(int i, Mode m, int32_t accent, int32_t variation,
                     uint8_t member)
{
	// Pick a voice from the pool rather than using the agent index directly.
	//
	// Order of preference: a free voice, then the quietest sounding one. Taking
	// the quietest means a steal lands on whatever is closest to finished, which
	// is the least audible thing to interrupt — and the release ramp below hides
	// what is left of it.
	int slot = -1;
	uint32_t leastLeft = 0xFFFFFFFFu;
	int oldestSlot = 0;
	for (int k = 0; k < kNumVoices; k++)
	{
		if (v_[k].agent < 0) { slot = k; break; }
		// A voice already fading out is the best thing to take: it is on its way
		// to silence anyway, so cutting it short costs nothing audible. Without
		// this the fades would occupy slots and shrink the effective pool.
		if (v_[k].fadeOut > 0) { leastLeft = 0; oldestSlot = k; continue; }
		// Which sounding voice has the LEAST LEFT to play?
		//
		// This used to compare how far each voice had got, which is wrong when
		// samples differ in length: a 770ms frog 300ms in looked "more played"
		// than a 179ms goose 150ms in, even though the goose is nearly finished
		// and the frog is a third of the way through. Frogs are the longest
		// recordings on the card (770ms mean against Geese's 179ms), so they
		// were the ones being cut — which is exactly what was reported.
		//
		// Remaining, not progress. Still a subtraction, no divide.
		uint32_t left = v_[k].pcm
			? (v_[k].pcmLen > v_[k].pcmIdx ? v_[k].pcmLen - v_[k].pcmIdx : 0)
			: static_cast<uint32_t>(v_[k].env);
		if (left <= leastLeft) { leastLeft = left; oldestSlot = k; }
	}
	if (slot < 0)
	{
		// Nothing free. Hand the chosen victim a 4ms fade-out and let it keep
		// sounding into the new note, rather than cutting it dead — the outgoing
		// discontinuity is the click, and a fade-in on the new voice cannot hide
		// it. It frees itself once the fade completes.
		//
		// The new note takes the same slot only if the victim is ALREADY fading
		// (nothing left to lose); otherwise it takes the next slot along, so the
		// two genuinely overlap for those 4ms.
		if (v_[oldestSlot].fadeOut > 0)
		{
			slot = oldestSlot;
		}
		else
		{
			v_[oldestSlot].fadeOut = kQ16One;
			slot = (oldestSlot + 1) % kNumVoices;
			// If that one is live too, it is the least-bad remaining choice; take
			// it outright rather than cascading fades through the whole pool.
		}
	}

	Voice &v = v_[slot];
	v.agent = static_cast<int8_t>(i);
	int mi = static_cast<int>(m);

	v.mode = static_cast<uint8_t>(mi);
	v.env = (accent > kQ16One) ? kQ16One : accent;
	v.decayShift = kDecay[mi];
	v.pitchEnv = kQ16One;
	v.phase = 0;
	v.phase2 = 0;
	v.filt = 0;
	v.filt2 = 0;
	v.bp = 0;

	selectVariantAndPan(v, i, m, member);

	// Pitch: mode base, shifted by the round-robin variant, then nudged by the
	// engine's state so repeated hits are never quite identical.
	int32_t pitchScale = kVariantPitch[mi][v.variant];
	pitchScale = mul_q16(pitchScale, kQ16One + (variation >> 3) - 4096);
	if (pitchScale < 8192) pitchScale = 8192;

	v.inc = static_cast<uint32_t>(
		(static_cast<int64_t>(kBaseInc[mi]) * pitchScale) >> 16);
	// Second oscillator, slightly detuned, for the modes that use it.
	v.inc2 = v.inc + (v.inc >> 6);

	if (usePcm_)
	{
		// Play this variant's slice of the mode's blob — in Horses, the sample
		// for the hoof that actually landed.
		// Cached at init — see sampleData_. This used to call ResolveSample(),
		// which reads the user header through XIP on every single trigger.
		v.pcm    = sampleData_[mi][v.variant];
		v.pcmLen = sampleLen_[mi][v.variant];
		v.pcmIdx = 0;
		v.pcmFrac = 0;
		v.fadeOut = 0;              // this voice is starting, not ending

		// Start the fade-in from silence. Recordings are trimmed to their attack
		// so they begin at a non-zero amplitude, and starting one abruptly in a
		// voice that was just cut off is a step discontinuity — a click. Two
		// milliseconds is short enough to leave a hoof or a drip sounding sharp,
		// and long enough to remove the edge.
		v.release = 0;

		// Playback rate. Everything used to play at exactly 1:1, so four horses
		// were byte-identical and a flock of geese was one goose repeated.
		//
		// The distinction that matters: an ANIMAL has a fixed size, so its rate
		// must be constant every time it sounds — a horse whose clop changes
		// pitch hit to hit is not one horse. A CROWD is many individuals, so
		// there the variation is per-event.
		int32_t rate = kQ16One;

		// TUNED (alt boot): play the recording at its own pitch, full stop.
		//
		// Everything below assumes the sample is a creature — a per-agent body
		// size, a per-hit wobble, a per-variant individual. All three are right
		// for a flock and wrong for anything pitched, where they read as four
		// permanently out-of-tune copies that also wobble. A static tone or a
		// chord stab wants none of it, and that is exactly the material someone
		// uploads. Rhythm keeps the humanisation; alt mode is an instrument.
		if (!tuned_)
		{
			// Per-agent body size: a fixed offset, so agent 2 is always the same
			// animal. kPcmAgentOffset is +/-1.0 in Q15, so >>15 scales the mode's
			// spread onto it.
			rate += (kPcmAgentOffset[i] * kPcmAgentSpread[mi]) >> 15;

			// Per-VARIANT pitch, which the PCM path never had. kVariantPitch was
			// computed above into v.inc and then only ever read by the synth
			// branch, so with samples baked in the round robin differed in timbre
			// but never in pitch — eight geese at exactly one pitch.
			//
			// NOT for Horses: its four variants are the four hooves of ONE animal,
			// and the rule the mode is built on is that a horse's pitch cannot
			// change hit to hit. Applying it there would undo kPcmPerHit being 0.
			//
			// Scaled by the mode's own spread rather than used raw, so a mode meant
			// to be uniform stays uniform: Cicadas moves a few cents, Frogs a few
			// tenths of a semitone. These are real recordings that already differ,
			// so this only has to break the sense of one sound repeating.
			if (mi != static_cast<int>(Mode::Horses))
			{
				int32_t dev = kVariantPitch[mi][v.variant] - kQ16One;
				rate += (dev * kPcmAgentSpread[mi]) >> 17;
			}

			if (kPcmPerHit[mi])
			{
				// Crowd modes: an extra small random nudge per event, so two
				// overlapping honks never fuse into one doubled sound.
				int32_t j = rand_bipolar(rng_);              // +/-16384
				rate += (j * kPcmPerHit[mi]) >> 14;
			}
		}

		if (rate < kQ16One / 4)  rate = kQ16One / 4;        // never below 0.25x
		if (rate > kQ16One * 3)  rate = kQ16One * 3;        // never above 3x
		v.pcmInc = static_cast<uint32_t>(rate);
		return;
	}

	// Frogs use Karplus-Strong: excite the delay line with noise, then let the
	// averaging low-pass in render() turn it into a pitched pluck.
	if (m == Mode::Frogs)
	{
		uint32_t len = 48000u * 65536u / (v.inc / 1024u + 1u) / 1024u;
		if (len < 16) len = 16;
		if (len > 127) len = 127;
		v.ksLen = static_cast<uint8_t>(len);
		v.ksPos = 0;
		for (uint8_t k = 0; k < v.ksLen; k++)
			v.ks[k] = static_cast<int16_t>(rand_bipolar(v.noiseRng) >> 3);
	}
}

// ---------------------------------------------------------------------------
// Drone boot
// ---------------------------------------------------------------------------
//
// Same six physics engines, completely different instrument. Instead of firing
// one-shot voices, each agent's continuous state bends a sustained tone:
// a horse's stride becomes a slow pitch sweep, a bucket's fill level a rising
// drone, the frogs' phase coherence a chorus that thickens as they lock. The
// triggers survive only as small accents, so events colour the texture rather
// than punctuating it.

// Fixed drone positions: wide and stable, so the four voices make an image you
// can sit inside rather than something that moves around you.
static const int32_t kPanCurveL[kNumAgents] = { 31000, 23000, 15000,  8000 };
static const int32_t kPanCurveR[kNumAgents] = {  8000, 15000, 23000, 31000 };

// How often each mode launches new grains, as control ticks between them at
// minimum density. Denser modes overlap more heavily.
// Chosen against the actual sample lengths so that grain duration divided by
// launch interval lands near 2-3x — enough overlap for a seamless texture, but
// within the kNumGrains slots. Ask for more overlap than there are slots and
// grains get stolen mid-playback, which chops audibly.
static const int32_t kGrainPeriod[kNumModes] = {
	 130,   // Horses  — a herd on a road, hooves still individually audible
	 150,   // Geese   — a constant agitated flock
	 620,   // Frogs   — long calls, so they need room to breathe
	  45,   // Rain    — steady downpour, near-continuous
	2200,   // Meteors — very long swooshes; sparse launches, huge grains
	  60    // Cicadas — a solid field of insects
};

// Playback-rate spread across grains, in 1/256ths. Wide spreads smear a
// recording into texture; narrow ones keep it recognisable.
static const int32_t kGrainSpread[kNumModes] = {
	 72,   // Horses
	 96,   // Geese
	120,   // Frogs  — different species
	110,   // Rain
	140,   // Meteors — the most smeared, most abstract
	 40    // Cicadas — near-uniform, it is one insect many times
};

// Base playback rate per mode, Q16. Below 65536 stretches the recording, which
// is what turns a one-shot into a sustained layer.
static const int32_t kGrainRate[kNumModes] = {
	52000,   // Horses  — 0.79x, heavier animals
	49000,   // Geese   — 0.75x, throatier
	42000,   // Frogs   — 0.64x, deep pond
	58000,   // Rain    — 0.88x, still watery
	26000,   // Meteors — 0.40x, long and cavernous
	61000    // Cicadas — 0.93x, close to natural
};

void __not_in_flash_func(VoiceBank::droneUpdate)(Mode m, const int32_t *state, int32_t global,
                            uint8_t triggers, int active, int32_t timbre)
{
	int mi = static_cast<int>(m);

	// With no samples baked in there is nothing to granulate, so Drone has
	// nothing to do. (Rhythm still works; see kHaveSamples.)
	if (!usePcm_) return;

	for (int i = 0; i < kNumAgents; i++)
	{
		DroneVoice &d = d_[i];

		if (i >= active)
		{
			// Fade out rather than cut, so changing population is a swell.
			d.level = slew(d.level, 0, 6);
			continue;
		}
		d.level = slew(d.level, kQ16One, 6);

		// Grain DENSITY comes from the ecosystem's global value plus this
		// agent's own state, so each voice thickens and thins on its own while
		// the whole texture still breathes together.
		//
		// Nothing is mapped to pitch. Most engines put a PHASE RAMP in state[],
		// and mapping a ramp to pitch is exactly what made the old oscillator
		// version sweep upward and snap back forever.
		int32_t want = (global + state[i]) >> 1;
		d.density = slew(d.density, want, 5);

		// Knob Y widens the pitch spread across grains, from a tight recognisable
		// layer out to a smeared cloud.
		d.spread = timbre;

		bool launch = (triggers & (1 << i)) != 0;

		// Otherwise launch on a countdown that shortens as density rises.
		if (--d.countdown <= 0)
		{
			launch = true;
			// Density shortens the interval by up to 40%. It was 75%, which
			// asked for far more overlap than there are grain slots — the
			// round-robin then stole grains mid-playback and chopped audibly.
			// 0.4 as a Q16 multiply, not `* 2 / 5`: the M0+ has no divider and
			// the literal compiled to an __aeabi_idiv call.
			int32_t period = kGrainPeriod[mi];
			period -= mul_q16(mul_q16(period, kQ16One * 2 / 5), d.density);
			if (period < 8) period = 8;
			// A little randomness so grains never lock into a machine pulse.
			// Masked, not `% 32` -- same reason.
			d.countdown = period + (static_cast<int32_t>(xorshift32(d.rng) & 31u));
		}

		if (launch)
		{
			Grain &g = d.g[d.next];
			// kNumGrains is a power of two, so mask rather than divide.
			static_assert((kNumGrains & (kNumGrains - 1)) == 0,
			              "kNumGrains must be a power of two for this mask");
			d.next = static_cast<uint8_t>((d.next + 1) & (kNumGrains - 1));

			// Pick any variant — in Drone the recordings are raw material, so
			// the hoof/individual distinction does not apply.
			// Same bound as the Rhythm round robin: granulate only what this mode
			// actually has, so two uploaded geese do not get mixed with six baked.
			// Cached table, and a mask rather than a modulo: same reasoning as
			// the Rhythm round robin above. A grain launch is control-rate, but
			// there is no reason to pay a software divide for it either.
			int nvar = variantCount_[mi];
			if (nvar < 1) nvar = 1;
			uint32_t r = xorshift32(d.rng) & 7u;
			while (r >= static_cast<uint32_t>(nvar)) r -= static_cast<uint32_t>(nvar);
			uint8_t var = variantSlot_[mi][r];
			g.pcm = sampleData_[mi][var];   // cached, see note()
			g.len = sampleLen_[mi][var];

			// Window shape is fixed for the grain's whole life, so pay for the
			// divide once here instead of 48000 times a second in the renderer.
			//
			// Scaled so dist*invHalf lands in Q16 after a fixed >>16, with the
			// reciprocal itself kept just under 2^32. Half-lengths on this card
			// run from 8 to ~500000 (meteors_5 is 118596 bytes and an uploaded
			// recording can fill the 1MB user region), which is too wide a range
			// for one fixed scale: a Q16 reciprocal truncates to 1 or 0 on the
			// long samples -- a ~10% window skew, or a silent grain -- while a
			// Q48 one overflows 32 bits on the short ones.
			// halfLen < 2 would make the reciprocal itself overflow 32 bits; such
			// a grain is a handful of samples and inaudible, so it windows to
			// silence rather than being special-cased through the render loop.
			uint32_t halfLen = g.len >> 1;
			g.invHalf = (halfLen >= 2)
				? static_cast<uint32_t>((static_cast<uint64_t>(1) << 32) / halfLen)
				: 0;

			// Start somewhere inside the sample, not always at the attack —
			// repeated attacks would read as a rhythm rather than a texture.
			uint32_t startMax = (g.len > 16) ? (g.len >> 1) : 0;
			g.pos = startMax ? ((xorshift32(d.rng) % startMax) << 16) : 0;

			int32_t rate = kGrainRate[mi];
			int32_t j = rand_bipolar(d.rng);                  // +/-16384
			rate += (j * kGrainSpread[mi]) >> 12;
			rate += (j * mul_q16(kGrainSpread[mi] << 2, d.spread)) >> 13;
			if (rate < 4096) rate = 4096;                     // floor at 0.06x
			g.inc = static_cast<uint32_t>(rate);

			g.level = kQ16One;
			g.active = true;
		}
	}
}

void __not_in_flash_func(VoiceBank::droneRender)(int active, int16_t &l, int16_t &r)
{
	int32_t accL = 0, accR = 0;

	for (int i = 0; i < kNumAgents; i++)
	{
		DroneVoice &d = d_[i];
		if (i >= active && d.level <= 0) continue;

		int32_t mix = 0;
		for (int k = 0; k < kNumGrains; k++)
		{
			Grain &g = d.g[k];
			if (!g.active) continue;

			uint32_t idx = g.pos >> 16;
			if (!g.pcm || idx >= g.len) { g.active = false; continue; }

			int32_t mu = static_cast<int32_t>(g.pos & 0xFFFF);
			int32_t a  = g.pcm[idx];
			int32_t b  = (idx + 1 < g.len) ? g.pcm[idx + 1] : 0;
			int32_t s  = (a + (((b - a) * mu) >> 16)) << 4;   // 8-bit -> 12-bit

			// Window the grain: fade in and out across its length so overlapping
			// copies cross-fade instead of clicking at their edges. Triangular,
			// which is cheap and good enough at this density.
			uint32_t half = g.len >> 1;
			uint32_t dist = (idx < half) ? idx : (g.len - 1 - idx);
			// A plain 32-bit multiply, no 64-bit helper call; >>16 lands it in
			// Q16. dist is capped below half because an odd len lets dist reach
			// half exactly, and half * (2^32/half) is exactly 2^32 -- which
			// wraps to zero and turns the window's peak into silence.
			if (dist >= half) dist = half ? half - 1 : 0;
			int32_t win = static_cast<int32_t>((dist * g.invHalf) >> 16);
			if (win > kQ16One) win = kQ16One;

			// Plain 32-bit multiplies, NOT mul_q16: that widens to int64_t, which
			// on the M0+ is an __aeabi_lmul library call -- and this line runs up
			// to 32 times per sample at 48kHz. |s| <= 2048 and win <= 65536, so
			// the product peaks at 1.3e8 and the accumulator at 5.4e8, both well
			// inside int32. Removing the divide above took droneRender from 17546
			// cycles to 9608; these two calls were the other half.
			mix += ((s * win) >> 16) >> 1;
			g.pos += g.inc;
		}

		mix = (mix * d.level) >> 16;

		// Fixed wide positions: the point is a stable image you can sit inside.
		accL += mul_q15(mix, kPanCurveL[i]);
		accR += mul_q15(mix, kPanCurveR[i]);
	}

	l = clamp12(accL);
	r = clamp12(accR);
}

void __not_in_flash_func(VoiceBank::render)(int active, int16_t &l, int16_t &r)
{
	int32_t accL = 0, accR = 0;

	for (int i = 0; i < kNumVoices; i++)
	{
		Voice &v = v_[i];
		int32_t s = 0;

		// Free voices cost one compare, which is what makes a pool affordable.
		if (v.agent < 0) continue;

		if (usePcm_)
		{
			// Compare in SAMPLE space, not in Q16 position space: pcmLen << 16
			// overflows 32 bits for anything longer than 65536 bytes (1.37s),
			// which silently truncated the two longest meteor swooshes to under
			// half their length.
			uint32_t idx = v.pcmIdx;
			if (v.pcm && idx < v.pcmLen)
			{
				// 8-bit PCM, linear-interpolated so pitch-shifted playback
				// doesn't alias badly.
				int32_t  mu  = static_cast<int32_t>(v.pcmFrac);
				int32_t  a   = v.pcm[idx];
				int32_t  b   = (idx + 1 < v.pcmLen) ? v.pcm[idx + 1] : 0;
				s = (a + (((b - a) * mu) >> 16)) << 4;   // 8-bit -> 12-bit

				// Advance: carry the Q16 fraction into whole samples, so the
				// index has the full 32-bit range instead of the 16 bits a
				// combined Q16 position would leave it.
				v.pcmFrac += v.pcmInc;
				v.pcmIdx  += v.pcmFrac >> 16;
				v.pcmFrac &= 0xFFFF;

				// Fade in over ~2ms. The PCM path had NO envelope at all: v.env
				// was set by note() and never read here, so a sample played flat
				// and a retrigger jumped straight to the new recording's first
				// byte. That step is what was heard as truncation.
				if (v.release < kQ16One)
				{
					// 4ms, up from 2. Matches the fade-OUT below so a steal is a
					// symmetrical crossfade rather than a dip or a bump.
					v.release += kQ16One / 192;    // 192 samples = 4ms at 48kHz
					if (v.release > kQ16One) v.release = kQ16One;
					// Plain 32-bit multiply, NOT mul_q16: that widens to int64
					// and becomes an __aeabi_lmul call, which at one per voice
					// per sample across eight voices cost ~700 cycles. |s| <=
					// 2048 and release <= 65536, so the product peaks at 1.3e8
					// with 16x headroom inside int32.
					s = (s * v.release) >> 16;
				}

				// Stolen: ramp to silence over 4ms and then free the slot. Long
				// enough to remove the step without smearing the new note that
				// is already sounding alongside it.
				if (v.fadeOut > 0)
				{
					v.fadeOut -= kQ16One / 192;    // 192 samples = 4ms at 48kHz
					if (v.fadeOut <= 0)
					{
						v.fadeOut = 0;
						v.agent = -1;
						v.pcm = nullptr;
						s = 0;
					}
					else s = (s * v.fadeOut) >> 16;
				}
			}
			else
			{
				// Finished: hand the voice back to the pool.
				v.agent = -1;
				v.pcm = nullptr;
			}
		}
		else if (v.env > 0)
		{
			switch (static_cast<Mode>(v.mode))
			{
			case Mode::Horses:
			{
				// Hoof on a hard road: a pitch-dropping sine body for the mass
				// of the animal, plus a sharp noise transient for the shoe
				// striking stone. The transient is most of what makes it read
				// as a hoof rather than a kick drum.
				uint32_t inc = static_cast<uint32_t>(
					(static_cast<int64_t>(v.inc) * (kQ16One + v.pitchEnv * 3)) >> 16);
				v.phase += inc;
				int32_t body = fast_sin(v.phase) >> 4;

				int32_t click = 0;
				if (v.pitchEnv > 40000)
				{
					// Band-passed noise for the stony "tk" of the impact.
					int32_t n = rand_bipolar(v.noiseRng);
					v.bp += (n - v.bp) >> 1;
					v.filt += (v.bp - v.filt) >> 3;
					click = (v.bp - v.filt) >> 3;
				}
				s = body + click;
				v.pitchEnv = fast_exp_decay(v.pitchEnv, 6);
				break;
			}
			case Mode::Geese:
			{
				// Honk: a buzzy saw through two poles, with the cutoff opening
				// at the attack so it starts hard and softens — the nasal
				// "kink" of the call.
				v.phase += v.inc;
				int32_t saw = static_cast<int32_t>(v.phase >> 20) - 2048;
				int32_t k = 1 + (v.pitchEnv >> 15);
				v.filt  += (saw - v.filt) >> k;
				v.filt2 += (v.filt - v.filt2) >> 1;
				s = v.filt2;
				v.pitchEnv = fast_exp_decay(v.pitchEnv, 8);
				break;
			}
			case Mode::Frogs:
			{
				// Karplus-Strong: average adjacent taps (the low-pass) and feed
				// it back around the delay line.
				// Increment and wrap, not `% v.ksLen`: ksLen is not a power of
				// two, so the modulo compiled to an __aeabi_idivmod call — in
				// the 48kHz render loop, for every sounding Frogs voice.
				uint8_t nxt = static_cast<uint8_t>(v.ksPos + 1);
				if (nxt >= v.ksLen) nxt = 0;
				int32_t avg = (v.ks[v.ksPos] + v.ks[nxt]) >> 1;
				avg -= avg >> 6;                  // damping
				v.ks[v.ksPos] = static_cast<int16_t>(avg);
				v.ksPos = nxt;
				s = avg;
				break;
			}
			case Mode::Rain:
			{
				// Water drip: a sine whose pitch RISES as it decays, which is
				// the acoustic signature of a bubble collapsing in liquid and
				// the reason a drip sounds like a drip.
				uint32_t inc = static_cast<uint32_t>(
					(static_cast<int64_t>(v.inc) *
					 (kQ16One + ((kQ16One - v.pitchEnv) >> 1))) >> 16);
				v.phase += inc;
				s = fast_sin(v.phase) >> 4;
				if (v.pitchEnv > 50000)
					s += rand_bipolar(v.noiseRng) >> 5;
				v.pitchEnv = fast_exp_decay(v.pitchEnv, 5);
				break;
			}
			case Mode::Cicadas:
			{
				// Stridulation: a high tone chopped by a fast amplitude buzz.
				// Two oscillators, one audio-rate and one at the wing-beat
				// rate, multiplied — that ring-mod is the insect quality.
				v.phase += v.inc;
				v.phase2 += v.inc >> 7;
				int32_t tone = fast_sin(v.phase) >> 4;
				int32_t buzz = (fast_sin(v.phase2) >> 8) + 128;   // 0..256
				s = (tone * buzz) >> 8;
				s += rand_bipolar(v.noiseRng) >> 6;   // a little chitin hiss
				break;
			}
			case Mode::Meteors:
			default:
			{
				// Filtered noise swoosh: the cutoff opens as the envelope
				// decays, giving the sound its passing-overhead sweep.
				int32_t n = rand_bipolar(v.noiseRng) >> 3;
				int32_t k = 1 + (v.env >> 14);        // 1..4
				v.filt  += (n - v.filt) >> k;
				v.filt2 += (v.filt - v.filt2) >> 3;
				v.phase += v.inc;
				s = v.filt2 + (fast_sin(v.phase) >> 6);
				break;
			}
			}

			// Apply and advance the amplitude envelope. 32-bit multiply for the
			// same reason as the PCM fade above: synth output is 12-bit too.
			s = (s * v.env) >> 16;
			v.env = fast_exp_decay(v.env, v.decayShift);
		}

		// Below audibility (or a synth voice whose envelope already reached
		// zero, which skips the branch above): return it to the pool so the next
		// note can have it without a steal.
		if (!usePcm_ && v.env < 64) { v.agent = -1; continue; }

		// Silence agents outside the current population.
		if (v.agent >= active) continue;

		accL += mul_q15(s, v.panL);
		accR += mul_q15(s, v.panR);
	}

	l = clamp12(accL);
	r = clamp12(accR);
}

} // namespace bio
