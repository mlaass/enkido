// SequenceCompiler — mini-notation AST -> cedar::Sequence/Event compilation.
// The class definition (with all method bodies, header-inline) lives in
// pattern_compiler.hpp so that patterns.cpp and pattern_transforms.cpp can
// instantiate it; this TU anchors the concern per prd-codegen-sprawl-cleanup
// Phase 3.

#include "pattern_compiler.hpp"
