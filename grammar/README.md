# Bender grammar

A TextMate grammar for Bender source files (`.ben`), packaged so it can be
used by editors and submitted to
[GitHub Linguist](https://github.com/github-linguist/linguist).

- `syntaxes/bender.tmLanguage.json` — the grammar (`source.bender`)
- `package.json` — VS Code / editor manifest
- `language-configuration.json` — comments, brackets, auto-closing
- `samples/` — representative `.ben` programs
- `linguist/` — ready-to-paste Linguist metadata and PR checklist

## Scope

| File | Meaning |
|---|---|
| `.ben` | Bender source — this grammar |
| `.benx` | Compiled Bender image (marshalled VM environment) — binary, not highlighted |

`.benx` is the output of `bender -c app.ben app.benx` and is loaded with
`bender -i app.benx`. It contains bytecode and constants for the Bender VM,
not native machine code, so it is intentionally not registered as a source
language.

## Regenerate

The grammar is generated from the running core library, so new core functions
and macros are picked up automatically:

```sh
bender tools/linguist_grammar.ben > grammar/syntaxes/bender.tmLanguage.json
```

Special forms are always `keyword.control.bender`, macros are detected through
their `:macro` property, and the remaining bindings become
`support.function.bender`.

## Validate

```sh
python3 -m json.tool grammar/syntaxes/bender.tmLanguage.json > /dev/null
```

For a real tokenizer check, the grammar is compatible with `vscode-textmate` +
`vscode-oniguruma`; see `linguist/PR-CHECKLIST.md` for the commands.

## Highlighted constructs

- line comments (`# ...`)
- double-quoted and buffer strings (`"..."`, `@"..."`)
- long/backtick strings (`` `...` ``, multiple backticks)
- keywords (`:name`, with `:hint` called out separately)
- booleans and `nil`
- decimal, hex, and radix numbers
- reader macros (`'`, `~`, `,`, `;`)
- recursive `()`, `[]`, `{}` and their `@` mutable forms
- special forms and macros as control keywords
- core functions as support functions
- everything else as ordinary symbols

## Submitting to Linguist

Linguist does **not** consume this repository directly. The grammar must live
in a public repository of its own, then be added to Linguist with
`script/add-grammar`. `linguist/PR-CHECKLIST.md` has the exact sequence and
the current acceptance requirements.

Two things to know before opening the PR:

1. The grammar repository must carry one of Linguist's
   [approved licenses](https://github.com/github-linguist/linguist/blob/main/vendor/README.md)
   (MIT is fine; see `LICENSE`).
2. Linguist only adds an extension once there is **sufficient public usage**
   of that extension on GitHub. Budget time to publish real `.ben` code first.

## License

MIT. The grammar derives from the upstream project's TextMate grammar, which
is MIT and copyright Calvin Rose and contributors.
