// samplestore.h — where the card gets its recordings from.
//
// Two sources, in priority order:
//
//   1. USER FLASH  — a 1MB region at a fixed address, written over USB by the
//                    web app (interface.html). Overrides per mode, so you can
//                    replace just the geese and keep everything else.
//   2. BAKED       — compiled into the firmware from samples/*.raw. The factory
//                    default, so a fresh card works with no upload.
//
// The user region sits at a FIXED offset rather than after the code, so
// reflashing the firmware does not move or wipe it.

#pragma once
#include <stdint.h>
#include "biomimicry.h"
#include "voices.h"          // kNumVariants
#include "samples_default.h" // baked fallback

namespace bio {

// --- Flash layout ----------------------------------------------------------
// RP2040 XIP window. The user region is the last 1MB of a 2MB part.
constexpr uint32_t kFlashBase     = 0x10000000u;
constexpr uint32_t kFlashSize     = 2u * 1024 * 1024;
constexpr uint32_t kUserRegionOff = 1u * 1024 * 1024;   // offset within flash
constexpr uint32_t kUserRegionLen = 1u * 1024 * 1024;

/// Directory written at the start of the user region. Fixed size so it can be
/// erased and rewritten as one 4KB sector.
struct UserSampleHeader
{
	uint32_t magic;       // kUserMagic when a valid upload is present
	uint32_t version;     // layout version, for future changes
	// Byte offsets are relative to the START of the user region's data area,
	// and a size of 0 means "this slot is empty, fall back to baked".
	uint32_t offset[kNumModes][kNumVariants];
	uint32_t size[kNumModes][kNumVariants];
	uint32_t totalBytes;
	uint32_t reserved[8];
};

constexpr uint32_t kUserMagic   = 0x42494F31u;   // "BIO1"
constexpr uint32_t kUserVersion = 1;

// The header occupies the first flash sector of the region; audio follows.
// RP2040 flash erases a sector at a time; every erase must be sector-aligned.
constexpr uint32_t kFlashSector   = 4096;

constexpr uint32_t kUserHeaderLen = 4096;
constexpr uint32_t kUserDataOff   = kUserRegionOff + kUserHeaderLen;
constexpr uint32_t kUserDataLen   = kUserRegionLen - kUserHeaderLen;

/// Read-only view of the user region as mapped through XIP.
static inline const UserSampleHeader *UserHeader()
{
	return reinterpret_cast<const UserSampleHeader *>(kFlashBase + kUserRegionOff);
}

static inline const int8_t *UserData()
{
	return reinterpret_cast<const int8_t *>(kFlashBase + kUserDataOff);
}

static inline bool HaveUserSamples()
{
	const UserSampleHeader *h = UserHeader();
	return h->magic == kUserMagic && h->version == kUserVersion;
}

/// Where a given mode+variant's audio actually lives, whichever source wins.
struct SampleRef
{
	const int8_t *data;
	uint32_t      len;
};

/// Resolve one slot. A user upload for that exact mode+variant wins; otherwise
/// the baked recording is used. This is what lets you replace one mode without
/// losing the rest.
static inline SampleRef ResolveSample(int mode, int variant)
{
	if (HaveUserSamples())
	{
		const UserSampleHeader *h = UserHeader();
		uint32_t sz = h->size[mode][variant];
		if (sz > 0 && h->offset[mode][variant] + sz <= kUserDataLen)
			return { UserData() + h->offset[mode][variant], sz };
	}

	if (kHaveSamples)
		return { kModeSample[mode] + kModeSampleOff[mode][variant],
		         kModeSampleSize[mode][variant] };

	return { nullptr, 0 };
}

/// How many round-robin variants a mode should actually use.
///
/// If ANY slot of a mode has been uploaded, only the uploaded ones count. Upload
/// two geese and you get two geese — not your two mixed with six of the card's
/// own, which is what a blind round robin over all eight slots would give you and
/// is never what someone replacing a mode wants.
///
/// Counts the leading run rather than the total, because the round robin picks an
/// index below the returned value: a gap would otherwise let it land on a slot
/// that was never filled. The web UI fills slot 1 upwards for this reason.
static inline int VariantCount(int mode)
{
	if (HaveUserSamples())
	{
		const UserSampleHeader *h = UserHeader();
		int n = 0;
		while (n < kNumVariants && h->size[mode][n] > 0) n++;
		if (n > 0) return n;
	}
	return kHaveSamples ? kNumVariants : 0;
}

/// True if there is anything at all to play — either source.
static inline bool AnySamples()
{
	return kHaveSamples || HaveUserSamples();
}

} // namespace bio
