// samples_default.h — optional baked-in PCM.
//
// samples.h is generated at build time by tools/mksamples.py + bin2h.py from
// samples/*.raw (8-bit signed mono, 48kHz). It is NOT committed: a fresh clone
// with no samples still compiles, and alt-boot simply falls back to the
// synthesized voices. Same __has_include trick WorkshopZX uses for snapshots.

#pragma once
#include <stdint.h>
#include "biomimicry.h"

namespace bio {

#if defined(__has_include)
#  if __has_include("samples.h")
#    define BIO_HAVE_SAMPLES 1
#  endif
#endif

#ifdef BIO_HAVE_SAMPLES

// samples.h defines, for each mode, an int8_t array + length:
//   horses_pcm[]/horses_pcm_len, geese_pcm[], frogs_pcm[], rain_pcm[], meteors_pcm[]
#include "samples.h"

constexpr bool kHaveSamples = true;

static const int8_t *const kModeSample[kNumModes] = {
	reinterpret_cast<const int8_t *>(horses_pcm),
	reinterpret_cast<const int8_t *>(geese_pcm),
	reinterpret_cast<const int8_t *>(frogs_pcm),
	reinterpret_cast<const int8_t *>(rain_pcm),
	reinterpret_cast<const int8_t *>(meteors_pcm),
	reinterpret_cast<const int8_t *>(cicadas_pcm)
};
static const uint32_t kModeSampleLen[kNumModes] = {
	horses_pcm_len, geese_pcm_len, frogs_pcm_len, rain_pcm_len,
	meteors_pcm_len, cicadas_pcm_len
};

#else

constexpr bool kHaveSamples = false;

// Stubs so voices.cpp compiles unchanged; usePcm_ is forced false when
// kHaveSamples is false, so these are never dereferenced.
static const int8_t *const kModeSample[kNumModes] = {
	nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};
static const uint32_t kModeSampleLen[kNumModes] = { 0, 0, 0, 0, 0, 0 };

#endif

} // namespace bio
