// BioMimicry — organic rhythms for the Music Thing Workshop Computer.
//
// Five physics engines (Horses, Geese, Frogs, Rain, Meteors) drive four agents.
// The engines run at 1.5kHz; voices and outputs run at the full 48kHz.
//
// Hardware realities that shape the mapping (the Computer has 2 pulse outs, 2 CV
// outs and ONE three-position switch):
//   * Discrete routing puts agents 1-2 on the pulse outs and fires agents 3-4 as
//     5V blips on the CV outs, so all four get a physical trigger.
//   * The single switch does double duty: a Down tap cycles the mode, while the
//     Up/Middle position selects the routing.
//   * Holding Down at power-on selects the PCM voice bank instead of the synth.

#include "ComputerCard.h"
#include "biomimicry.h"
#include "engines.h"
#include "voices.h"

using namespace bio;

// Gate width for the pulse outs and the CV-out trigger blips: 5ms at 48kHz is
// comfortably long enough for any drum module or envelope to register.
static constexpr int kGateSamples = 240;

// How long to sample the switch at startup before deciding the boot mode. The
// switch only becomes readable once the audio worker is running, so this window
// also covers the release of the hold.
static constexpr int kBootWindowSamples = 48000 / 2;   // ~0.5s

class BioMimicryCard : public ComputerCard
{
public:
	BioMimicryCard()
	{
		engines_[0] = &horses_;
		engines_[1] = &geese_;
		engines_[2] = &frogs_;
		engines_[3] = &rain_;
		engines_[4] = &meteors_;
	}

	virtual void ProcessSample()
	{
		// ---- Boot window ------------------------------------------------
		// Decide the voice backend before any normal switch handling, so the
		// power-on hold is never mistaken for a mode-cycle tap.
		if (bootPhase_ < kBootWindowSamples)
		{
			if (SwitchVal() == Switch::Down) bootHeldDown_ = true;
			if (++bootPhase_ == kBootWindowSamples)
			{
				voices_.init(bootHeldDown_);
				engines_[mode_]->reset(0xB10Du);
				// Swallow the release of the boot hold.
				switchArmed_ = (SwitchVal() != Switch::Down);
			}
			return;
		}

		// ---- Control tick (1.5kHz) --------------------------------------
		if (++ctrlDiv_ >= kCtrlDiv)
		{
			ctrlDiv_ = 0;
			controlTick();
		}

		// ---- Audio (48kHz) ----------------------------------------------
		int16_t l, r;
		voices_.render(population_, l, r);
		AudioOut1(l);
		AudioOut2(r);

		// ---- Gate/blip timers (48kHz) -----------------------------------
		serviceOutputs();
	}

private:
	// -------------------------------------------------------------------
	void controlTick()
	{
		// --- Switch: Down taps cycle the mode, Up/Middle sets routing. ---
		Switch sw = SwitchVal();
		if (sw == Switch::Down)
		{
			if (switchArmed_)
			{
				switchArmed_ = false;
				mode_ = static_cast<uint8_t>((mode_ + 1) % kNumModes);
				engines_[mode_]->reset(seed_ ^ (mode_ * 2654435761u));
				clearOutputs();
			}
		}
		else
		{
			switchArmed_ = true;
			routing_ = (sw == Switch::Up) ? Routing::Discrete : Routing::Summed;
		}

		// --- Knobs, with CV modulating Main and X. ---
		int32_t physics = knob_to_q16(KnobVal(Knob::Main));
		if (Connected(Input::CV1)) physics += CVIn1() << 4;
		physics = clampQ16(physics);

		int32_t popRaw = knob_to_q16(KnobVal(Knob::X));
		if (Connected(Input::CV2)) popRaw += CVIn2() << 4;
		popRaw = clampQ16(popRaw);

		// Clock tracking for Pulse In 2. The period between edges lets the
		// engines entrain to an external tempo rather than merely be nudged by
		// it. It ages out after ~3s of silence so a stopped clock releases the
		// ecosystem back to its own timing instead of freezing it.
		if (clockAge_ < 3 * kCtrlRate) clockAge_++;
		else clockPeriod_ = 0;

		if (clockPending_)
		{
			if (clockAge_ >= 2 && clockAge_ < 3 * kCtrlRate) clockPeriod_ = clockAge_;
			clockAge_ = 0;
		}

		Ctrl c;
		c.physics     = physics;
		c.chaos       = knob_to_q16(KnobVal(Knob::Y));
		c.population  = 1 + (popRaw * kNumAgents - 1) / kQ16One;
		if (c.population < 1) c.population = 1;
		if (c.population > kNumAgents) c.population = kNumAgents;
		c.spook       = spookPending_;
		c.clock       = clockPending_;
		c.clockPeriod = clockPeriod_;
		spookPending_ = false;
		clockPending_ = false;
		population_   = c.population;

		// --- Physics. ---
		EngineOut out;
		engines_[mode_]->tick(c, out);

		// Agents beyond the population never sound or fire.
		uint8_t mask = out.triggers & static_cast<uint8_t>((1 << c.population) - 1);

		// --- Voices. ---
		Mode m = static_cast<Mode>(mode_);
		for (int i = 0; i < kNumAgents; i++)
			if (mask & (1 << i))
				voices_.note(i, m, kQ16One, out.state[i]);

		// --- Trigger outputs. ---
		if (routing_ == Routing::Discrete)
		{
			if (mask & 0b0001) pulseTimer_[0] = kGateSamples;
			if (mask & 0b0010) pulseTimer_[1] = kGateSamples;
			if (mask & 0b0100) cvTimer_[0]    = kGateSamples;
			if (mask & 0b1000) cvTimer_[1]    = kGateSamples;
		}
		else
		{
			// Summed: everything OR'd onto Pulse 1; Pulse 2 marks a "cluster"
			// (two or more agents firing at once) as an accent.
			if (mask) pulseTimer_[0] = kGateSamples;
			if (popcount4(mask) >= 2) pulseTimer_[1] = kGateSamples;

			// CV outs carry continuous state instead of triggers.
			cvLevel_[0] = out.state[0];
			cvLevel_[1] = out.global;
		}

		// --- LEDs: 0-4 show the mode, 5 glows with activity. ---
		for (int i = 0; i < kNumModes; i++) LedOn(i, i == mode_);
		if (mask) activity_ = kQ16One;
		LedBrightness(5, static_cast<uint16_t>(activity_ >> 4));
		activity_ = fast_exp_decay(activity_, 3);

		seed_ = seed_ * 1664525u + 1013904223u;
	}

	// -------------------------------------------------------------------
	// 48kHz: run down the gate widths and hold the CV outs.
	void serviceOutputs()
	{
		// Pulse ins are edge-detected at audio rate so a short trigger is never
		// missed between control ticks.
		if (PulseIn1RisingEdge()) spookPending_ = true;
		if (PulseIn2RisingEdge()) clockPending_ = true;

		PulseOut1(pulseTimer_[0] > 0);
		PulseOut2(pulseTimer_[1] > 0);
		if (pulseTimer_[0] > 0) pulseTimer_[0]--;
		if (pulseTimer_[1] > 0) pulseTimer_[1]--;

		for (int i = 0; i < 2; i++)
		{
			if (routing_ == Routing::Discrete)
			{
				// CV out as a trigger: a calibrated 5V blip.
				CVOutMillivolts(i, cvTimer_[i] > 0 ? 5000 : 0);
				if (cvTimer_[i] > 0) cvTimer_[i]--;
			}
			else
			{
				// CV out as continuous state, 0-5V. Slewed a little so stepped
				// control-rate updates don't click.
				cvSmooth_[i] = slew(cvSmooth_[i], cvLevel_[i], 4);
				CVOutMillivolts(i, (cvSmooth_[i] * 5000) >> 16);
			}
		}
	}

	void clearOutputs()
	{
		pulseTimer_[0] = pulseTimer_[1] = 0;
		cvTimer_[0] = cvTimer_[1] = 0;
		cvLevel_[0] = cvLevel_[1] = 0;
	}

	static int32_t clampQ16(int32_t v)
	{
		if (v < 0) return 0;
		if (v > kQ16One) return kQ16One;
		return v;
	}

	static int popcount4(uint8_t m)
	{
		return ((m >> 0) & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1);
	}

	// -------------------------------------------------------------------
	HorsesEngine  horses_;
	GeeseEngine   geese_;
	FrogsEngine   frogs_;
	RainEngine    rain_;
	MeteorsEngine meteors_;
	Engine       *engines_[kNumModes];

	VoiceBank voices_;

	uint8_t  mode_       = 0;
	Routing  routing_    = Routing::Discrete;
	int      population_ = 1;

	int      ctrlDiv_    = 0;
	bool     switchArmed_ = false;
	bool     spookPending_ = false;
	bool     clockPending_ = false;
	int32_t  clockPeriod_  = 0;   // control ticks between Pulse In 2 edges
	int32_t  clockAge_     = 0;   // ticks since the last edge
	int32_t  activity_   = 0;
	uint32_t seed_       = 0xC0FFEEu;

	int      pulseTimer_[2] = { 0, 0 };
	int      cvTimer_[2]    = { 0, 0 };
	int32_t  cvLevel_[2]    = { 0, 0 };
	int32_t  cvSmooth_[2]   = { 0, 0 };

	int      bootPhase_    = 0;
	bool     bootHeldDown_ = false;
};

int main()
{
	static BioMimicryCard card;
	card.EnableNormalisationProbe();
	card.Run();
}
