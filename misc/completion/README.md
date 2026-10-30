# Shell completions for Bender

This directory contains shell completion scripts for the `bender` interpreter.

## Bash

```sh
# Temporary (current session only)
source /path/to/bender.bash

# Permanent (system-wide)
sudo cp bender.bash /etc/bash_completion.d/bender

# Permanent (user only)
cp bender.bash ~/.local/share/bash-completion/completions/bender
```

## Zsh

```sh
# Copy to a directory in your $fpath, e.g.:
cp bender.zsh ~/.zsh/completions/_bender

# Ensure the directory is in $fpath (add to ~/.zshrc if needed):
# fpath=(~/.zsh/completions $fpath)
# autoload -Uz compinit && compinit
```

## Fish

```sh
cp bender.fish ~/.config/fish/completions/bender.fish
```
