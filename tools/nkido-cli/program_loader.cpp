#include "program_loader.hpp"

#include "asset_loader.hpp"
#include "cedar/io/uri_resolver.hpp"
#include "cedar/opcodes/midi.hpp"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nkido {
namespace {

// Derive a bank name from a manifest URI. Mirrors the web
// `BankRegistry.extractBankName` heuristic: the last URL/path segment with
// any trailing `.json` / `.strudel.json` removed, or — if that segment is
// "strudel" — the parent segment (so `…/Dirt-Samples/strudel.json` →
// `Dirt-Samples`). For bare `github:user/repo` URIs the last `/`-separated
// segment is the repo name.
std::string derive_bank_name(const std::string& uri) {
    std::string last;
    std::string parent;
    std::size_t pos = 0;
    while (pos < uri.size()) {
        auto slash = uri.find('/', pos);
        if (slash == std::string::npos) {
            parent = last;
            last = uri.substr(pos);
            break;
        }
        if (slash > pos) {
            parent = last;
            last = uri.substr(pos, slash - pos);
        }
        pos = slash + 1;
    }
    auto strip_ext = [](std::string& s, const std::string& ext) {
        if (s.size() > ext.size() &&
            s.compare(s.size() - ext.size(), ext.size(), ext) == 0) {
            s.resize(s.size() - ext.size());
        }
    };
    strip_ext(last, ".strudel.json");
    strip_ext(last, ".json");
    if (last == "strudel" && !parent.empty()) return parent;
    if (last.empty()) return "unnamed";
    return last;
}

}  // namespace

bool prepare_program_assets(cedar::VM& vm,
                            const Options& opts,
                            const akkado::CompileResult& cr,
                            std::ostream& err) {
    // Build the bank list. Order matters: --bank flags first, then
    // samples() declarations in source order. Each bank is registered
    // both into `default_banks` (searched first-hit-wins for unqualified
    // RequiredSample lookups) and `named_banks` (keyed by derived bank
    // name for `.bank("Name")` qualified lookups).
    std::vector<BankManifest> default_banks;
    std::unordered_map<std::string, BankManifest> named_banks;

    auto load_bank = [&](const std::string& uri) -> bool {
        try {
            auto m = fetch_bank_manifest(uri);
            std::string name = derive_bank_name(uri);
            err << "Loaded bank manifest '" << name << "' from " << uri
                << " (" << m.samples.size() << " samples)\n";
            default_banks.push_back(m);
            named_banks[std::move(name)] = std::move(m);
            return true;
        } catch (const std::exception& e) {
            err << "error: bank '" << uri << "' failed to load: " << e.what() << "\n";
            return false;
        }
    };

    for (const auto& uri : opts.bank_uris) {
        if (!load_bank(uri)) return false;
    }
    for (const auto& req : cr.required_uris) {
        if (req.kind == akkado::UriKind::SampleBank) {
            if (!load_bank(req.uri)) return false;
        }
    }

    // Built-in default kit (bpb_808_clean). Appended last so user-provided
    // banks always shadow it for unqualified lookups, but bare names like
    // `bd`, `hh`, `sd` still resolve when the user supplies nothing.
    // Suppressed by --no-default-bank, or when NKIDO_DEFAULT_KIT="".
    //
    // Note on variant aliasing: this kit uses single-element variant arrays
    // per name (`"bd": ["kick01rr1.wav"]`), so `s"bd:N"` for any N aliases
    // back to `bd` via `lookup_variant`'s modulo. This matches the web's
    // current DEFAULT_DRUM_KIT behavior. Users who want true variant
    // indexing should supply their own bank manifest.
    //
    // Only emit "not available" diagnostics when the program actually
    // references samples — silent otherwise to keep load output clean for
    // pure-synth patches.
    if (!opts.no_default_bank) {
        const bool has_required = !cr.required_samples_extended.empty();
        // Discovery diagnostics go to a sink for synth-only patches so the
        // load output stays clean; only emit them when the program actually
        // references samples.
        std::ostringstream sink;
        std::ostream& diag = has_required ? err : static_cast<std::ostream&>(sink);
        if (auto uri = find_default_bank_uri(diag)) {
            try {
                auto m = fetch_bank_manifest(*uri);
                std::string name = derive_bank_name(*uri);
                if (has_required) {
                    err << "Loaded default kit '" << name << "' from "
                        << *uri << " (" << m.samples.size() << " samples)\n";
                }
                default_banks.push_back(m);
                // Don't clobber a user-named bank with the same key.
                named_banks.try_emplace(std::move(name), std::move(m));
            } catch (const std::exception& e) {
                if (has_required) {
                    err << "info: default kit '" << *uri
                        << "' could not be loaded: " << e.what()
                        << " (bare sample names will not resolve)\n";
                }
            }
        } else if (has_required) {
            err << "info: no default sample kit available, bare sample"
                   " names will not resolve (suppress with --no-default-bank)\n";
        }
    }

    // SoundFonts (one fetch each, registered under their URI tail as
    // display name).
    for (const auto& uri : opts.soundfont_uris) {
        std::string display = uri;
        auto last = uri.find_last_of('/');
        if (last != std::string::npos) display = uri.substr(last + 1);
        const int sf_id = load_soundfont_uri(vm, uri, display);
        if (sf_id < 0) {
            err << "error: SoundFont '" << uri << "' failed to load\n";
            return false;
        }
    }

    // Single-sample URIs (`--sample name=uri` or `--sample uri`; uri
    // tail used as the registry name when no explicit name).
    for (const auto& spec : opts.sample_uris) {
        std::string name, uri;
        auto eq = spec.find('=');
        if (eq != std::string::npos) {
            name = spec.substr(0, eq);
            uri  = spec.substr(eq + 1);
        } else {
            uri = spec;
            auto last = uri.find_last_of('/');
            std::string filename = last == std::string::npos ? uri : uri.substr(last + 1);
            auto dot = filename.find_last_of('.');
            name = dot == std::string::npos ? filename : filename.substr(0, dot);
        }
        if (load_sample_uri(vm, uri, name) == 0) {
            err << "error: sample '" << uri << "' failed to load\n";
            return false;
        }
    }

    // Resolve every RequiredSample referenced by the program against the
    // loaded bank list. Default-bank events search `default_banks`
    // first-hit-wins; events qualified with `.bank("Name")` look up
    // `named_banks` by the derived bank name.
    if (!cr.required_samples_extended.empty()) {
        register_required_samples(
            vm, cr.required_samples_extended, default_banks, named_banks);
    }

    // Wavetables declared via wt_load() in source order. The runtime slot
    // ID must match the inst.rate value the compiler baked into the
    // bytecode; warn if the slot drifts. Path is interpreted relative to
    // CWD if not absolute.
    for (const auto& wt : cr.required_wavetables) {
        std::string err_msg;
        const int id = vm.wavetable_registry().load_from_file(
            wt.name, wt.path, &err_msg);
        if (id < 0) {
            err << "error: " << err_msg << "\n";
            return false;
        }
        if (id != wt.id) {
            err << "warning: wavetable '" << wt.name
                << "' got runtime slot " << id
                << " but compiler expected " << wt.id << "\n";
        }
    }

    return true;
}

void resolve_sample_ids_in_events(cedar::VM& vm, akkado::CompileResult& cr) {
    for (auto& init : cr.state_inits) {
        if (init.type != akkado::StateInitData::Type::SequenceProgram) continue;
        for (const auto& mapping : init.sequence_sample_mappings) {
            if (mapping.seq_idx >= init.sequence_events.size()) continue;
            auto& events = init.sequence_events[mapping.seq_idx];
            if (mapping.event_idx >= events.size()) continue;
            std::string lookup;
            if (mapping.bank.empty() || mapping.bank == "default") {
                lookup = mapping.variant > 0
                    ? mapping.sample_name + ":" + std::to_string(mapping.variant)
                    : mapping.sample_name;
            } else {
                lookup = mapping.bank + "_" + mapping.sample_name + "_"
                       + std::to_string(mapping.variant);
            }
            const std::uint32_t id = vm.sample_bank().get_sample_id(lookup);
            auto& ev = events[mapping.event_idx];
            const std::uint8_t slot = mapping.value_slot;
            if (slot < cedar::MAX_VALUES_PER_EVENT) {
                ev.values[slot] = static_cast<float>(id);
            }
        }
    }
}

void apply_builtin_var_overrides(cedar::VM& vm,
                                 const akkado::CompileResult& cr) {
    for (const auto& ov : cr.builtin_var_overrides) {
        if (ov.name == "bpm") {
            vm.set_bpm(ov.value);
        }
    }
}

void apply_state_inits(cedar::VM& vm,
                       const akkado::CompileResult& result,
                       std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    seq_storage.reserve(seq_storage.size() + result.state_inits.size());
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            std::vector<cedar::Sequence> seq_copy = init.sequences;
            for (std::size_t i = 0; i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
                if (!init.sequence_events[i].empty()) {
                    seq_copy[i].events = const_cast<cedar::Event*>(init.sequence_events[i].data());
                    seq_copy[i].num_events = static_cast<std::uint32_t>(init.sequence_events[i].size());
                    seq_copy[i].capacity = static_cast<std::uint32_t>(init.sequence_events[i].size());
                }
            }
            seq_storage.push_back(std::move(seq_copy));
            auto& stored = seq_storage.back();
            vm.init_sequence_program_state(
                init.state_id, stored.data(), stored.size(),
                init.cycle_length, init.is_sample_pattern, init.total_events);
            if (init.iter_n > 0) {
                vm.init_sequence_iter_state(init.state_id, init.iter_n, init.iter_dir);
            }
        } else if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            vm.init_poly_state(init.state_id, init.poly_seq_state_id,
                               init.poly_max_voices, init.poly_mode,
                               init.poly_steal_strategy);
        } else if (init.type == akkado::StateInitData::Type::ExtendedParams) {
            vm.init_extended_params(init.state_id,
                                    init.ext_constants.data(),
                                    init.ext_buffer_indices.data(),
                                    init.ext_count);
        } else if (init.type == akkado::StateInitData::Type::Timeline) {
            auto& state = vm.states().get_or_create<cedar::TimelineState>(init.state_id);
            state.num_points = std::min(
                static_cast<std::uint32_t>(init.timeline_breakpoints.size()),
                static_cast<std::uint32_t>(cedar::TimelineState::MAX_BREAKPOINTS));
            for (std::uint32_t i = 0; i < state.num_points; ++i) {
                state.points[i] = init.timeline_breakpoints[i];
            }
            state.loop = init.timeline_loop;
            state.loop_length = init.timeline_loop_length;
        }
    }
}

// Resolve every File-kind RequiredMidiSource via the URI resolver and
// register the parsed sequence on the VM, then init the MidiQueueState
// for every MIDI source (live + file). Called from load_and_prepare_immediate
// after load_program_immediate so the registry survives the VM reset
// triggered by program load. Used by render mode (no AudioEngine wrapper)
// AND play mode (AudioEngine::apply_midi_route_plan replays the same
// init idempotently before opening the live device routes).
void apply_midi_source_inits(cedar::VM& vm,
                             const std::vector<akkado::RequiredMidiSource>& required,
                             std::ostream& err) {
    std::unordered_set<std::string> loaded;
    auto& resolver = cedar::UriResolver::instance();
    for (const auto& req : required) {
        if (req.kind != cedar::MidiSourceKind::File) continue;
        if (req.name_or_path.empty()) continue;
        if (!loaded.insert(req.name_or_path).second) continue;
        auto result = resolver.load(req.name_or_path);
        if (!result.success()) {
            err << "[midi] failed to fetch '" << req.name_or_path
                << "': " << result.error().message << "\n";
            continue;
        }
        const auto& bytes = result.buffer();
        const std::int32_t rc = vm.load_midi_file(req.name_or_path,
                                                  bytes.data(), bytes.size());
        if (rc < 0) {
            err << "[midi] parse failed for '" << req.name_or_path << "'\n";
        }
    }
    for (const auto& req : required) {
        vm.init_midi_queue_state(
            req.state_id,
            req.kind,
            req.name_or_path.empty() ? nullptr : req.name_or_path.c_str(),
            req.channel_filter,
            req.loop,
            req.tempo_mode);
    }
}

bool load_and_prepare_immediate(cedar::VM& vm,
                                const Options& opts,
                                LoadResult& load,
                                std::vector<std::vector<cedar::Sequence>>& seq_storage,
                                std::ostream& err) {
    if (load.compile_result) {
        if (!prepare_program_assets(vm, opts, *load.compile_result, err)) {
            return false;
        }
        resolve_sample_ids_in_events(vm, *load.compile_result);
    } else if (!opts.bank_uris.empty() || !opts.soundfont_uris.empty()
               || !opts.sample_uris.empty()) {
        err << "warning: --bank/--soundfont/--sample with .cedar input: "
               "RequiredSample metadata is unavailable; sample lookups may fail.\n";
        akkado::CompileResult empty;
        (void)prepare_program_assets(vm, opts, empty, err);
    }

    if (!vm.load_program_immediate(load.instructions)) {
        err << "error: failed to load program into VM\n";
        return false;
    }

    if (load.compile_result) {
        apply_state_inits(vm, *load.compile_result, seq_storage);
        apply_builtin_var_overrides(vm, *load.compile_result);
        apply_midi_source_inits(vm, load.compile_result->required_midi_sources,
                                err);
    }
    return true;
}

}  // namespace nkido
