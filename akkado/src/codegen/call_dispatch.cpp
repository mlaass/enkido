// Call-branch emitters — per-shape emission paths for builtin calls.
// Split out of visit() per prd-codegen-sprawl-cleanup Phase 4.

#include "akkado/codegen.hpp"
#include "akkado/named_args.hpp"
#include "akkado/codegen/codegen.hpp"  // Master include for all codegen helpers
#include "akkado/codegen/instruction_builder.hpp"
#include "akkado/codegen/state_init_builder.hpp"
#include "akkado/builtins.hpp"
#include "akkado/overload.hpp"
#include "akkado/compile_context.hpp"
#ifdef CEDAR_HOST_EXTENSIONS
#include "akkado/host_extensions.hpp"
#endif
#include <algorithm>
#include <cctype>
#include <optional>

namespace akkado {

// Use helpers from akkado::codegen namespace
using codegen::is_upgradeable_oscillator;
using codegen::upgrade_for_fm;
using codegen::SamplePatternEmitCtx;
using codegen::emit_sample_chain;

// ============================================================================
// Dispatch-ladder steps (compact optional-returning probes)
// ============================================================================

bool CodeGenerator::collect_spread_args(NodeIndex node,
                                        std::vector<ExpandedArg>& expanded_args,
                                        bool& did_spread_swap) {
    NodeIndex it = ast_->arena[node].first_child;
    bool has_spread = false;
    while (it != NULL_NODE) {
        const Node& a = ast_->arena[it];
        if (a.type == NodeType::Argument &&
            a.extra_child(0) != NULL_NODE) {
            has_spread = true;
            break;
        }
        it = ast_->arena[it].next_sibling;
    }
    if (has_spread) {
        auto expanded_opt = expand_call_arguments(node);
        if (!expanded_opt) return false;
        expanded_args = std::move(*expanded_opt);
        did_spread_swap = true;
    }
    return true;
}

std::optional<TypedValue> CodeGenerator::try_operator_dispatch(
    NodeIndex node, const Node& n, const std::string& func_name) {
    if (const auto* op_overloads = lookup_operator_overloads(func_name)) {
        const DispatchPattern& op_pat = (*op_overloads)[0];
        if (op_pat.target.kind == DispatchTarget::Kind::LegacyHandler &&
            op_pat.target.legacy_handler ==
                LegacyHandlerId::BinaryOpBroadcast) {
            TypedValue tv = handle_binary_op_call(node, n);
            if (node_types_.find(node) == node_types_.end()) {
                node_types_[node] = tv;
            }
            return tv;
        }
        // Builtin-target operator (comparison/logical): fall through to
        // the generic builtin emission.
    }
    return std::nullopt;
}

std::optional<TypedValue> CodeGenerator::try_builtin_overload_dispatch(
    NodeIndex node, const Node& n, const std::string& func_name) {
    const auto* bov = lookup_builtin_overloads(func_name);
    if (!bov) return std::nullopt;

    if (bov->size() == 1) {
        // Single-pattern family (pan/pingpong/smooch, delay*). No
        // resolve()/arg pre-visit: the dispatch dimension is channel
        // width / arg count (orthogonal to the type model, PRD §3)
        // and stays inside the LegacyHandler, which re-visits args
        // itself. Mirrors the Phase-2 operator path — avoids a
        // double-visit.
        const DispatchTarget& tgt = bov->front().target;
        if (tgt.kind == DispatchTarget::Kind::LegacyHandler) {
            CodegenHandler h = nullptr;
            switch (tgt.legacy_handler) {
                case LegacyHandlerId::Pan:
                    h = &CodeGenerator::handle_pan_call; break;
                case LegacyHandlerId::Pingpong:
                    h = &CodeGenerator::handle_pingpong_call; break;
                case LegacyHandlerId::Smooch:
                    h = &CodeGenerator::handle_smooch_call; break;
                // Phase 5: heavy pattern / higher-order handlers.
                case LegacyHandlerId::Poly:
                    h = &CodeGenerator::handle_poly_call; break;
                case LegacyHandlerId::Mono:
                    h = &CodeGenerator::handle_mono_call; break;
                case LegacyHandlerId::Each:
                    h = &CodeGenerator::handle_each_call; break;
                case LegacyHandlerId::EachVoice:
                    h = &CodeGenerator::handle_each_voice_call; break;
                case LegacyHandlerId::Transport:
                    h = &CodeGenerator::handle_transport_call; break;
                case LegacyHandlerId::Midi:
                    h = &CodeGenerator::handle_midi_call; break;
                default: break;
            }
            if (h) {
                TypedValue tv = (this->*h)(node, n);
                if (node_types_.find(node) == node_types_.end()) {
                    node_types_[node] = tv;
                }
                return tv;
            }
        }
        // Builtin-target single pattern (delay*): fall through to the
        // generic emission below (time unit from inst_rate).
    } else {
        // Multi-pattern family (sample/sample_loop): the first family
        // to drive a live resolve() type dispatch. The id slot
        // (index 2) selects the String-name form vs the numeric/
        // Signal id form. We type the id from its *literal* AST node
        // so the gate needs no visit (no double-emit); a non-literal
        // id is left ungated and handled by the generic path as
        // before. A literal id that is neither String nor coercible
        // to Signal (e.g. a record/array literal) → E424.
        std::vector<NodeIndex> arg_values;
        for (NodeIndex it_arg = n.first_child; it_arg != NULL_NODE;
             it_arg = ast_->arena[it_arg].next_sibling) {
            const Node& an = ast_->arena[it_arg];
            arg_values.push_back(
                an.type == NodeType::Argument ? an.first_child : it_arg);
        }
        const std::size_t req = bov->front().required_count;  // 3
        if (arg_values.size() == req && arg_values[req - 1] != NULL_NODE) {
            std::optional<ValueType> id_type;
            switch (ast_->arena[arg_values[req - 1]].type) {
                case NodeType::StringLit: id_type = ValueType::String; break;
                case NodeType::NumberLit: id_type = ValueType::Number; break;
                case NodeType::RecordLit: id_type = ValueType::Record; break;
                case NodeType::ArrayLit:  id_type = ValueType::Array;  break;
                default: break;  // non-literal → leave ungated
            }
            if (id_type) {
                std::vector<ArgDescriptor> ads(arg_values.size());
                for (auto& ad : ads) ad.skip = true;  // gate id only
                ads[req - 1].skip = false;
                ads[req - 1].type = *id_type;
                if (!resolve(*bov, ads).matched) {
                    error("E424",
                          func_name + "() has no overload matching an "
                          "id of type " + value_type_name(*id_type) +
                          "; expected a sample name (String) or a "
                          "numeric id (Number/Signal)",
                          ast_->arena[arg_values[req - 1]].location);
                    if (node_types_.find(node) == node_types_.end()) {
                        node_types_[node] = TypedValue::error_val();
                    }
                    return TypedValue::error_val();
                }
            }
        }
        // Matched / ungated: fall through to the generic SAMPLE_PLAY
        // emission, which performs the String-name parse and numeric
        // id handling as today.
    }
    return std::nullopt;
}

// ============================================================================
// Generic builtin emission (step 10 of the Call dispatch ladder)
// ============================================================================

TypedValue CodeGenerator::emit_builtin_call(NodeIndex node, const Node& n,
                                            const std::string& func_name,
                                            SourceLocation call_loc,
                                            std::vector<ExpandedArg>& expanded_args,
                                            bool did_spread_swap) {
    const BuiltinInfo* builtin = lookup_builtin(func_name);

    if (!builtin) {
        error("E107", "Unknown function: '" + func_name + "'", n.location);
        return TypedValue::error_val();
    }

    // Phase-1 overload resolution (PRD prd-builtin-overload-resolution):
    // each builtin is one synthesized DispatchPattern mirroring its
    // param_types/args_are_signal. The per-arg type check below resolves
    // against this pattern via matches_arg(), replacing the old inline
    // param_types ladder with the single declarative model.
    const DispatchPattern call_pattern = make_builtin_pattern(*builtin);

    // Spread-expanded builtin calls reach codegen with named Argument
    // nodes still in the chain — the analyzer defers reordering for
    // spread calls (a `..record` source is only known after value
    // evaluation). Map those field names onto the builtin's parameter
    // slots now, so `chorus(@, .., ..{dry: 1, wet: 0.5})` binds dry/wet
    // (incl. extended-param slots) by name instead of being consumed
    // positionally and silently misbinding.
    if (did_spread_swap &&
        !reorder_spread_named_args(*builtin, func_name, expanded_args,
                                   n.location)) {
        return TypedValue::error_val();
    }

    // Materialise the (possibly reordered) ExpandedArg vector into
    // CallSlots for the per-arg loop. Side-table entries are keyed
    // by (call_node, slot_index); the buffer was already allocated
    // by the spread source's earlier visit.
    std::optional<std::vector<CallSlot>> spread_call_slots;
    if (did_spread_swap) {
        std::vector<CallSlot> slots;
        slots.reserve(expanded_args.size());
        for (std::size_t i = 0; i < expanded_args.size(); ++i) {
            auto& ea = expanded_args[i];
            CallSlot s;
            s.loc = ea.loc;
            if (ea.is_underscore) {
                s.kind = CallSlot::Kind::Underscore;
                s.node = NULL_NODE;
            } else if (ea.resolved.has_value()) {
                s.kind = CallSlot::Kind::Resolved;
                s.node = NULL_NODE;
                pre_resolved_values_[{node, i}] = *ea.resolved;
            } else {
                s.kind = CallSlot::Kind::AstNode;
                // Unwrap Argument if present so downstream consumers
                // see the value node directly (matches chain-mode).
                NodeIndex av = ea.source_node;
                if (av != NULL_NODE) {
                    const Node& sn = ast_->arena[av];
                    if (sn.type == NodeType::Argument) {
                        av = sn.first_child;
                    }
                }
                s.node = av;
            }
            slots.push_back(std::move(s));
        }
        spread_call_slots = std::move(slots);
    }

    // For stateful functions, push path BEFORE visiting children
    // so nested calls see their parent's context
    bool pushed_path = false;
    if (builtin->requires_state) {
        std::uint32_t count = call_counters_[func_name]++;
        std::string unique_name = func_name + "#" + std::to_string(count);
        push_path(unique_name);
        pushed_path = true;
    }

    // Visit arguments (dependencies must be satisfied).
    //
    // Iteration source: when spread expansion ran, `spread_call_slots`
    // holds the canonical slot-ordered argument list (Resolved,
    // Underscore, or AstNode). Otherwise we synthesise an equivalent
    // CallSlot vector by walking `n.first_child` once — keeping the
    // rest of the loop body single-shape.
    std::vector<std::uint16_t> arg_buffers;
#ifdef CEDAR_HOST_EXTENSIONS
    // Upstream event source for an accepts_events host node (0 = none).
    std::uint32_t host_seq_state_id = 0;
#endif
    std::vector<CallSlot> chain_slots;
    const std::vector<CallSlot>* effective_slots = nullptr;
    if (spread_call_slots.has_value()) {
        effective_slots = &*spread_call_slots;
    } else {
        for (NodeIndex it_arg = n.first_child; it_arg != NULL_NODE;
             it_arg = ast_->arena[it_arg].next_sibling) {
            NodeIndex arg_value = it_arg;
            const Node& arg_node = ast_->arena[it_arg];
            if (arg_node.type == NodeType::Argument) {
                arg_value = arg_node.first_child;
            }
            const Node& val_node = ast_->arena[arg_value];
            bool is_placeholder =
                (val_node.type == NodeType::Identifier &&
                 std::holds_alternative<Node::IdentifierData>(val_node.data) &&
                 ctx_->interner->view(val_node.as_identifier()) == "_");
            CallSlot s;
            s.loc = val_node.location;
            if (is_placeholder) {
                s.kind = CallSlot::Kind::Underscore;
                s.node = NULL_NODE;
            } else {
                s.kind = CallSlot::Kind::AstNode;
                s.node = arg_value;
            }
            chain_slots.push_back(std::move(s));
        }
        effective_slots = &chain_slots;
    }

    for (std::size_t arg_idx = 0; arg_idx < effective_slots->size();
         ++arg_idx) {
        const CallSlot& slot = (*effective_slots)[arg_idx];

        if (slot.kind == CallSlot::Kind::Underscore) {
            if (builtin->has_default(arg_idx)) {
                const std::uint16_t default_buf =
                    codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                        .const_value(builtin->get_default(arg_idx))
                        .emit(*this, n.location);
                if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                    if (pushed_path) pop_path();
                    return TypedValue::error_val();
                }
                arg_buffers.push_back(default_buf);
            } else {
                error("E106", "Cannot skip required parameter '" +
                      std::string(builtin->param_names[arg_idx]) +
                      "' — no default value", slot.loc);
                arg_buffers.push_back(0);
            }
            continue;
        }

        TypedValue arg_tv;
        SourceLocation diag_loc;

        if (slot.kind == CallSlot::Kind::Resolved) {
            auto pr_it = pre_resolved_values_.find({node, arg_idx});
            arg_tv = (pr_it != pre_resolved_values_.end())
                         ? pr_it->second
                         : TypedValue::error_val();
            diag_loc = slot.loc;
        } else {
            // CallSlot::Kind::AstNode
            NodeIndex arg_value = slot.node;
            arg_tv = visit(arg_value);
            diag_loc = ast_->arena[arg_value].location;

            // sample("name"[, ...]) form: parse the sample-name string,
            // register it for runtime loading, and emit a PUSH_CONST
            // placeholder whose state_id immediate is patched to the
            // bank-assigned sample ID after samples are loaded. The
            // numeric `sample(t, p, <int>)` form falls through.
            const Node& val_node = ast_->arena[arg_value];
            if (func_name == "sample" && arg_idx == 2 &&
                arg_tv.type == ValueType::String &&
                val_node.type == NodeType::StringLit) {
                const std::string& raw = val_node.as_string();
                std::string bank, after_bank, name;
                int variant = 0;
                auto slash = raw.find('/');
                if (slash != std::string::npos) {
                    bank = raw.substr(0, slash);
                    after_bank = raw.substr(slash + 1);
                } else {
                    after_bank = raw;
                }
                auto colon = after_bank.rfind(':');
                if (colon != std::string::npos) {
                    std::string vstr = after_bank.substr(colon + 1);
                    bool all_digits = !vstr.empty() &&
                        std::all_of(vstr.begin(), vstr.end(),
                            [](char c){ return std::isdigit(static_cast<unsigned char>(c)); });
                    if (all_digits) {
                        variant = std::stoi(vstr);
                        name = after_bank.substr(0, colon);
                    } else {
                        name = after_bank;
                    }
                } else {
                    name = after_bank;
                }

                if (name.empty()) {
                    error("E161", "sample() name string is empty", val_node.location);
                } else {
                    publish_sample_refs({RequiredSample{bank, name, variant}});

                    const std::uint32_t inst_idx =
                        static_cast<std::uint32_t>(instructions_.size());
                    const std::uint16_t buf =
                        codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                            .const_value(0.0f)
                            .emit(*this, val_node.location);
                    if (buf != BufferAllocator::BUFFER_UNUSED) {
                        scalar_sample_mappings_.push_back(
                            ScalarSampleMapping{inst_idx, bank, name, variant});
                        arg_tv = TypedValue::signal(buf);
                    }
                }
            }
        }

#ifdef CEDAR_HOST_EXTENSIONS
        // Event-input host nodes (hosting PRD §4.1): the pattern's
        // EVENT STREAM is the input, not its scalar projection. Leave
        // the slot unwired and record the upstream sequencer's state
        // id for the manifest — the host resolves the block's events
        // itself via StatePool::resolve_output_events. This also
        // bypasses the E160 poly-coerce reject below: chords are
        // exactly what the event input carries.
        if (arg_tv.type == ValueType::Pattern && arg_tv.pattern &&
            builtin->opcode == cedar::Opcode::HOST_OP &&
            host_node_accepts_events(func_name)) {
            if (host_seq_state_id != 0 &&
                host_seq_state_id != arg_tv.pattern->state_id) {
                error("E264", func_name +
                      "() accepts at most one event input per call",
                      diag_loc);
            }
            host_seq_state_id = arg_tv.pattern->state_id;
            arg_buffers.push_back(BufferAllocator::BUFFER_UNUSED);
            continue;
        }
#endif

        // PRD prd-patterns-as-scalar-values §5.3: implicit
        // Pattern→Signal coerce. arg_tv.buffer is already the
        // pattern's primary value buffer — for monophonic non-
        // sample patterns this is the FREQ buffer (Signal-typed
        // since SEQPAT_STEP populates raw scalars); for sample
        // patterns it is the post-SAMPLE_PLAY audio output
        // (already a Signal), which legitimately routes through
        // out(), gain stages, and effects.
        //
        // The genuine footgun is a polyphonic non-sample pattern
        // (chord, multi-voice note pattern): without a coerce
        // reject, `sine(c"Am")` would silently emit only
        // voice-0's freq, dropping the chord's other voices.
        // Reject those at the slot with E160 so the user opts
        // into poly() / scalar() / a voice index explicitly.
        bool slot_expects_signal =
            arg_idx < MAX_BUILTIN_PARAMS &&
            (builtin->param_types[arg_idx] == ParamValueType::Signal ||
             builtin->param_types[arg_idx] == ParamValueType::Any);
        if (builtin->args_are_signal && slot_expects_signal &&
            arg_tv.type == ValueType::Pattern && arg_tv.pattern &&
            arg_tv.pattern->max_voices > 1 &&
            !arg_tv.pattern->is_sample_pattern) {
            error("E160",
                  func_name + "() cannot use a polyphonic pattern as scalar at "
                  "argument '" + std::string(builtin->param_names[arg_idx]) +
                  "'; use poly() to consume it, or pick a voice/field "
                  "explicitly (e.g. p.freq)",
                  diag_loc);
        }

        // PRD prd-pattern-event-arrays §4.5/§5.6: a DynArray
        // (notes(e)/freqs(e)) has a runtime-varying length, so it
        // cannot auto-fan-out across a builtin's fixed arity the
        // way a static array does. Reject it with a directive
        // pointing at poly() for runtime polyphony.
        if (arg_tv.type == ValueType::DynArray) {
            error("E181",
                  func_name + "() cannot auto-expand over a dynamic "
                  "array (chord size varies per pattern event). "
                  "Wrap with poly() for runtime polyphony:\n"
                  "  e |> poly(@, (f, g, v) -> osc(\"sin\", f) * "
                  "ar(g, 0.01, 0.3) * v)",
                  diag_loc);
        }

        // A Void expression (e.g. piping onward from out(), which
        // returns no value) carries no buffer — wiring its 0xFFFF
        // sentinel into the instruction would make the VM read out
        // of bounds. Coerce to silence (BUFFER_ZERO) and warn
        // ([[feedback_livecoding_coerce_dont_fail]]).
        if (!arg_tv.error && arg_tv.type == ValueType::Void &&
            arg_tv.buffer == 0xFFFF) {
            warn("W161",
                 func_name + "() argument '" +
                 (arg_idx < MAX_BUILTIN_PARAMS
                      ? std::string(builtin->param_names[arg_idx])
                      : std::to_string(arg_idx)) +
                 "' has no value (void expression — e.g. piping "
                 "onward from out()) — coerced to silence",
                 diag_loc);
            arg_tv.buffer = cedar::BUFFER_ZERO;
        }

        arg_buffers.push_back(arg_tv.buffer);

        // Type check against annotation (non-fatal — continue for max error reporting).
        // DynArray already reported the dedicated E181 above; skip
        // the generic type-mismatch to avoid a duplicate diagnostic.
        //
        // Two checking modes (PRD prd-compiler-type-system Phase 4):
        //
        //  - EXPLICIT annotation (param_types[i] != Any): strict — the
        //    builtin author opted into a precise type. `type_compatible`
        //    decides. out/bus reject Array, visualizers want a String
        //    label, etc.
        //
        //  - IMPLICIT Signal slot (no annotation, builtin coerces all
        //    args to Signal via `args_are_signal`): coerce-friendly per
        //    the live-coding philosophy ([[feedback_livecoding_coerce_
        //    dont_fail]]). Number/Pattern (voice-0 coerce), Array
        //    (map-over-array expansion, `triad.saw().sum()`), Record
        //    (event-as-scalar → .freq) and String (e.g. sample("bd")
        //    ids) all have a defensible interpretation, so they pass.
        //    Only Function and StateCell have no audio meaning at a
        //    signal slot — those are the genuine no-coercion-path
        //    mistakes, and the only types this implicit path rejects.
        if (arg_idx < MAX_BUILTIN_PARAMS &&
            !arg_tv.error && arg_tv.type != ValueType::Void &&
            arg_tv.type != ValueType::DynArray) {
            const ArgMatcher& matcher = call_pattern.params[arg_idx];
            ArgDescriptor ad;
            ad.type = arg_tv.type;
            if (!matches_arg(matcher, ad)) {
                if (matcher.kind == ArgMatcher::Kind::Type) {
                    // EXPLICIT annotation mismatch.
                    error("E160", func_name + "() argument '" +
                          std::string(builtin->param_names[arg_idx]) +
                          "' expects " + param_value_type_name(matcher.type) +
                          ", got " + value_type_name(arg_tv.type),
                          diag_loc);
                } else {
                    // IMPLICIT signal-coerce slot rejected Function/StateCell.
                    error("E160", func_name + "() argument '" +
                          std::string(builtin->param_names[arg_idx]) +
                          "' expects a signal, got " +
                          value_type_name(arg_tv.type) +
                          " — no coercion path",
                          diag_loc);
                }
            }
        }
    }

    // PRD §4.4 / §5.3 rule 2: out(L, R) expects two Mono signals.
    // Any Stereo in either slot is a compile error — tell the user
    // exactly how to fix it (wrap the stereo in out(sig) or split
    // with left()/right()).
    //
    // Only flag when the argument's resolved TypedValue is stereo
    // (channels == Stereo with a valid right_buffer). Checking
    // `stereo_buffer_pairs_` would false-positive on `left(s)` /
    // `right(s)` extractions that legitimately reuse the pair's
    // left/right buffer as a mono signal.
    if (func_name == "out" && arg_buffers.size() == 2) {
        // PRD prd-stereo-native-opcodes §5.6, §9.4: out() argument
        // shape mismatch is no longer E185 — it auto-escalates.
        // For each arg, we route a (L, R) contribution to the
        // accumulating OUTPUT bus:
        //   - Mono arg  → broadcast (L = R = arg)
        //   - Stereo arg → split (L = arg.left, R = arg.right)
        // The two contributions sum at the bus since OUTPUT does
        // `output_left += L; output_right += R;` (utility.hpp:55-56).
        //
        // Detect a mismatch first so we can avoid the per-arg
        // emission path when both args are mono (the existing
        // single-OUTPUT emission below is unchanged for that case).
        bool any_stereo = false;
        std::array<TypedValue, 2> resolved{};
        {
            NodeIndex ch = n.first_child;
            for (std::size_t ai = 0; ai < 2 && ch != NULL_NODE; ++ai,
                     ch = ast_->arena[ch].next_sibling) {
                const Node& arg_node = ast_->arena[ch];
                NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                     arg_node.first_child : ch;
                resolved[ai] = visit(arg_value);
                if (resolved[ai].is_stereo() &&
                    resolved[ai].right_buffer != 0xFFFF) {
                    any_stereo = true;
                }
            }
        }
        if (any_stereo) {
            warn("W185",
                 "out() with mixed channel shapes — auto-escalating: "
                 "mono args broadcast to both buses, stereo args drive "
                 "both buses; contributions sum.",
                 n.location);
            for (std::size_t ai = 0; ai < 2; ++ai) {
                std::uint16_t left_in  = arg_buffers[ai];
                std::uint16_t right_in = arg_buffers[ai];
                if (resolved[ai].is_stereo() &&
                    resolved[ai].right_buffer != 0xFFFF) {
                    left_in  = resolved[ai].buffer;
                    right_in = resolved[ai].right_buffer;
                }
                codegen::InstructionBuilder(cedar::Opcode::OUTPUT)
                    .inputs({left_in, right_in})
                    .output(0xFFFF)
                    .emit(*this);
            }
            if (pushed_path) pop_path();
            return cache_and_return(node, TypedValue::void_val());
        }
    }

    // Special case: out() with single argument
    // Check if the argument is stereo - if so, use both channels
    if (func_name == "out" && arg_buffers.size() == 1) {
        // Get the first argument node to check if it's stereo
        NodeIndex first_arg = n.first_child;
        if (first_arg != NULL_NODE) {
            const Node& arg_node = ast_->arena[first_arg];
            NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                 arg_node.first_child : first_arg;

            // Check stereo by both node and buffer (buffer fallback for pipe chains)
            bool arg_is_stereo = is_stereo(arg_value) || is_stereo_buffer(arg_buffers[0]);

            if (arg_is_stereo) {
                // Stereo input - use both channels
                StereoBuffers stereo;
                if (is_stereo(arg_value)) {
                    stereo = get_stereo_buffers(arg_value);
                } else {
                    stereo = get_stereo_buffers_by_buffer(arg_buffers[0]);
                }
                arg_buffers[0] = stereo.left;
                arg_buffers.push_back(stereo.right);
            } else {
                // Mono input - duplicate to both channels
                arg_buffers.push_back(arg_buffers[0]);
            }
        } else {
            // No argument node? Just duplicate
            arg_buffers.push_back(arg_buffers[0]);
        }
    }

    // PRD §5.3 rule 1 / §5.2 (G1): declarative channel-type mismatch.
    // For builtins that are not stereo_native, a stereo signal in a
    // Mono slot (or vice versa) is a compile error E186. Bespoke-handler
    // builtins (stereo/mono/left/right/width/ms_encode/ms_decode via the
    // codegen_handler dispatch; pan/pingpong via the builtin overload table)
    // never reach this path — they enforce their own signatures with
    // E181–E184. `out()` is handled above via E185 and the single-arg
    // expansion branch.
    //
    // Stereo-native opcodes (prd-stereo-native-opcodes) skip this check
    // because they auto-escalate mono → stereo at the boundary (mono
    // input is broadcast inside the opcode body). As of Phase 5 every
    // audio-signal opcode is stereo-native; auto-lift is retired.
    //
    // Gated on !did_spread_swap: with spread expansion the original
    // first_child chain may contain a spread Argument whose
    // first_child is NULL_NODE (the spread source lives in
    // extra_children[0], not as a child) — re-visiting that would
    // segfault. Pre-Phase-1a the synthesized chain didn't have this
    // shape; we preserve byte-identical behaviour by skipping the
    // re-walk. Per-arg type checks for the spread case already ran
    // inside the per-arg loop using the cached TypedValues.
    if (func_name != "out" && !builtin->stereo_native && !did_spread_swap) {
        NodeIndex ch = n.first_child;
        for (std::size_t ai = 0; ai < arg_buffers.size() &&
                ai < MAX_BUILTIN_PARAMS && ch != NULL_NODE; ++ai,
                ch = ast_->arena[ch].next_sibling) {
            const Node& arg_node = ast_->arena[ch];
            NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                 arg_node.first_child : ch;
            // `_` placeholders (user-written or synthesized by the
            // analyzer's named-arg gap fill) were default-filled in the
            // arg loop and have no channel shape to check — re-visiting
            // one as an identifier would be a spurious E102.
            const Node& val_node = ast_->arena[arg_value];
            if (val_node.type == NodeType::Identifier &&
                std::holds_alternative<Node::IdentifierData>(val_node.data) &&
                ctx_->interner->view(val_node.as_identifier()) == "_") {
                continue;
            }
            TypedValue resolved = visit(arg_value);
            if (resolved.type != ValueType::Signal) continue;
            ChannelCount actual = (resolved.is_stereo() &&
                                  resolved.right_buffer != 0xFFFF)
                                  ? ChannelCount::Stereo
                                  : ChannelCount::Mono;
            ChannelCount expected = builtin->input_channels[ai];
            if (actual != expected) {
                std::string param_label;
                if (!builtin->param_names[ai].empty()) {
                    param_label = " '" + std::string(builtin->param_names[ai]) + "'";
                }
                std::string hint = (expected == ChannelCount::Mono)
                    ? " Use `left(x)`, `right(x)`, or `mono(x)` to reduce to mono."
                    : " Use `stereo(x)` to promote to stereo.";
                error("E186",
                      "'" + std::string(func_name) +
                      "' expects " + channel_count_name(expected) +
                      " for argument" + param_label +
                      " (position " + std::to_string(ai + 1) + "), got " +
                      channel_count_name(actual) + "." + hint,
                      ast_->arena[ch].location);
                if (pushed_path) pop_path();
                return TypedValue::error_val();
            }
        }
    }

    // Multi-buffer argument detection (stereo or chord expansion).
    // Stereo input is consumed by the stereo-native emission path
    // below (single dispatch, STEREO_INPUT flag). Chord expansion-to-N
    // applies to stateful UGens (per-voice state).
    //
    // Gated on !did_spread_swap: a spread-expanded call has already
    // unpacked its array/record source into individual per-slot
    // values via expand_call_arguments. Re-running multi-buffer
    // fan-out here would double-expand. Pre-Phase-1a, the synthesized
    // PreResolved children weren't multi-buffer sources so the loop
    // silently no-op'd; post-Phase-1a we read the original chain,
    // whose spread-source array IS multi-buffer, so we must skip
    // explicitly. See prd-parser-codegen-correctness.md §4 Phase 1a.
    int expansion_arg_idx = -1;
    std::vector<std::uint16_t> expansion_buffers;
    std::vector<NodeIndex> arg_nodes;

    if (!arg_buffers.empty() && !did_spread_swap) {
        NodeIndex arg_iter = n.first_child;
        while (arg_iter != NULL_NODE) {
            const Node& arg_node = ast_->arena[arg_iter];
            NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                 arg_node.first_child : arg_iter;
            arg_nodes.push_back(arg_value);
            arg_iter = ast_->arena[arg_iter].next_sibling;
        }

        for (std::size_t i = 0; i < arg_nodes.size(); ++i) {
            if (is_multi_buffer(arg_nodes[i])) {
                expansion_arg_idx = static_cast<int>(i);
                expansion_buffers = get_multi_buffers(arg_nodes[i]);
                break;
            }
            const Node& arg_n = ast_->arena[arg_nodes[i]];
            if (arg_n.type == NodeType::Identifier) {
                std::string name = std::string(ctx_->interner->view(arg_n.as_identifier()));
                std::uint32_t param_hash = fnv1a_hash(name);
                auto pit = param_multi_buffer_sources_.find(param_hash);
                if (pit != param_multi_buffer_sources_.end()) {
                    NodeIndex source_node = pit->second;
                    if (is_multi_buffer(source_node)) {
                        expansion_arg_idx = static_cast<int>(i);
                        expansion_buffers = get_multi_buffers(source_node);
                        break;
                    }
                }
            }
        }

    }

    BuiltinEmitCtx ectx{node, n, func_name, *builtin, call_loc, pushed_path,
                        arg_buffers, expansion_arg_idx, expansion_buffers,
                        arg_nodes,
#ifdef CEDAR_HOST_EXTENSIONS
                        host_seq_state_id
#else
                        0
#endif
    };

    // PRD prd-stereo-native-opcodes Phase 1: declarative stereo-native
    // emission. Builtins that opt in produce a stereo output pair in a
    // single dispatch, regardless of input channel count (mono auto-
    // escalates to L=R inside the opcode body). Stereo primary input
    // adds STEREO_INPUT to the flag combo so the opcode reads
    // inputs[0]+1 for R. Chord/polyphonic expansion onto a stereo-
    // native opcode is rejected — user must wrap explicitly in poly().
    // Array expansion on a non-signal (control) slot of a stereo-native
    // opcode — e.g. `lp(sig, [500, 1000, 2000])` — falls through to
    // the chord/array expansion loop below. The loop emits N stereo-
    // native instances (one per expanded element); see
    // prd-stereo-native-opcodes §9.10 (pattern/array events on control
    // slots are unaffected by stereo-native processing).
    const bool stereo_native_control_expansion =
        builtin->stereo_native &&
        expansion_arg_idx > 0 &&
        expansion_buffers.size() > 1;

    if (builtin->stereo_native && !stereo_native_control_expansion) {
        return emit_stereo_native_call(ectx);
    }

    // Chord expansion to N instances (stateful UGens only — per-voice state).
    // For stereo-native builtins, each instance allocates an adjacent L/R
    // output pair and sets STEREO_OUTPUT (prd-stereo-native-opcodes §9.10).
    if (builtin->requires_state && expansion_arg_idx >= 0 && expansion_buffers.size() > 1) {
        return emit_chord_expanded_call(ectx);
    }

    return emit_generic_builtin_tail(ectx);
}

// ============================================================================
// Per-shape emission paths
// ============================================================================

TypedValue CodeGenerator::emit_stereo_native_call(const BuiltinEmitCtx& c) {
    const NodeIndex node = c.node;
    const Node& n = c.n;
    const std::string& func_name = c.func_name;
    const BuiltinInfo* builtin = &c.builtin;
    const SourceLocation call_loc = c.call_loc;
    const bool pushed_path = c.pushed_path;
    const std::vector<std::uint16_t>& arg_buffers = c.arg_buffers;
    const int expansion_arg_idx = c.expansion_arg_idx;
    const std::vector<std::uint16_t>& expansion_buffers = c.expansion_buffers;
    const std::vector<NodeIndex>& arg_nodes = c.arg_nodes;

    bool primary_stereo =
        expansion_arg_idx == 0 &&
        expansion_buffers.size() == 2 &&
        is_stereo(arg_nodes[0]);

    // Output width: Stereo declares "always emit a pair";
    // Match declares "follow the primary signal input" — when
    // the primary is mono, emit a single buffer so the result
    // slots into downstream mono parameter slots without E186.
    bool emit_stereo;
    switch (builtin->output_channels) {
        case ChannelCount::Match:
            emit_stereo = primary_stereo;
            break;
        case ChannelCount::Mono:
            emit_stereo = false;
            break;
        case ChannelCount::Stereo:
        default:
            emit_stereo = true;
            break;
    }

    // Chord/poly expansion onto the PRIMARY SIGNAL slot of a
    // stereo-native opcode = E187. Each voice would carry its own
    // stereo pair, ambiguous without poly(). Stereo input (size==2
    // + is_stereo on slot 0) is fine; >2 voices or 2 non-stereo
    // voices indicate a chord that needs poly().
    bool is_chord_expansion_on_signal =
        expansion_arg_idx == 0 &&
        expansion_buffers.size() > 1 &&
        !primary_stereo;
    if (is_chord_expansion_on_signal) {
        error("E187",
              "Chord/polyphonic expansion into stereo-native opcode '" +
              std::string(func_name) +
              "' requires explicit poly() wrapping.",
              call_loc);
        if (pushed_path) pop_path();
        return TypedValue::error_val();
    }

    current_source_loc_ = call_loc;

    std::size_t n_params = builtin->total_params();
    auto expanded_args = arg_buffers;

    // For stereo primary input: ensure the primary signal arg
    // points at the LEFT buffer of the stereo pair — opcode reads
    // inputs[0]+1 for R when STEREO_INPUT is set.
    if (primary_stereo) {
        expanded_args[static_cast<std::size_t>(expansion_arg_idx)] =
            expansion_buffers[0];
    }

    // Fill in any remaining defaults with control-rate constants
    for (std::size_t j = expanded_args.size(); j < n_params; ++j) {
        if (builtin->has_default(j)) {
            const std::uint16_t default_buf =
                codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                    .const_value(builtin->get_default(j))
                    .emit(*this, n.location);
            if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                if (pushed_path) pop_path();
                return TypedValue::error_val();
            }
            expanded_args.push_back(default_buf);
        }
    }

    // Allocate output buffer(s): adjacent L/R pair when emitting
    // stereo, otherwise a single buffer (Match+mono input path).
    std::uint16_t out_left = alloc_buffer(n.location);
    if (out_left == BufferAllocator::BUFFER_UNUSED) {
        if (pushed_path) pop_path();
        return TypedValue::error_val();
    }
    std::uint16_t out_right = 0xFFFF;
    if (emit_stereo) {
        out_right = alloc_buffer(n.location);
        if (out_right == BufferAllocator::BUFFER_UNUSED) {
            if (pushed_path) pop_path();
            return TypedValue::error_val();
        }
        if (out_right != out_left + 1) {
            error("E166", "Internal error: stereo buffer allocation not adjacent",
                  n.location);
            if (pushed_path) pop_path();
            return TypedValue::error_val();
        }
    }

    cedar::Instruction inst{};
    inst.opcode = builtin->opcode;
    inst.out_buffer = out_left;
    inst.inputs[0] = expanded_args.size() > 0 ? expanded_args[0] : 0xFFFF;
    inst.inputs[1] = expanded_args.size() > 1 ? expanded_args[1] : 0xFFFF;
    inst.inputs[2] = expanded_args.size() > 2 ? expanded_args[2] : 0xFFFF;
    inst.inputs[3] = expanded_args.size() > 3 ? expanded_args[3] : 0xFFFF;
    inst.inputs[4] = expanded_args.size() > 4 ? expanded_args[4] : 0xFFFF;
    inst.rate = builtin->inst_rate;
    inst.flags = static_cast<std::uint16_t>(
        (emit_stereo ? cedar::InstructionFlag::STEREO_OUTPUT : 0u) |
        (primary_stereo ? cedar::InstructionFlag::STEREO_INPUT : 0u));

    // FM detection on stereo-native oscillators (currently no
    // stereo-native oscillators exist, but keep the path uniform).
    if (is_upgradeable_oscillator(inst.opcode) && !expanded_args.empty()) {
        if (is_fm_modulated(expanded_args[0])) {
            inst.opcode = upgrade_for_fm(inst.opcode);
        }
    }

    // Stereo-native state ID uses plain fnv1a(semantic_path) — no
    // /L suffix, no XOR. Per-channel fields live inside one state
    // struct (see DattorroState predelay_buffer[2], etc.).
    inst.state_id = compute_state_id();

#ifdef CEDAR_HOST_EXTENSIONS
    // Hosted nodes are stereo devices (hosting PRD §6.1), so they
    // take this branch — the generic manifest hook below is
    // unreachable for stereo-native builtins. Record here too.
    if (inst.opcode == cedar::Opcode::HOST_OP &&
        !record_host_node_manifest(inst, *builtin, func_name, n,
                                   c.host_seq_state_id)) {
        if (pushed_path) pop_path();
        return TypedValue::error_val();
    }
#endif

    emit_extended_params_init(inst.state_id, *builtin, arg_buffers);
    emit(inst);

    if (pushed_path) pop_path();

    if (emit_stereo) {
        register_stereo(node, out_left, out_right);
        return cache_and_return(node,
            TypedValue::stereo_signal(out_left, out_right));
    }
    return cache_and_return(node, TypedValue::signal(out_left));
}

TypedValue CodeGenerator::emit_chord_expanded_call(const BuiltinEmitCtx& c) {
    const NodeIndex node = c.node;
    const Node& n = c.n;
    const BuiltinInfo* builtin = &c.builtin;
    const bool pushed_path = c.pushed_path;
    const std::vector<std::uint16_t>& arg_buffers = c.arg_buffers;
    const int expansion_arg_idx = c.expansion_arg_idx;
    const std::vector<std::uint16_t>& expansion_buffers = c.expansion_buffers;

    std::vector<TypedValue> result_elements;
    std::size_t n_params = builtin->total_params();

    for (std::size_t i = 0; i < expansion_buffers.size(); ++i) {
        // Push unique path for each expansion
        push_path("elem" + std::to_string(i));

        // Create argument buffers with expanded element substituted
        auto expanded_args = arg_buffers;
        expanded_args[static_cast<std::size_t>(expansion_arg_idx)] = expansion_buffers[i];

        // Fill in defaults for this instance
        for (std::size_t j = expanded_args.size(); j < n_params; ++j) {
            if (builtin->has_default(j)) {
                const std::uint16_t default_buf =
                    codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                        .const_value(builtin->get_default(j))
                        .emit(*this, n.location);
                if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                    pop_path();
                    if (pushed_path) pop_path();
                    return TypedValue::error_val();
                }
                expanded_args.push_back(default_buf);
            }
        }

        // Output width: Stereo declares "always emit a pair";
        // Match declares "follow primary signal input", which
        // is always single-buffer on this branch (expansion
        // is on a control slot, slot 0 stays mono).
        bool emit_stereo;
        if (builtin->stereo_native) {
            switch (builtin->output_channels) {
                case ChannelCount::Match:
                case ChannelCount::Mono:
                    emit_stereo = false;
                    break;
                case ChannelCount::Stereo:
                default:
                    emit_stereo = true;
                    break;
            }
        } else {
            emit_stereo = false;
        }

        // Allocate output buffer(s): adjacent L/R pair when
        // emitting stereo, single buffer otherwise.
        std::uint16_t inst_out = alloc_buffer(n.location);
        std::uint16_t inst_out_r = 0xFFFF;
        if (inst_out == BufferAllocator::BUFFER_UNUSED) {
            pop_path();
            if (pushed_path) pop_path();
            return TypedValue::error_val();
        }
        if (emit_stereo) {
            inst_out_r = alloc_buffer(n.location);
            if (inst_out_r == BufferAllocator::BUFFER_UNUSED) {
                pop_path();
                if (pushed_path) pop_path();
                return TypedValue::error_val();
            }
            if (inst_out_r != inst_out + 1) {
                error("E166", "Internal error: stereo buffer allocation not adjacent",
                      n.location);
                pop_path();
                if (pushed_path) pop_path();
                return TypedValue::error_val();
            }
        }

        // Build instruction for this instance
        cedar::Instruction inst{};
        inst.opcode = builtin->opcode;
        inst.out_buffer = inst_out;
        inst.inputs[0] = expanded_args.size() > 0 ? expanded_args[0] : 0xFFFF;
        inst.inputs[1] = expanded_args.size() > 1 ? expanded_args[1] : 0xFFFF;
        inst.inputs[2] = expanded_args.size() > 2 ? expanded_args[2] : 0xFFFF;
        inst.inputs[3] = expanded_args.size() > 3 ? expanded_args[3] : 0xFFFF;
        inst.inputs[4] = expanded_args.size() > 4 ? expanded_args[4] : 0xFFFF;
        inst.rate = builtin->inst_rate;
        if (emit_stereo) {
            inst.flags = static_cast<std::uint16_t>(
                cedar::InstructionFlag::STEREO_OUTPUT);
            // (Stereo input through array expansion on a control
            // slot is not currently expressible — the expansion
            // arg occupies slot >0; slot 0 stays single-buffer.)
        }

        // FM detection for this instance
        if (is_upgradeable_oscillator(inst.opcode) && !expanded_args.empty()) {
            if (is_fm_modulated(expanded_args[0])) {
                inst.opcode = upgrade_for_fm(inst.opcode);
            }
        }

        // Compute state_id with unique path
        inst.state_id = compute_state_id();
        emit_extended_params_init(inst.state_id, *builtin, expanded_args);
        emit(inst);

        if (emit_stereo) {
            register_stereo(node, inst_out, inst_out_r);
            result_elements.push_back(
                TypedValue::stereo_signal(inst_out, inst_out_r));
        } else {
            result_elements.push_back(TypedValue::signal(inst_out));
        }
        pop_path();
    }

    // Pop the outer stateful path
    if (pushed_path) pop_path();

    // Build array result. Stereo-source primary input is handled
    // by the stereo-native emission path above (single dispatch),
    // so this branch only ever sees chord/array expansion.
    std::uint16_t first_buf = result_elements[0].buffer;
    auto tv = TypedValue::make_array(std::move(result_elements), first_buf);

    return cache_and_return(node, tv);
}

TypedValue CodeGenerator::emit_generic_builtin_tail(const BuiltinEmitCtx& c) {
    const NodeIndex node = c.node;
    const Node& n = c.n;
    const std::string& func_name = c.func_name;
    const BuiltinInfo* builtin = &c.builtin;
    const SourceLocation call_loc = c.call_loc;
    const bool pushed_path = c.pushed_path;
    std::vector<std::uint16_t>& arg_buffers = c.arg_buffers;

    // Restore call location before emitting default parameter instructions
    // (visiting arguments may have changed current_source_loc_)
    current_source_loc_ = call_loc;

    // Fill in missing optional arguments with defaults
    std::size_t total_params = builtin->total_params();
    for (std::size_t i = arg_buffers.size(); i < total_params; ++i) {
        if (builtin->has_default(i)) {
            const std::uint16_t default_buf =
                codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                    .const_value(builtin->get_default(i))
                    .emit(*this, n.location);
            if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                if (pushed_path) pop_path();
                return TypedValue::error_val();
            }

            arg_buffers.push_back(default_buf);
        }
    }

    // Scalar SAMPLE_PLAY (the `sample()` builtin) routes through
    // emit_sample_chain so all SAMPLE_PLAY emission sits behind one
    // helper. Pre-migration, this path emitted a single SAMPLE_PLAY
    // here with inputs[3]/[4] populated from arg_buffers (always
    // BUFFER_UNUSED for the 3-input sample() builtin) and state_id
    // from compute_state_id(). emit_sample_chain Kind::Scalar
    // produces the same instruction; verified by golden test G10.
    if (builtin->opcode == cedar::Opcode::SAMPLE_PLAY) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Scalar;
        ctx.trigger_buf = arg_buffers.size() > 0 ? arg_buffers[0] : static_cast<std::uint16_t>(0xFFFF);
        // Caller-supplied pitch buffer (the 2nd arg) — helper skips
        // its own PUSH_CONST 1.0 emission when this is set.
        ctx.pitch_buf   = arg_buffers.size() > 1 ? arg_buffers[1] : BufferAllocator::BUFFER_UNUSED;
        ctx.value_buf   = arg_buffers.size() > 2 ? arg_buffers[2] : static_cast<std::uint16_t>(0xFFFF);
        ctx.velocity_buf = BufferAllocator::BUFFER_UNUSED;  // no MUL on scalar path
        ctx.rate = builtin->inst_rate;
        ctx.loc = n.location;
        if (pushed_path) {
            ctx.seq_state_id = compute_state_id();
            pop_path();
        } else {
            ctx.seq_state_id = 0;
        }
        std::uint16_t out = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::error_val();
        }
        TypedValue result = TypedValue::signal(out);
        // Match on a non-stereo-native builtin falls back to Mono
        // (Match is only meaningful on the stereo-native path).
        result.channels =
            (builtin->output_channels == ChannelCount::Stereo)
                ? ChannelCount::Stereo
                : ChannelCount::Mono;
        if (result.channels == ChannelCount::Stereo) {
            result.right_buffer = static_cast<std::uint16_t>(out + 1);
        }
        return cache_and_return(node, result);
    }

    // Allocate output buffer
    std::uint16_t out = alloc_buffer(n.location);
    if (out == BufferAllocator::BUFFER_UNUSED) {
        if (pushed_path) pop_path();
        return TypedValue::error_val();
    }

    // Build instruction
    cedar::Instruction inst{};
    inst.opcode = builtin->opcode;
    inst.out_buffer = out;
    inst.inputs[0] = arg_buffers.size() > 0 ? arg_buffers[0] : 0xFFFF;
    inst.inputs[1] = arg_buffers.size() > 1 ? arg_buffers[1] : 0xFFFF;
    inst.inputs[2] = arg_buffers.size() > 2 ? arg_buffers[2] : 0xFFFF;
    inst.inputs[3] = arg_buffers.size() > 3 ? arg_buffers[3] : 0xFFFF;
    inst.inputs[4] = arg_buffers.size() > 4 ? arg_buffers[4] : 0xFFFF;
    inst.rate = builtin->inst_rate;

    // Special handling for ADSR: pack release time (arg 4) into rate field
    // Release time in tenths of seconds (0-255 -> 0-25.5s)
    if (func_name == "adsr" && arg_buffers.size() >= 5) {
        // Find the release argument value from AST to extract literal
        NodeIndex adsr_arg = n.first_child;
        for (std::size_t idx = 0; adsr_arg != NULL_NODE && idx < 5; ++idx) {
            if (idx == 4) {
                const Node& arg_node = ast_->arena[adsr_arg];
                NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                     arg_node.first_child : adsr_arg;
                if (arg_value != NULL_NODE) {
                    const Node& val_node = ast_->arena[arg_value];
                    if (val_node.type == NodeType::NumberLit) {
                        float release_val = static_cast<float>(val_node.as_number());
                        inst.rate = static_cast<std::uint8_t>(
                            std::clamp(release_val / 0.1f, 0.0f, 255.0f));
                    }
                }
                break;
            }
            adsr_arg = ast_->arena[adsr_arg].next_sibling;
        }
    }

    // delay / delay_ms / delay_smp encode the time unit (0=s, 1=ms,
    // 2=samples) in the rate field. This is fully data-driven via
    // BuiltinInfo::inst_rate (applied at `inst.rate = builtin->inst_rate`
    // above; the three entries declare 0/1/2) — no func-name ladder
    // needed. They are also registered in the Phase-3 builtin overload
    // table (lookup_builtin_overloads) as single Builtin patterns.

    // PRD prd-extended-params §6b — phaser's `feedback` and `stages`
    // are now full extended params (ExtendedParams<3>, slots 0/1),
    // along with `lfo_phase` (slot 2). The legacy rate-field bit
    // packing is gone; emit_extended_params_init below handles them.

    // Generate state_id from current path (already pushed if stateful)
    if (pushed_path) {
        inst.state_id = compute_state_id();
        pop_path();
    } else {
        inst.state_id = 0;
    }

    // FM Detection: Automatically upgrade oscillators to 4x when frequency
    // input comes from an audio-rate source (another oscillator, noise, etc.)
    if (is_upgradeable_oscillator(inst.opcode) && !arg_buffers.empty()) {
        std::uint16_t freq_buffer = arg_buffers[0];
        if (is_fm_modulated(freq_buffer)) {
            inst.opcode = upgrade_for_fm(inst.opcode);
        }
    }

#ifdef CEDAR_HOST_EXTENSIONS
    // A hosted node names its instance with a string literal that never
    // reaches an input buffer (StringLit lowers to a bufferless
    // TypedValue, so inputs[] already carries 0xFFFF for that slot).
    // Record the call site so the host can bind an instance to this
    // state_id off the audio thread before resuming it.
    if (inst.opcode == cedar::Opcode::HOST_OP &&
        !record_host_node_manifest(inst, *builtin, func_name, n,
                                   c.host_seq_state_id)) {
        return TypedValue::error_val();
    }
#endif

    emit_extended_params_init(inst.state_id, *builtin, arg_buffers);
    emit(inst);
    // Propagate the builtin's declared output channel count (PRD §5.2).
    // For the common mono-in/mono-out case this stays Mono. It is the
    // hook for future stereo-native generators (e.g. `in()`) to emit
    // Stereo results through the generic path; today all stereo-output
    // builtins route through codegen_stereo.cpp, so this only ever
    // takes the Mono branch in the current tree.
    TypedValue result = TypedValue::signal(out);
    // Match only resolves on the stereo-native path; if we land
    // here with Match, treat as Mono (no Stereo pair allocated).
    result.channels =
        (builtin->output_channels == ChannelCount::Stereo)
            ? ChannelCount::Stereo
            : ChannelCount::Mono;
    if (result.channels == ChannelCount::Stereo) {
        result.right_buffer = static_cast<std::uint16_t>(out + 1);
    }
    return cache_and_return(node, result);
}

}  // namespace akkado
