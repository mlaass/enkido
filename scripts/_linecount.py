import os
from collections import defaultdict

lang_by_ext = {
    '.cpp': 'C++', '.hpp': 'C++', '.h': 'C++', '.cc': 'C++', '.cxx': 'C++', '.c': 'C',
    '.ts': 'TypeScript', '.tsx': 'TypeScript',
    '.js': 'JavaScript', '.mjs': 'JavaScript', '.cjs': 'JavaScript',
    '.svelte': 'Svelte',
    '.py': 'Python',
    '.md': 'Markdown',
    '.json': 'JSON',
    '.html': 'HTML',
    '.css': 'CSS',
    '.sh': 'Shell',
    '.cmake': 'CMake',
    '.yml': 'YAML', '.yaml': 'YAML',
    '.toml': 'TOML',
    '.akkado': 'Akkado',
}

skip_dirs = {'build', 'build-cedar', 'node_modules', '.git', '.svelte-kit',
             'dist', 'out', '__pycache__', '.venv', 'venv', 'output'}

def norm(path):
    return path.replace('\\', '/').lstrip('./').lstrip('/')

def get_part(p):
    if p.startswith('cedar/third_party/'): return 'cedar (third_party)'
    if p.startswith('cedar/tests/'): return 'cedar (tests)'
    if p.startswith('cedar/'): return 'cedar'
    if p.startswith('akkado/tests/'): return 'akkado (tests)'
    if p.startswith('akkado/'): return 'akkado'
    if p.startswith('web/tests/'): return 'web (tests)'
    if p.startswith('web/share-api/test/'): return 'web (tests)'
    if p.startswith('web/src/'): return 'web (src)'
    if p.startswith('web/wasm/'): return 'web (wasm bindings)'
    if p.startswith('web/scripts/'): return 'web (build scripts)'
    if p.startswith('web/static/docs/'): return 'web (static docs)'
    if p.startswith('web/static/patches/'): return 'web (static patches)'
    if p.startswith('web/static/worklet/'): return 'web (worklet)'
    if p.startswith('web/static/'): return 'web (static)'
    if p.startswith('web/'): return 'web (config)'
    if p.startswith('tools/'): return 'tools'
    if p.startswith('experiments/'): return 'experiments'
    if p.startswith('docs/'): return 'docs'
    if p.startswith('cmake/'): return 'cmake'
    if p.startswith('scripts/'): return 'scripts'
    if p.startswith('.github/'): return '.github'
    return 'root'

stats = defaultdict(lambda: [0, 0])

for root, dirs, files in os.walk('.'):
    dirs[:] = [d for d in dirs if d not in skip_dirs and not d.startswith('.git')]
    for fname in files:
        ext = os.path.splitext(fname)[1].lower()
        if fname == 'CMakeLists.txt':
            lang = 'CMake'
        elif ext in lang_by_ext:
            lang = lang_by_ext[ext]
        else:
            continue
        fpath = os.path.join(root, fname)
        rel = norm(os.path.relpath(fpath))
        if 'generated' in rel.lower():
            continue
        if rel.endswith('.generated.ts'):
            continue
        try:
            with open(fpath, 'rb') as f:
                lines = sum(1 for _ in f)
        except Exception:
            continue
        part = get_part(rel)
        key = (part, lang)
        stats[key][0] += 1
        stats[key][1] += lines

parts_total = defaultdict(lambda: [0, 0])
langs_total = defaultdict(lambda: [0, 0])
grand_files = 0
grand_lines = 0
for (part, lang), (f, l) in stats.items():
    parts_total[part][0] += f
    parts_total[part][1] += l
    langs_total[lang][0] += f
    langs_total[lang][1] += l
    grand_files += f
    grand_lines += l

groups = {
    'akkado': ['akkado'],
    'akkado tests': ['akkado (tests)'],
    'cedar': ['cedar', 'cedar (third_party)'],
    'cedar tests': ['cedar (tests)'],
    'web': ['web (src)', 'web (wasm bindings)', 'web (build scripts)',
            'web (static docs)', 'web (static patches)', 'web (worklet)',
            'web (static)', 'web (config)'],
    'web tests': ['web (tests)'],
}
print('## GROUPED')
print()
print('| Group | Files | Lines | % |')
print('|-------|------:|------:|--:|')
named_files = named_lines = 0
for group, members in groups.items():
    f = sum(parts_total[m][0] for m in members)
    l = sum(parts_total[m][1] for m in members)
    named_files += f
    named_lines += l
    print(f'| {group} | {f} | {l:,} | {100*l/grand_lines:.1f}% |')
other_f = grand_files - named_files
other_l = grand_lines - named_lines
print(f'| other (docs, experiments, tools, scripts, cmake, root) | {other_f} | {other_l:,} | {100*other_l/grand_lines:.1f}% |')
print(f'| **TOTAL** | **{grand_files}** | **{grand_lines:,}** | 100% |')
print()

print('## DETAILED')
print()
print('| Part | Language | Files | Lines |')
print('|------|----------|------:|------:|')
for (part, lang) in sorted(stats.keys(), key=lambda k: (-stats[k][1], k[0], k[1])):
    f, l = stats[(part, lang)]
    print(f'| {part} | {lang} | {f} | {l:,} |')

print()
print('## BY PART')
print()
print('| Part | Files | Lines | % |')
print('|------|------:|------:|--:|')
for part in sorted(parts_total.keys(), key=lambda p: -parts_total[p][1]):
    f, l = parts_total[part]
    print(f'| {part} | {f} | {l:,} | {100*l/grand_lines:.1f}% |')
print(f'| **TOTAL** | **{grand_files}** | **{grand_lines:,}** | 100% |')

print()
print('## BY LANGUAGE')
print()
print('| Language | Files | Lines | % |')
print('|----------|------:|------:|--:|')
for lang in sorted(langs_total.keys(), key=lambda l: -langs_total[l][1]):
    f, l = langs_total[lang]
    print(f'| {lang} | {f} | {l:,} | {100*l/grand_lines:.1f}% |')
print(f'| **TOTAL** | **{grand_files}** | **{grand_lines:,}** | 100% |')
