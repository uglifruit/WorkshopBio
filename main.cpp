// BioMimicry — a program card for the Music Thing Modular Workshop System Computer.
//
// SCAFFOLD (v0.0.1): this builds, flashes, and proves the card is alive — knobs
// and CV in reach the outputs, the LEDs respond, audio passes. The BioMimicry
// engine itself goes in from here.
//
// ProcessSample() is called by ComputerCard at 48kHz. Everything is fixed-point
// and allocation-free; keep it that way. Inputs and outputs are signed 12-bit
// (-2048..2047), knobs are unsigned 12-bit (0..4095).

#include "ComputerCard.h"

class BioMimicryCard : public ComputerCard
{
public:
	BioMimicryCard() {}

	virtual void ProcessSample()
	{
		// --- Scaffold behaviour: a plain, audible sign of life. ---
		// Knob Main sets the rate of a slow LFO on CV Out 1; audio passes through
		// so a patched signal is heard. Replace all of this with the engine.

		uint32_t rate = 200 + (KnobVal(Knob::Main) >> 3); // 200..711
		phase_ += rate;

		// Triangle from the phase accumulator, scaled to the 12-bit CV range.
		uint32_t tri = phase_ >> 20;                       // 0..4095
		if (tri > 2047) tri = 4095 - tri;                  // 0..2047 fold
		int16_t lfo = static_cast<int16_t>(tri) - 1024;

		CVOut1(lfo);
		CVOut2(static_cast<int16_t>(KnobVal(Knob::X) >> 1) - 1024);

		// Pass audio through so a patched signal confirms the codec is running.
		AudioOut1(AudioIn1());
		AudioOut2(AudioIn2());

		// Gates follow the pulse inputs.
		PulseOut1(PulseIn1());
		PulseOut2(PulseIn2());

		// Slow LED sweep so an unpatched card still shows it is running.
		if (++ledDivider_ >= 3000)
		{
			ledDivider_ = 0;
			ledIndex_ = (ledIndex_ + 1) % 6;
			for (int i = 0; i < 6; i++) LedOff(i);
			LedOn(ledIndex_);
		}
	}

private:
	uint32_t phase_ = 0;
	uint16_t ledDivider_ = 0;
	uint8_t  ledIndex_ = 0;
};

int main()
{
	static BioMimicryCard card;
	card.EnableNormalisationProbe();
	card.Run();
}
