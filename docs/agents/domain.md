# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- **`GLOSSARY.md`** at the repo root, if it exists.
- **`docs/decisions/`**: ADRs named `NNNN-YYYY-MM-DD-short-title.md`. Read the ADRs that touch the area you're about to work in. `docs/architecture.md` is the system blueprint; consult it before modifying core modules.

If any of these files don't exist, **proceed silently**. Don't flag their absence; don't suggest creating them upfront. The `/domain-modeling` skill (reached via `/grill-with-docs` and `/improve-codebase-architecture`) creates `GLOSSARY.md` lazily when terms actually get resolved.

## File structure

Single-context repo:

```
/
├── GLOSSARY.md
└── docs/
    ├── architecture.md
    └── decisions/
        ├── 0000-template.md
        └── 0001-....md
```

## Use the glossary's vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a hypothesis, a test name), use the term as defined in `GLOSSARY.md`. Don't drift to synonyms the glossary explicitly avoids.

If the concept you need isn't in the glossary yet, that's a signal: either you're inventing language the project doesn't use (reconsider) or there's a real gap (note it for `/domain-modeling`).

## Flag ADR conflicts

If your output contradicts an existing ADR, surface it explicitly rather than silently overriding:

> _Contradicts ADR-0007 (optional per-frame CRC32C integrity), but worth reopening because…_
