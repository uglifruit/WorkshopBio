// voices.cpp — voice rendering at 48kHz.
//
// Each mode gets a timbre that suggests its creature without pretending to be a
// recording, plus a round-robin variant system and a stereo strategy that comes
// from the ecosystem rather than from a knob.

#include "voices.h"
#include "samples_default.h"   // pulls in samples.h if baked, else stubs

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

// Round-robin pitch offsets, Q16. In Horses these are the four hooves (hinds
// heavier and lower than fores); elsewhere they are simply four individuals.
static const int32_t kVariantPitch[kNumModes][kNumVariants] = {
	// Horses: LH, LF, RH, RF. Hind hooves strike lower and heavier than fores,
	// so the gait has an audible front/back shape, not just left/right.
	{  56000,  74000,  60000,  79000 },
	// Geese: four birds of different size.
	{  65536,  52000,  78000,  44000 },
	// Frogs: four species in the chorus.
	{  65536,  49000,  88000,  61000 },
	// Rain: four drip sizes.
	{  65536,  81000,  53000,  92000 },
	// Meteors: four distances.
	{  65536,  48000,  86000,  57000 },
	// Cicadas: four insects, tight spread — a real field is fairly uniform.
	{  65536,  69000,  62000,  72000 }
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

void VoiceBank::init(bool usePcm)
{
	usePcm_ = usePcm && kHaveSamples;
	for (int i = 0; i < kNumAgents; i++)
	{
		Voice &v = v_[i];
		v.env = 0; v.phase = 0; v.phase2 = 0; v.inc = 0; v.inc2 = 0;
		v.pitchEnv = 0;
		v.noiseRng = 0x1234567u + static_cast<uint32_t>(i) * 2654435761u;
		v.filt = 0; v.filt2 = 0; v.bp = 0;
		v.decayShift = 10; v.mode = 0; v.variant = 0;
		v.ksLen = 64; v.ksPos = 0;
		for (int k = 0; k < 128; k++) v.ks[k] = 0;
		v.pcm = nullptr; v.pcmLen = 0; v.pcmPos = 0; v.pcmInc = 65536;
		setPan(v, 2 + i * 4);
		lastVariant_[i] = 0xFF;

		DroneVoice &d = d_[i];
		d.phase = 0; d.phase2 = 0; d.inc = 0; d.inc2 = 0;
		d.pitch = kQ16One; d.level = 0; d.filt = 0; d.bright = 0;
		d.noiseRng = 0x2468ACEu + static_cast<uint32_t>(i) * 40503u;
	}
}

void VoiceBank::selectVariantAndPan(Voice &v, int agent, Mode m, uint8_t member)
{
	int mi = static_cast<int>(m);

	if (m == Mode::Horses)
	{
		// The variant IS the hoof that landed, reported by the engine. A gait
		// only sounds like an animal when each leg has its own voice — and with
		// a herd, each horse plays all four of its own hooves.
		v.variant = static_cast<uint8_t>(member & (kNumVariants - 1));
	}
	else
	{
		// Round robin with no immediate repeat, so you never hear the same
		// honk or drip twice running.
		uint8_t pick;
		do {
			pick = static_cast<uint8_t>(xorshift32(rng_) & (kNumVariants - 1));
		} while (pick == lastVariant_[agent] && kNumVariants > 1);
		v.variant = pick;
		lastVariant_[agent] = pick;
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
		// Agent's spot, offset by which swarm member this is, so the twelve
		// birds occupy twelve places rather than four.
		setPan(v, 1 + agent * 4 + (v.variant & 3));
		break;
	case PanMode::Random:
		// Anywhere — every drip and every meteor is a new object.
		setPan(v, static_cast<int>(xorshift32(rng_) % 17));
		break;
	}
}

void VoiceBank::note(int i, Mode m, int32_t accent, int32_t variation,
                     uint8_t member)
{
	Voice &v = v_[i];
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
		v.pcm    = kModeSample[mi] + kModeSampleOff[mi][v.variant];
		v.pcmLen = kModeSampleSize[mi][v.variant];
		v.pcmPos = 0;
		// Real recordings already differ from each other, so play them at pitch
		// rather than stacking the synth's variant transposition on top.
		v.pcmInc = 65536;
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

// Drone root pitches per mode, as 48kHz increments. Deliberately low and spread
// so four agents make a chord rather than a cluster.
static const uint32_t kDroneBase[kNumModes] = {
	 5400000u,   // Horses  ~60Hz  — a low, heavy drone
	 9800000u,   // Geese   ~110Hz — reedy
	 7300000u,   // Frogs   ~82Hz  — the chorus
	14600000u,   // Rain    ~165Hz — brighter, watery
	 3600000u,   // Meteors ~40Hz  — subsonic rumble
	21900000u    // Cicadas ~245Hz — high shimmer
};

// Per-agent interval, Q16 pitch multipliers. Root, fifth, octave, minor tenth —
// a stack that stays consonant however many agents are active.
static const int32_t kDroneInterval[kNumAgents] = { 65536, 98304, 131072, 157286 };

void VoiceBank::droneUpdate(Mode m, const int32_t *state, int32_t global,
                            uint8_t triggers, int active, int32_t timbre)
{
	int mi = static_cast<int>(m);

	for (int i = 0; i < kNumAgents; i++)
	{
		DroneVoice &d = d_[i];

		if (i >= active)
		{
			// Fade out rather than cut, so changing population is a swell.
			d.level = slew(d.level, 0, 6);
			continue;
		}

		// The agent's own state bends its pitch by up to a fifth. This is the
		// heart of the mode: whatever the engine is doing internally is now
		// audible as continuous motion instead of as discrete hits.
		int32_t bend = kQ16One + (state[i] >> 1);
		int32_t target = mul_q16(kDroneInterval[i], bend);
		d.pitch = slew(d.pitch, target, 5);

		// The whole-ecosystem value drives brightness, so the texture opens and
		// closes with the global behaviour — density, agitation, coherence.
		d.bright = slew(d.bright, global, 6);

		// A trigger is an accent, not a note: a small level bump on top of a
		// tone that never stops.
		int32_t lvlTarget = (kQ16One * 3) / 5;
		if (triggers & (1 << i)) lvlTarget = kQ16One;
		d.level = slew(d.level, lvlTarget, (triggers & (1 << i)) ? 2 : 7);

		// Set the oscillator increments; droneRender advances the phases at
		// audio rate. Knob Y detunes the pair, from a unison shimmer out to a
		// wide slow beating.
		uint32_t base = kDroneBase[mi];
		d.inc = static_cast<uint32_t>(
			(static_cast<int64_t>(base) * d.pitch) >> 16);
		d.inc2 = d.inc + (d.inc >> 9) + static_cast<uint32_t>(timbre >> 6);
	}
}

void VoiceBank::droneRender(int active, int16_t &l, int16_t &r)
{
	int32_t accL = 0, accR = 0;

	for (int i = 0; i < kNumAgents; i++)
	{
		DroneVoice &d = d_[i];
		if (d.level <= 0 && i >= active) continue;

		// Two detuned saws through a one-pole opened by the ecosystem's global
		// state. Saws rather than sines so the filter has something to work on.
		// The oscillators are advanced here, at 48kHz — droneUpdate only sets
		// the increments, at the 1.5kHz control rate.
		d.phase  += d.inc;
		d.phase2 += d.inc2;
		int32_t a = static_cast<int32_t>(d.phase  >> 20) - 2048;
		int32_t b = static_cast<int32_t>(d.phase2 >> 20) - 2048;
		int32_t s = (a + b) >> 1;

		// Brightness maps to filter shift 5 (dark) .. 1 (open).
		int32_t k = 5 - (d.bright >> 14);
		if (k < 1) k = 1;
		if (k > 5) k = 5;
		d.filt += (s - d.filt) >> k;
		s = d.filt;

		s = mul_q16(s, d.level) >> 1;

		// Drones sit at fixed positions, spread wide — the point is a stable
		// stereo image you can sit inside, not movement.
		accL += mul_q15(s, kPanCurveL[i]);
		accR += mul_q15(s, kPanCurveR[i]);
	}

	l = clamp12(accL);
	r = clamp12(accR);
}

void VoiceBank::render(int active, int16_t &l, int16_t &r)
{
	int32_t accL = 0, accR = 0;

	for (int i = 0; i < kNumAgents; i++)
	{
		Voice &v = v_[i];
		int32_t s = 0;

		if (usePcm_)
		{
			if (v.pcm && v.pcmPos < (v.pcmLen << 16))
			{
				// 8-bit PCM, linear-interpolated so pitch-shifted playback
				// doesn't alias badly.
				uint32_t idx = v.pcmPos >> 16;
				int32_t  mu  = static_cast<int32_t>(v.pcmPos & 0xFFFF);
				int32_t  a   = v.pcm[idx];
				int32_t  b   = (idx + 1 < v.pcmLen) ? v.pcm[idx + 1] : 0;
				s = (a + (((b - a) * mu) >> 16)) << 4;   // 8-bit -> 12-bit
				v.pcmPos += v.pcmInc;
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
				uint8_t nxt = static_cast<uint8_t>((v.ksPos + 1) % v.ksLen);
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

			// Apply and advance the amplitude envelope.
			s = mul_q16(s, v.env);
			v.env = fast_exp_decay(v.env, v.decayShift);
		}

		// Silence agents outside the current population.
		if (i >= active) continue;

		accL += mul_q15(s, v.panL);
		accR += mul_q15(s, v.panR);
	}

	l = clamp12(accL);
	r = clamp12(accR);
}

} // namespace bio
