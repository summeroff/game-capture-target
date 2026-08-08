# Coding style (fakegame)

Machine-enforced by `.clang-format` + `scripts/format.*` + the CI **Format** job
(`clang-format-18` pinned). Human summary:

## Braces (Allman + joined else)

```cpp
// Opening brace on its own line, same column as the control keyword.
if (ok)
{
  DoWork();
}

// Exception: keep "} else {" / "} else if" on one spine.
if (a)
{
  One();
}
else if (b)
{
  Two();
}
else
{
  Three();
}

void Foo()
{
  // ...
}
```

Do **not** use K&R (`if (ok) {`) for new code. Do not invent a third style mid-file.

## Indent / width

- 2-space indent (matches existing sources)
- No tabs
- Column soft limit 100 (clang-format may wrap)
- Pointers: `Type* name` (left-aligned `*`)

## Includes

- Project headers first, then STL, then Windows/system
- Do not auto-sort includes (`SortIncludes: false`) — keep intentional groups

## Build flags

- MSVC `/W4 /permissive- /utf-8`, treat warnings as errors in verify builds when practical
- C++17, static CRT (`/MT`)

## Format commands

```bat
scripts\format.bat           rem rewrite
scripts\format.bat --check   rem CI-equivalent gate
```

```bash
./scripts/format.sh
./scripts/format.sh --check
```

Run format on touched files **before push** when the Format job is enabled.
