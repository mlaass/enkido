#pragma once

// Master include for all opcode implementations
// Include this in vm.cpp to get all inline opcode functions

#include "dsp_state.hpp"
#include "dsp_utils.hpp"
#include "arithmetic.hpp"
#include "arrays.hpp"
#include "math.hpp"
#include "utility.hpp"
#include "edge_op.hpp"
#include "state_op.hpp"
#include "oscillators.hpp"
#include "filters.hpp"
#include "envelopes.hpp"
#include "samplers.hpp"
#ifndef CEDAR_NO_SOUNDFONT
#include "soundfont.hpp"
#endif
#include "delays.hpp"
#include "midi.hpp"
#include "sequencing.hpp"
#include "distortion.hpp"
#include "modulation.hpp"
#include "dynamics.hpp"
#include "reverbs.hpp"
#include "logic.hpp"
#include "stereo.hpp"
