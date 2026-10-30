# Fish completion for bender

# Disable file completion by default; re-enable for specific flags
complete -c bender -f

# Flags
complete -c bender -s h -l help -d 'Show usage and exit'
complete -c bender -s v -l version -d 'Show version and exit'
complete -c bender -s s -l stdin -d 'Read raw stdin (no readline features)'
complete -c bender -s e -l eval -r -d 'Execute Bender source string'
complete -c bender -s E -l expression -r -d 'Execute Bender expression as short-fn with remaining args'
complete -c bender -s d -l debug -d 'Enable debug mode'
complete -c bender -s n -l nocolor -d 'Disable ANSI colors in REPL'
complete -c bender -s N -l color -d 'Enable ANSI colors in REPL'
complete -c bender -s r -l repl -d 'Open REPL after executing sources'
complete -c bender -s R -l noprofile -d 'Disable loading user profile in REPL'
complete -c bender -s p -l persistent -d 'Persistent mode (continue after errors)'
complete -c bender -s q -l quiet -d 'Hide logo in REPL'
complete -c bender -s k -l flycheck -d 'Compile only (lint), do not execute'
complete -c bender -s i -l image -d 'Treat script as a .benx file'
complete -c bender -s m -l syspath -r -d 'Set syspath for module loading' -a '(__fish_complete_directories)'
complete -c bender -s c -l compile -r -d 'Precompile source to .benx'
complete -c bender -s l -l library -r -d 'Import module before script/REPL' -a '(find . -name "*.ben" -printf "%f\n" 2>/dev/null)'
complete -c bender -s w -l lint-warn -r -d 'Set warning linting level' -a ':none :relaxed :normal :strict'
complete -c bender -s x -l lint-error -r -d 'Set error linting level' -a ':none :relaxed :normal :strict'

# File arguments: .ben files and directories
complete -c bender -F -a '*.ben'
