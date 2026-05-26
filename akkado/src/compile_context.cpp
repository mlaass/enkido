#include "akkado/compile_context.hpp"
#include "akkado/string_interner.hpp"
#include "akkado/voicing.hpp"

namespace akkado {

CompileContext::CompileContext()
    : voicing_registry(std::make_unique<voicing::VoicingRegistry>()),
      interner(std::make_unique<StringInterner>()) {}

CompileContext::~CompileContext() = default;

CompileContext::CompileContext(CompileContext&&) noexcept = default;
CompileContext& CompileContext::operator=(CompileContext&&) noexcept = default;

} // namespace akkado
