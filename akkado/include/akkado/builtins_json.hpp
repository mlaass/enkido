#pragma once

#include <string>

namespace akkado {

/// Serialize the BUILTIN_FUNCTIONS / BUILTIN_ALIASES registry to a JSON string
/// for editor consumption. The shape is:
///
///   {
///     "functions": {
///       "<name>": {
///         "params": [
///           {"name": "<param>", "required": true|false [, "default": <num>]
///            [, "type": "record"]
///            [, "optionFields": [{"name", "type", "default"?, "description"?, "values"?}, ...]]
///            [, "acceptsSpread": true|false]}
///         ],
///         "description": "<docstring>"
///       }, ...
///     },
///     "aliases": { "<from>": "<to>", ... },
///     "keywords": ["fn", "seq", "timeline", ...]
///   }
///
/// The optional `optionFields` block is emitted when a parameter is typed as
/// ParamValueType::Record AND a matching OptionSchema is declared on its
/// BuiltinInfo. See PRD docs/prd-records-system-unification.md §5.1.
std::string serialize_builtins_json();

}  // namespace akkado
