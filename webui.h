// webui.h — USB-MIDI/SysEx sample upload for BioMimicry.
//
// The card enumerates as a USB-MIDI device; interface.html talks to it over
// WebMIDI SysEx. Drop WAVs in the browser, they arrive as 8-bit mono 48kHz and
// are written into the user flash region — no toolchain, no reflash.
//
// SysEx payloads must be 7-bit (every byte 0-127), so audio is 7-bit encoded:
// 7 source bytes become 8 septets, the 8th carrying the high bits. Same scheme
// WorkshopZX uses for snapshots.
//
// IMPORTANT: writing flash on the RP2040 stalls XIP, so any code still running
// from flash halts mid-erase. Rather than make the whole audio path
// RAM-resident, the card enters an UPLOAD MODE for the duration: the ecosystem
// stops, audio is muted and the LEDs show progress. Uploading is a setup
// activity, not a performance one.

#pragma once
#include <stdint.h>
#include "biomimicry.h"
#include "samplestore.h"

namespace bio {

// SysEx message IDs (first payload byte). fw = firmware, ui = browser.
enum : uint8_t {
	MSG_HELLO      = 0x01,  // ui->fw: announce presence; fw replies MSG_INFO
	MSG_INFO       = 0x02,  // fw->ui: version, capacity, what is loaded
	MSG_UP_BEGIN   = 0x10,  // ui->fw: start upload session (stops audio; appends)
	MSG_UP_SLOT    = 0x11,  // ui->fw: begin a mode+variant; payload mode,variant,len
	MSG_UP_CHUNK   = 0x12,  // ui->fw: 7-bit-encoded audio for the current slot
	MSG_UP_SLOTEND = 0x13,  // ui->fw: this slot is complete
	MSG_UP_END     = 0x14,  // ui->fw: whole upload done -> commit header, resume
	MSG_UP_ACK     = 0x15,  // fw->ui: ready for more / progress
	MSG_UP_ERR     = 0x16,  // fw->ui: something went wrong; payload = code
	MSG_ERASE      = 0x20,  // ui->fw: forget user samples, revert to baked
	MSG_PROF_GET   = 0x30,  // ui->fw: send the timing peaks (profile builds)
	MSG_PROF       = 0x31,  // fw->ui: peak cycles per bucket + overrun count
};

// Manufacturer ID 0x7D = "prototyping / private use".
constexpr uint8_t kManufacturerId = 0x7D;

// Error codes carried by MSG_UP_ERR.
enum : uint8_t {
	ERR_NONE = 0, ERR_TOO_BIG = 1, ERR_BAD_SLOT = 2, ERR_PROTOCOL = 3,
};

/// 7-bit encode/decode. Returns bytes written.
/// Encoded length = ceil(srcLen/7)*8; decoded length <= (srcLen/8)*7.
uint32_t Encode7bit(const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t dstMax);
uint32_t Decode7bit(const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t dstMax);

/// USB-MIDI transport + upload state machine.
class WebUI
{
public:
	void Init();
	/// Poll USB. Cheap when idle; call from the audio callback.
	void Task();

	/// True while an upload is in progress — the card mutes and stops running
	/// the ecosystem, because flash writes stall execution anyway.
	bool Uploading() const { return uploading_; }

	/// True once an upload has begun. The audio interrupt is disabled for the
	/// whole upload and the card reboots when it ends, so this is a one-way
	/// latch: the card is an upload appliance until it restarts.
	///
	/// This replaces a `flashBusy` flag that core 0 spun on from inside its DMA
	/// interrupt handler. That deadlocked every time — see EnterUploadMode().
	static volatile bool uploadMode;

	/// Set by core 0 once it has stopped inside its RAM-resident handler and is
	/// spinning. Core 1 waits for this before touching flash — without it, core 0
	/// may still be executing ComputerCard's flash-resident callback when XIP
	/// drops, which hard-faults the chip.
	static volatile bool core0Parked;

	/// 0..kQ16One, for the LED progress display.
	int32_t Progress() const;

private:
	void HandleSysex(const uint8_t *msg, uint32_t len);
	void Send(const uint8_t *payload, uint32_t len);
	void SendAck(uint8_t code, uint32_t value);
	void SendErr(uint8_t code);
	void FlushPage();          // commit the 256-byte staging page to flash
	void CommitHeader();

	bool     uploading_ = false;
	uint8_t  slotMode_ = 0, slotVariant_ = 0;
	uint32_t writeOff_ = 0;     // next byte offset within the data area
	uint32_t expected_ = 0;     // total bytes announced for this session

	// Flash writes must be whole 256-byte pages, so bytes are staged here.
	uint8_t  page_[256];
	uint32_t pageFill_ = 0;
	uint32_t pageAddr_ = 0;

	// The header being built up as slots arrive; written last so a half-finished
	// upload never leaves a valid-looking directory behind.
	UserSampleHeader hdr_;

	uint8_t  rx_[1024];
	uint32_t rxLen_ = 0;
	bool     inSysex_ = false;
	// Send() pumps tud_task() so replies leave immediately; this stops that
	// nested call from recursing without bound.
	bool     pumping_ = false;
};

} // namespace bio
