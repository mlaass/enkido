---
title: Documentation Guide
category: concepts
keywords: [contributing, documentation, guide, standards]
---

# Documentation Guide

This guide outlines standards and practices for writing NKIDO documentation.

## Content Structure

### Frontmatter

Every documentation file must have YAML frontmatter with:

```yaml
---
title: Document Title
category: builtins | language | mini-notation | concepts | tutorials
keywords: [keyword1, keyword2, ...]
order: 1  # Optional, for tutorials
subcategory: oscillators  # Optional, for builtins
---
```

### Builtin Documentation Format

For each builtin function:

1. **Heading** - Function name as h2 (`## funcname`)
2. **One-line description** - Bold title with brief description
3. **Parameter table** - Name, type, default, description
4. **Aliases** - List alternative names
5. **Explanation** - 1-2 paragraphs explaining the function
6. **Examples** - 2-3 code examples with comments
7. **Related** - Links to related functions

Example:

```markdown
## lp

**Lowpass Filter** - Passes frequencies below the cutoff.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| in    | signal | -       | Input signal |
| cut   | signal | -       | Cutoff frequency |
| q     | number | 0.707   | Resonance |

Aliases: `lowpass`, `svflp`

Description of how the filter works...

```akk
// Example code
saw(220) |> lp(%, 800) |> out(%, %)
```

Related: [hp](#hp), [bp](#bp)
```

### Tutorial Format

Tutorials should:

1. Start with a clear goal
2. Build concepts progressively
3. Include runnable examples after each concept
4. End with "Next Steps" linking to the next tutorial

## Writing Style

### Voice and Tone

- Use present tense, active voice
- Be concise but complete
- Address the reader directly ("you can...", "try this...")
- Avoid jargon without explanation

### Code Examples

- Every example should be self-contained and produce audible output
- Include comments explaining non-obvious parts
- Start with simple examples, build to complex
- Test all examples before committing

### Terminology

Use consistent terms:

| Term | Meaning |
|------|---------|
| signal | A continuous audio-rate value |
| trigger | A momentary pulse (0 to 1 transition) |
| gate | A sustained on/off signal |
| frequency | Pitch in Hz |
| cutoff | Filter frequency parameter |
| resonance/Q | Filter emphasis at cutoff |

## Keywords

Keywords enable F1 help and search. Include:

- Function names and aliases
- Parameter names
- Common synonyms users might search
- Related concepts

## Priority Order

When expanding documentation, follow this priority:

1. **High Priority** (most commonly used):
   - Oscillators: sin, saw, tri, sqr
   - Filters: lp, hp, moog
   - Envelopes: adsr, ar
   - Output: out
   - Core operators: |>, %

2. **Medium Priority**:
   - Effects: delay, reverb, chorus
   - Utility: mtof, noise, clock
   - Mini-notation basics

3. **Lower Priority**:
   - Math functions
   - Advanced effects
   - Edge cases

## Review Checklist

Before submitting documentation:

- [ ] Frontmatter is complete and valid
- [ ] All examples tested and working
- [ ] F1 keywords registered (included in keywords array)
- [ ] Related functions linked
- [ ] Spelling and grammar checked
- [ ] Consistent with existing documentation style

## File Organization

```
docs/
├── reference/
│   ├── builtins/
│   │   ├── oscillators.md
│   │   ├── filters.md
│   │   └── ...
│   ├── language/
│   └── mini-notation/
├── concepts/
├── tutorials/
└── DOCUMENTATION_GUIDE.md
```
