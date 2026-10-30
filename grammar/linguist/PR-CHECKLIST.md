# Linguist submission checklist

This directory prepares the upstream
[github-linguist/linguist](https://github.com/github-linguist/linguist) change.
Nothing here is consumed automatically.

## 0. Publish the grammar repo

Linguist vendors grammars from a separate public repository. Publish this
`grammar/` directory as its own repo, for example
`https://github.com/<org>/bender-grammar`, containing at least:

```text
syntaxes/bender.tmLanguage.json
package.json
language-configuration.json
LICENSE
```

The repository must carry an approved license (MIT, Apache-2.0, BSD, etc.).
MIT is already provided in `../LICENSE`.

## 1. Add the language

Append the entry from `languages.yml.snippet` to
`lib/linguist/languages.yml`, keeping the file in ASCII order. Do **not** add
`language_id`; step 4 generates it.

## 2. Add the grammar

```sh
script/add-grammar https://github.com/<org>/bender-grammar
```

This validates the grammar, copies it under `vendor/grammars/`, and updates
`grammars.yml` with:

```yaml
vendor/grammars/bender-grammar:
- source.bender
```

If validation fails, fix the grammar rather than hand-editing `grammars.yml`.

## 3. Add samples

Put representative programs under `samples/Bender/`. Linguist explicitly
rejects "hello world" and tutorial examples. Use real code that shows the
language's structure. At least two or three files are expected.

The files in `../samples/` are a starting point, but prefer code from real
public Bender projects. State the source URL and license for each file, or
state that you wrote it and release it under MIT.

## 4. Generate the language ID

```sh
script/update-ids
```

Commit the resulting `language_id` in `languages.yml`.

## 5. Run the checks

```sh
bundle exec rake test
bundle exec rubocop
```

Also confirm the grammar tokenizes with `vscode-textmate`:

```sh
npm install vscode-textmate vscode-oniguruma
node -e "/* load syntaxes/bender.tmLanguage.json and tokenize ../samples/*.ben */"
```

## 6. Open the PR

Use the Linguist pull-request template and fill every section. Include links to
GitHub search results showing `.ben` files in the wild, and link directly to
the original sources of the samples.

## Acceptance gate

Linguist's contributor guide is explicit:

> We will only add new extensions once they have sufficient usage on GitHub.

A new `.ben` extension is likely below that bar today. The practical sequence
is:

1. Publish the grammar and editor extension.
2. Publish real Bender projects and examples using `.ben`.
3. Re-open the Linguist PR with public usage evidence.

Until then the grammar still works locally in any TextMate-compatible editor
and in VS Code through `package.json`.
