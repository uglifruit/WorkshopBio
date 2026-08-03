// webui.cpp — USB-MIDI SysEx sample upload.

#include "webui.h"
#include <string.h>
#include "tusb.h"
#include "profile.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

namespace bio {

// ---------------------------------------------------------------------------
// 7-bit encoding
// ---------------------------------------------------------------------------

uint32_t Encode7bit(const uint8_t *src, uint32_t srcLen, uint8_t *dst,
                    uint32_t dstMax)
{
	uint32_t o = 0;
	for (uint32_t i = 0; i < srcLen; i += 7)
	{
		uint32_t n = (srcLen - i < 7) ? (srcLen - i) : 7;
		if (o + n + 1 > dstMax) break;
		uint8_t high = 0;
		for (uint32_t k = 0; k < n; k++)
			if (src[i + k] & 0x80) high |= static_cast<uint8_t>(1u << k);
		dst[o++] = high;
		for (uint32_t k = 0; k < n; k++)
			dst[o++] = src[i + k] & 0x7F;
	}
	return o;
}

uint32_t Decode7bit(const uint8_t *src, uint32_t srcLen, uint8_t *dst,
                    uint32_t dstMax)
{
	uint32_t o = 0;
	for (uint32_t i = 0; i < srcLen; i += 8)
	{
		uint8_t high = src[i];
		uint32_t n = (srcLen - i - 1 < 7) ? (srcLen - i - 1) : 7;
		for (uint32_t k = 0; k < n; k++)
		{
			if (o >= dstMax) return o;
			dst[o++] = static_cast<uint8_t>(src[i + 1 + k]
			         | ((high & (1u << k)) ? 0x80 : 0x00));
		}
	}
	return o;
}

// ---------------------------------------------------------------------------
// Flash writing
// ---------------------------------------------------------------------------
//
// Erase and program stall XIP, so the caller must already have stopped audio
// (see WebUI::Uploading). Both cores must be quiet: core 1 is not running any
// BioMimicry code, but interrupts are disabled anyway because a flash-resident
// ISR firing mid-erase would hard-fault.

static void __not_in_flash_func(EraseRegion)(uint32_t off, uint32_t len)
{
	uint32_t ints = save_and_disable_interrupts();
	flash_range_erase(off, len);
	restore_interrupts(ints);
}

static void __not_in_flash_func(ProgramPage)(uint32_t off, const uint8_t *data)
{
	uint32_t ints = save_and_disable_interrupts();
	flash_range_program(off, data, 256);
	restore_interrupts(ints);
}

// ---------------------------------------------------------------------------

void WebUI::Init()
{
	tusb_init();
	memset(&hdr_, 0, sizeof(hdr_));
}

int32_t WebUI::Progress() const
{
	if (!uploading_ || expected_ == 0) return 0;
	uint64_t p = static_cast<uint64_t>(writeOff_) * kQ16One / expected_;
	return (p > kQ16One) ? kQ16One : static_cast<int32_t>(p);
}

void WebUI::Task()
{
	tud_task();

	uint8_t packet[4];
	while (tud_midi_available())
	{
		if (!tud_midi_packet_read(packet)) break;

		// Reassemble SysEx from USB-MIDI 4-byte packets. The code-index number
		// in the low nibble tells us how many of the three data bytes are real.
		uint8_t cin = packet[0] & 0x0F;
		uint32_t n = 0;
		switch (cin)
		{
		case 0x4: n = 3; break;                 // sysex continues
		case 0x5: n = 1; break;                 // ends with 1 byte
		case 0x6: n = 2; break;                 // ends with 2 bytes
		case 0x7: n = 3; break;                 // ends with 3 bytes
		default:  n = 0; break;
		}

		for (uint32_t i = 0; i < n; i++)
		{
			uint8_t b = packet[1 + i];
			if (b == 0xF0) { inSysex_ = true; rxLen_ = 0; continue; }
			if (b == 0xF7)
			{
				if (inSysex_) HandleSysex(rx_, rxLen_);
				inSysex_ = false;
				rxLen_ = 0;
				continue;
			}
			if (inSysex_ && rxLen_ < sizeof(rx_)) rx_[rxLen_++] = b;
		}
	}
}

void WebUI::Send(const uint8_t *payload, uint32_t len)
{
	uint8_t buf[64];
	if (len + 3 > sizeof(buf)) return;
	uint32_t o = 0;
	buf[o++] = 0xF0;
	buf[o++] = kManufacturerId;
	memcpy(buf + o, payload, len);
	o += len;
	buf[o++] = 0xF7;
	tud_midi_stream_write(0, buf, o);
}

void WebUI::SendAck(uint8_t code, uint32_t value)
{
	uint8_t p[6] = { MSG_UP_ACK, code,
	                 static_cast<uint8_t>((value >> 14) & 0x7F),
	                 static_cast<uint8_t>((value >> 7)  & 0x7F),
	                 static_cast<uint8_t>( value        & 0x7F), 0 };
	Send(p, 5);
}

void WebUI::SendErr(uint8_t code)
{
	uint8_t p[2] = { MSG_UP_ERR, code };
	Send(p, 2);
	uploading_ = false;
}

void WebUI::FlushPage()
{
	if (pageFill_ == 0) return;
	// Pad the tail so a partial page still programs cleanly.
	memset(page_ + pageFill_, 0, 256 - pageFill_);
	ProgramPage(pageAddr_, page_);
	pageAddr_ += 256;
	pageFill_ = 0;
}

void WebUI::CommitHeader()
{
	FlushPage();
	hdr_.magic = kUserMagic;
	hdr_.version = kUserVersion;
	hdr_.totalBytes = writeOff_;

	// The header lives in its own sector, written last: until this lands the
	// magic is absent, so an interrupted upload falls back to baked samples
	// rather than pointing at half-written audio.
	static uint8_t sector[4096];
	memset(sector, 0xFF, sizeof(sector));
	memcpy(sector, &hdr_, sizeof(hdr_));
	EraseRegion(kUserRegionOff, 4096);
	for (uint32_t i = 0; i < 4096; i += 256)
		ProgramPage(kUserRegionOff + i, sector + i);
}

void WebUI::HandleSysex(const uint8_t *msg, uint32_t len)
{
	if (len < 2 || msg[0] != kManufacturerId) return;
	const uint8_t *p = msg + 1;
	uint32_t n = len - 1;

	switch (p[0])
	{
	case MSG_HELLO:
	{
		// Report version, capacity and whether user samples are loaded, so the
		// browser can show what is actually on the card.
		uint8_t info[8] = {
			MSG_INFO, kUserVersion,
			static_cast<uint8_t>(HaveUserSamples() ? 1 : 0),
			static_cast<uint8_t>(kHaveSamples ? 1 : 0),
			static_cast<uint8_t>((kUserDataLen >> 14) & 0x7F),
			static_cast<uint8_t>((kUserDataLen >> 7)  & 0x7F),
			static_cast<uint8_t>( kUserDataLen        & 0x7F),
			static_cast<uint8_t>(kNumModes) };
		Send(info, 8);
		break;
	}

	case MSG_UP_BEGIN:
	{
		// payload: 3 septets of total byte count
		if (n < 4) { SendErr(ERR_PROTOCOL); break; }
		expected_ = (static_cast<uint32_t>(p[1]) << 14)
		          | (static_cast<uint32_t>(p[2]) << 7)
		          |  static_cast<uint32_t>(p[3]);
		if (expected_ > kUserDataLen) { SendErr(ERR_TOO_BIG); break; }

		uploading_ = true;
		writeOff_ = 0;
		pageFill_ = 0;
		pageAddr_ = kUserDataOff;
		memset(&hdr_, 0, sizeof(hdr_));

		// Erase the whole data area up front: one long stall while muted,
		// rather than a stutter per sector during the transfer.
		EraseRegion(kUserDataOff, kUserDataLen);
		SendAck(0, 0);
		break;
	}

	case MSG_UP_SLOT:
	{
		if (!uploading_ || n < 3) { SendErr(ERR_PROTOCOL); break; }
		slotMode_ = p[1];
		slotVariant_ = p[2];
		if (slotMode_ >= kNumModes || slotVariant_ >= kNumVariants)
		{
			SendErr(ERR_BAD_SLOT);
			break;
		}
		// Slots start on a page boundary so each can be programmed independently.
		FlushPage();
		writeOff_ = pageAddr_ - kUserDataOff;
		hdr_.offset[slotMode_][slotVariant_] = writeOff_;
		hdr_.size[slotMode_][slotVariant_] = 0;
		SendAck(1, writeOff_);
		break;
	}

	case MSG_UP_CHUNK:
	{
		if (!uploading_) { SendErr(ERR_PROTOCOL); break; }
		uint8_t dec[512];
		uint32_t got = Decode7bit(p + 1, n - 1, dec, sizeof(dec));
		if (writeOff_ + got > kUserDataLen) { SendErr(ERR_TOO_BIG); break; }

		for (uint32_t i = 0; i < got; i++)
		{
			page_[pageFill_++] = dec[i];
			if (pageFill_ == 256)
			{
				ProgramPage(pageAddr_, page_);
				pageAddr_ += 256;
				pageFill_ = 0;
			}
		}
		writeOff_ += got;
		hdr_.size[slotMode_][slotVariant_] += got;
		SendAck(2, writeOff_);
		break;
	}

	case MSG_UP_SLOTEND:
		if (!uploading_) { SendErr(ERR_PROTOCOL); break; }
		SendAck(3, hdr_.size[slotMode_][slotVariant_]);
		break;

	case MSG_UP_END:
		if (!uploading_) { SendErr(ERR_PROTOCOL); break; }
		CommitHeader();
		uploading_ = false;
		SendAck(4, writeOff_);
		break;

#ifdef BIO_PROFILE
	// Only exists in profile builds, so the released firmware is byte-identical
	// to one built without the profiler at all. The browser treats no reply as
	// "this card has no profiler".
	case MSG_PROF_GET:
	{
		// Exact cycle counts beat asking someone to decode blinking LEDs.
		// Each value is 3 septets (21 bits), plenty for a 2604-cycle budget.
		uint8_t p2[2 + kNumProf * 3 + 3];
		uint32_t o = 0;
		p2[o++] = MSG_PROF;
		p2[o++] = static_cast<uint8_t>(kNumProf);
		for (int i = 0; i < kNumProf; i++)
		{
			uint32_t v = gProf[i].peak;
			p2[o++] = static_cast<uint8_t>((v >> 14) & 0x7F);
			p2[o++] = static_cast<uint8_t>((v >> 7)  & 0x7F);
			p2[o++] = static_cast<uint8_t>( v        & 0x7F);
		}
		uint32_t ov = gProf[0].overruns;
		p2[o++] = static_cast<uint8_t>((ov >> 14) & 0x7F);
		p2[o++] = static_cast<uint8_t>((ov >> 7)  & 0x7F);
		p2[o++] = static_cast<uint8_t>( ov        & 0x7F);
		Send(p2, o);
		ProfileReset();          // so each query reports a fresh window
		break;
	}
#endif

	case MSG_ERASE:
		// Wipe just the header sector: the audio stays but is unreferenced, so
		// the card falls straight back to its baked samples.
		uploading_ = true;      // mute for the erase
		EraseRegion(kUserRegionOff, 4096);
		uploading_ = false;
		SendAck(5, 0);
		break;

	default:
		break;
	}
}

} // namespace bio
