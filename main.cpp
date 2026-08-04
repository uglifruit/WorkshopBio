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
#include "samples_default.h"   // kHaveSamples
#include "samplestore.h"       // user flash region
#include "webui.h"             // USB-MIDI sample upload
#include "profile.h"           // -DBIO_PROFILE=ON: cycle-count ProcessSample
#include "pico/multicore.h"

using namespace bio;

// Shared with core 1, which does nothing but service USB. Single writer per
// field (core 0 sets them once at the end of boot, core 1 only reads), so no
// lock is needed — the same discipline WorkshopZX uses for its CrossCore.
static volatile bool  gUsbReady = false;
static WebUI * volatile gWebUI = nullptr;
// Core 1 drives the progress LEDs during an upload, when core 0's audio
// interrupt is switched off and nothing else is left running.
class BioMimicryCard;
static BioMimicryCard * volatile gCard = nullptr;

// Gate width for the pulse outs and the CV-out trigger blips: 5ms at 48kHz is
// comfortably long enough for any drum module or envelope to register.
static constexpr int kGateSamples = 240;

// How long to sample the switch at startup before deciding the boot mode. The
// switch only becomes readable once the audio worker is running, so this window
// also covers the release of the hold.
static constexpr int kBootWindowSamples = 48000 / 2;   // ~0.5s

// After booting, announce the mode on the LEDs for a moment: Rhythm lights the
// left column, Drone the right. Without this the only way to tell the modes
// apart is by ear, and a Drone patch is easily mistaken for broken samples.
static constexpr int kSplashSamples = 48000;           // ~1s

class BioMimicryCard : public ComputerCard
{
public:
	/// Progress bar during an upload, called from core 1 because core 0's audio
	/// interrupt is switched off by then. RAM-resident: this runs while flash is
	/// being erased, and LedOn() only pokes a PWM register, so it is safe there.
	void __not_in_flash_func(showUploadProgress)()
	{
		int32_t prog = gWebUI ? gWebUI->Progress() : 0;
		int lit = (prog * 6) >> 16;
		for (int i = 0; i < 6; i++) LedOn(static_cast<uint32_t>(i), i < lit);
	}

	BioMimicryCard()
	{
		engines_[0] = &horses_;
		engines_[1] = &geese_;
		engines_[2] = &frogs_;
		engines_[3] = &rain_;
		engines_[4] = &meteors_;
		engines_[5] = &cicadas_;
		// USB runs on core 1. tud_task() is TinyUSB's whole device stack and is
		// unbounded by design — measured at up to 36000 cycles, i.e. 14x the
		// entire 20.8us audio budget. Calling that from inside the audio
		// interrupt was always wrong; it only looked survivable because it is
		// intermittent, and every occurrence drops samples.
		multicore_launch_core1(core1Entry);
	}

	/// Core 1: nothing but USB. It must not touch anything core 0 owns.
	static void __not_in_flash_func(core1Entry)()
	{
		// Wait for core 0 to finish booting and construct the WebUI.
		while (!gUsbReady) tight_loop_contents();
		for (;;)
		{
			gWebUI->Task();

			// Once an upload starts, core 0's audio interrupt is off and this is
			// the only core still running — so the progress LEDs have to be
			// driven from here. LedOn() is RAM-resident and only writes a PWM
			// register, so it stays safe even mid-erase.
			if (WebUI::uploadMode && gCard) gCard->showUploadProgress();
		}
	}

	virtual void __not_in_flash_func(ProcessSample)()
	{
		// An upload is starting: park HERE, in RAM, and never return to flash.
		//
		// Masking DMA_IRQ_0 from core 1 is not sufficient on its own, which is
		// what the first two attempts got wrong. ComputerCard::AudioCallback,
		// BufferFull and AudioWorker's outer loop all live in flash, and the CV
		// output runs a second flash-resident ISR (PWM_IRQ_WRAP -> OnCVPWMWrap).
		// Any of them executing when flash_range_erase drops XIP is a hard fault.
		//
		// So core 0 stops inside this function, which IS RAM-resident, mutes the
		// outputs, tells core 1 it has arrived, and spins until the reboot. It
		// never returns, so the flash-resident caller never runs again.
		//
		// This is safe now in a way it was not before: USB lives on core 1, so
		// spinning core 0 no longer stalls the upload it is waiting for.
		if (WebUI::uploadMode)
		{
			AudioOut1(0);
			AudioOut2(0);
			PulseOut1(false);
			PulseOut2(false);
			WebUI::core0Parked = true;
			for (;;) tight_loop_contents();
		}

		// Times the WHOLE callback, which is the number that decides whether
		// audio glitches — not the per-sample average.
		BIO_PROFILE_SCOPE(Total);

		// ---- Boot window ------------------------------------------------
		// Decide the voice backend before any normal switch handling, so the
		// power-on hold is never mistaken for a mode-cycle tap.
		if (bootPhase_ < kBootWindowSamples)
		{
			// The switch is NOT readable straight away, and reads as Down until
			// it settles. ComputerCard derives it from knobs[3], which comes off
			// a ~60Hz smoothing filter starting at zero — and zero decodes as
			// Down. So for the first few milliseconds of every boot the card
			// reports Down wherever the switch actually is.
			//
			// The old code latched on "Down seen at any point in the window",
			// which therefore latched on EVERY boot: both modes came up as
			// Drone. Take a single reading once settled instead, which is what
			// WorkshopZX's BootSelector does (see its main.cpp) and what is
			// proven on this hardware.
			if (++bootPhase_ == kBootWindowSamples)
			{
				// Holding the switch at power-on picks DRONE, a different
				// instrument built from the same six engines. PCM sample
				// playback is a build-time choice now (bake samples/ or don't),
				// not something worth spending a whole boot mode on.
				boot_ = (SwitchVal() == Switch::Down) ? BootMode::Drone
				                                     : BootMode::Rhythm;
				webui_.Init();
				gCard  = this;
				gWebUI = &webui_;
				gUsbReady = true;      // releases core 1
				voices_.init(AnySamples());
				engines_[mode_]->reset(0xB10Du);
				// Swallow the release of the boot hold.
				switchArmed_ = (SwitchVal() != Switch::Down);
				// Announce what booted, because otherwise the only way to know
				// is by ear — and a Drone patch can be mistaken for broken
				// samples. See the splash pattern in ProcessSample.
				splash_ = kSplashSamples;
			}
			return;
		}

		// USB is serviced on core 1; nothing to do here. See core1Entry.
		//
		// Uploads are NOT handled here any more. This handler stops being called
		// at all once an upload starts (DMA_IRQ_0 is disabled), so the progress
		// LEDs are driven from core 1 — see core1Entry. A progress bar rendered
		// here would simply freeze on the first frame.

		// ---- Boot splash -------------------------------------------------
		// LEDs are 0 1 / 2 3 / 4 5. Rhythm lights the left column, Drone the
		// right, so which instrument you are in is visible on power-up.
		if (splash_ > 0)
		{
			if (--splash_ == 0)
			{
				for (int i = 0; i < 6; i++) LedOff(i);
				// Start measuring only now: the boot-time voices_.init() memset
				// is a one-off spike and would otherwise latch the overrun
				// indicator permanently.
				BIO_PROFILE_ARM();
			}
			else if (splash_ == kSplashSamples - 1)
			{
				bool drone = (boot_ == BootMode::Drone);
				for (int i = 0; i < 6; i++)
					LedOn(i, ((i & 1) == 1) == drone);
			}
		}

		// ---- Control tick (1.5kHz) --------------------------------------
		// NOTE: this runs INLINE, inside the DMA interrupt. Dividing by
		// kCtrlDiv lowers the average load but does NOT relax the deadline —
		// on the sample where it fires, the whole engine must still finish
		// within this one 20.83us slot. Build with -DBIO_PROFILE=ON to see
		// what that actually costs.
		// The engine and the LED/UI housekeeping used to run on the SAME sample,
		// stacking their costs into one peak. They are now offset by half the
		// divider, so each gets its own sample. Same rates, lower worst case.
		if (++ctrlDiv_ >= kCtrlDiv) ctrlDiv_ = 0;
		if (ctrlDiv_ == 0)
		{
			BIO_PROFILE_SCOPE(Engine);
			controlTick();
		}
		else if (ctrlDiv_ == kCtrlDiv / 2)
		{
			BIO_PROFILE_SCOPE(Outputs);
			uiTick();
		}

		// ---- Audio (48kHz) ----------------------------------------------
		int16_t l, r;
		{
			BIO_PROFILE_SCOPE(Voices);
			if (boot_ == BootMode::Drone) voices_.droneRender(population_, l, r);
			else                          voices_.render(population_, l, r);
		}
		AudioOut1(l);
		AudioOut2(r);

		{
			BIO_PROFILE_SCOPE(Outputs);
			// ---- Listening (48kHz) --------------------------------------
			listen();

			// ---- Gate/blip timers (48kHz) -------------------------------
			serviceOutputs();
		}

#ifdef BIO_PROFILE
		profileReadout();
#endif
	}

private:
#ifdef BIO_PROFILE
	// -------------------------------------------------------------------
	// The first hardware run flashed all six LEDs in every mode, i.e. something
	// overruns everywhere. "Everywhere" points at shared cost rather than any
	// one engine, so the readout now CYCLES through the buckets and names the
	// culprit instead of only saying that one exists.
	//
	// Each bucket gets ~1.5s. Its identity is shown on the LEFT column
	// (LEDs 0/2/4) as a 3-bit number, and its peak as a fraction of budget on
	// the RIGHT column (LEDs 1/3/5), one LED per third. A bucket that alone
	// exceeds budget flashes its right column.
	//
	//   left 000 = Total    001 = Engine   010 = Voices
	//        011 = Outputs  100 = Usb
	void profileReadout()
	{
		if (++profDiv_ < 700) return;       // ~68Hz update
		profDiv_ = 0;

		if (++profPhase_ >= 100)            // ~1.5s per bucket
		{
			profPhase_ = 0;
			profBucket_ = (profBucket_ + 1) % kNumProf;
		}
		profFlash_ = !profFlash_;

		const ProfStat &s = gProf[profBucket_];
		uint32_t frac = static_cast<uint32_t>(
			static_cast<uint64_t>(s.peak) * 65536u / kCycleBudget);

		// Left column: which bucket, in binary.
		LedOn(0, (profBucket_ & 1) != 0);
		LedOn(2, (profBucket_ & 2) != 0);
		LedOn(4, (profBucket_ & 4) != 0);

		// Right column: that bucket's peak, a third of the budget per LED.
		if (frac >= 65536)
		{
			// Over budget on its own — flash so it is unmistakable.
			LedOn(1, profFlash_); LedOn(3, profFlash_); LedOn(5, profFlash_);
		}
		else
		{
			int lit = static_cast<int>((frac * 3) >> 16);
			LedOn(1, lit > 0);
			LedOn(3, lit > 1);
			LedOn(5, lit > 2);
		}

		// A Down tap clears the peaks, so each mode can be measured cleanly
		// without power-cycling.
		if (SwitchVal() == Switch::Down) BIO_PROFILE_RESET();
	}

	int  profDiv_ = 0;
	int  profPhase_ = 0;
	int  profBucket_ = 0;
	bool profFlash_ = false;

	// PulseOut2 is protected, so the gate hook goes through a static trampoline
	// holding the one card instance. Only compiled in profile builds, and it
	// takes over Pulse Out 2 — do not release a build with this on.
	static BioMimicryCard *profCard_;
	static void ProfGate(bool on) { if (profCard_) profCard_->PulseOut2(on); }
public:
	void EnableProfileGate() { profCard_ = this; bio::gProfGate = &ProfGate; }
private:
#endif

	// -------------------------------------------------------------------
	void __not_in_flash_func(controlTick)()
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
		// A sharp transient on Audio In 2 counts as a spook, so the ecosystem
		// reacts to the rest of the patch and not only to a patched gate.
		c.spook       = spookPending_ || startle_;
		c.clock       = clockPending_;
		c.clockPeriod = clockPeriod_;
		c.loudness    = loudness_;
		spookPending_ = false;
		clockPending_ = false;
		startle_      = false;
		population_   = c.population;

		// --- Physics. ---
		EngineOut out;
		out.triggers = 0;
		out.global = 0;
		for (int i = 0; i < kNumAgents; i++) { out.state[i] = 0; out.member[i] = 0; }
		engines_[mode_]->tick(c, out);

		// Agents beyond the population never sound or fire.
		uint8_t mask = out.triggers & static_cast<uint8_t>((1 << c.population) - 1);

		// --- Voices. ---
		Mode m = static_cast<Mode>(mode_);
		if (boot_ == BootMode::Drone)
		{
			voices_.droneUpdate(m, out.state, out.global, mask, c.population,
			                    c.chaos);
		}
		else
		{
			for (int i = 0; i < kNumAgents; i++)
				if (mask & (1 << i))
					voices_.note(i, m, kQ16One, out.state[i], out.member[i]);
		}

		// --- Trigger outputs. ---
		if (boot_ == BootMode::Drone)
		{
			// Drone pairs each gate with the DENSITY of the texture it came
			// from, so the audio out and the control outs describe the same
			// thing: you can hear a layer and modulate something else with how
			// thick it is.
			//
			// Switch Up   — one voice:  gates on Pulse 1, its density on CV 1.
			// Switch Mid  — two voices: agents 1 and 2 on Pulse 1 and Pulse 2,
			//               their densities on CV 1 and CV 2.
			if (routing_ == Routing::Discrete)
			{
				if (mask) pulseTimer_[0] = kGateSamples;
				cvLevel_[0] = voices_.droneDensity(0);
				cvLevel_[1] = out.global;
			}
			else
			{
				if (mask & 0b0001) pulseTimer_[0] = kGateSamples;
				if (mask & 0b0010) pulseTimer_[1] = kGateSamples;
				cvLevel_[0] = voices_.droneDensity(0);
				cvLevel_[1] = voices_.droneDensity(1);
			}
		}
		else if (routing_ == Routing::Discrete)
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

		// Triggers feed the activity glow, which uiTick() renders on its own
		// sample so the two costs do not stack.
		if (mask) activity_ = kQ16One;

		seed_ = seed_ * 1664525u + 1013904223u;
	}

	// -------------------------------------------------------------------
	// LED housekeeping, run on a different sample from the engine so their
	// costs never land in the same 20.8us slot.
	void __not_in_flash_func(uiTick)()
	{
		activity_ = fast_exp_decay(activity_, 3);

		// Leave the LEDs alone while the boot splash is still showing.
		if (splash_ != 0) return;

		// Six modes on six LEDs leaves none spare for an activity indicator, so
		// the two jobs share: the mode's own LED sits at a dim "you are here"
		// glow and flares to full on every trigger. One light, both meanings.
		for (int i = 0; i < kNumModes; i++)
		{
			if (i == mode_)
			{
				constexpr int32_t kIdleGlow = kQ16One / 5;
				int32_t level = kIdleGlow
				              + mul_q16(activity_, kQ16One - kIdleGlow);
				LedBrightness(i, static_cast<uint16_t>(level >> 4));
			}
			else
			{
				LedOff(i);
			}
		}
	}

	// -------------------------------------------------------------------
	// 48kHz: the ecosystem listens to the rest of the patch.
	//
	// Audio In 1 is LOUDNESS — a fast envelope follower. A loud room is a
	// disturbed one: it spooks the geese, silences the cicadas and startles the
	// herd. Audio In 2 is DISTURBANCE — the same signal differentiated, so it
	// responds to transients rather than level, which is what actually alarms an
	// animal. Both only act when something is patched in.
	void __not_in_flash_func(listen)()
	{
		if (Connected(Input::Audio1))
		{
			int32_t a = AudioIn1();
			if (a < 0) a = -a;
			// Fast attack, slow release: an envelope that tracks how alive the
			// room is rather than the waveform itself.
			if (a > loudness_) loudness_ = slew(loudness_, a << 4, 3);
			else               loudness_ = slew(loudness_, a << 4, 9);
			if (loudness_ > kQ16One) loudness_ = kQ16One;
		}
		else loudness_ = 0;

		if (Connected(Input::Audio2))
		{
			int32_t a = AudioIn2();
			int32_t d = a - lastAudio2_;
			lastAudio2_ = a;
			if (d < 0) d = -d;
			// A sharp transient arms a startle that the control tick consumes.
			if ((d << 4) > kQ16One / 3) startle_ = true;
		}
		else lastAudio2_ = 0;
	}

	// -------------------------------------------------------------------
	// 48kHz: run down the gate widths and hold the CV outs.
	void __not_in_flash_func(serviceOutputs)()
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
			// Only Rhythm's Discrete routing uses the CV outs as triggers
			// (agents 3 and 4, which have no pulse out of their own). Drone
			// always carries continuous density instead, in both positions.
			bool cvAsTrigger = (boot_ == BootMode::Rhythm)
			                && (routing_ == Routing::Discrete);
			if (cvAsTrigger)
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
	CicadasEngine cicadas_;
	Engine       *engines_[kNumModes];

	VoiceBank voices_;
	WebUI     webui_;

	uint8_t  mode_       = 0;
	Routing  routing_    = Routing::Discrete;
	int      population_ = 1;

	int      ctrlDiv_    = 0;
	bool     switchArmed_ = false;
	bool     spookPending_ = false;
	bool     clockPending_ = false;
	int32_t  clockPeriod_  = 0;   // control ticks between Pulse In 2 edges
	int32_t  clockAge_     = 0;   // ticks since the last edge
	int32_t  loudness_     = 0;   // Q16 envelope of Audio In 1
	int32_t  lastAudio2_   = 0;
	bool     startle_      = false;
	BootMode boot_         = BootMode::Rhythm;
	int32_t  activity_   = 0;
	uint32_t seed_       = 0xC0FFEEu;

	int      pulseTimer_[2] = { 0, 0 };
	int      cvTimer_[2]    = { 0, 0 };
	int32_t  cvLevel_[2]    = { 0, 0 };
	int32_t  cvSmooth_[2]   = { 0, 0 };

	int      bootPhase_    = 0;
	int      splash_       = 0;   // boot-mode announcement countdown
};

#ifdef BIO_PROFILE
BioMimicryCard *BioMimicryCard::profCard_ = nullptr;
#endif

int main()
{
	BIO_PROFILE_INIT();
	static BioMimicryCard card;
	card.EnableNormalisationProbe();
#ifdef BIO_PROFILE
	card.EnableProfileGate();      // Pulse Out 2 mirrors the callback duration
#endif
	card.Run();
}
