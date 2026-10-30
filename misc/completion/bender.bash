# Bash completion for bender
# Install: source this file, or place it in /etc/bash_completion.d/bender
# or ~/.local/share/bash-completion/completions/bender

_bender() {
    local cur prev words cword
    _init_completion || return

    local flags="-h --help -v --version -s --stdin -e --eval -E --expression -d --debug -n --nocolor -N --color -r --repl -R --noprofile -p --persistent -q --quiet -k --flycheck -m --syspath -c --compile -i --image -l --library -w --lint-warn -x --lint-error --"

    case "$prev" in
        -e|--eval|-E|--expression)
            # Argument is Bender source code — no file completion
            return
            ;;
        -m|--syspath)
            # syspath: complete directories
            _filedir -d
            return
            ;;
        -c|--compile|-l|--library)
            # source file: complete .ben files
            _filedir bender
            return
            ;;
        -w|--lint-warn|-x|--lint-error)
            # linting level
            COMPREPLY=($(compgen -W ":none :relaxed :normal :strict" -- "$cur"))
            return
            ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=($(compgen -W "$flags" -- "$cur"))
        return
    fi

    # Default: complete .ben files and directories
    _filedir bender
}

complete -F _bender bender
