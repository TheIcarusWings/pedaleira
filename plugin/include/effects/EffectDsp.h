#pragma once
// Everything an effect needs in one include: the base class, the
// registration hook and the shared DSP helpers (namespace fx). See
// docs/effects.md.
#include "ParameterisedBlockProcessor.h"
#include "effects/EffectRegistration.h"
#include "effects/dsp/DelayLine.h"
#include "effects/dsp/EnvelopeFollower.h"
#include "effects/dsp/Filters.h"
#include "effects/dsp/Levels.h"
#include "effects/dsp/Lfo.h"
#include "effects/dsp/Noise.h"
#include "effects/dsp/Saturation.h"
#include "effects/dsp/Smoothing.h"
#include "effects/dsp/TempoSync.h"
