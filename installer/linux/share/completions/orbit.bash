# bash completion for orbit (Orbit Download Manager)

_orbit() {
    local cur prev cmd
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}
    cmd=${COMP_WORDS[1]}

    if [ "$COMP_CWORD" -eq 1 ]; then
        COMPREPLY=($(compgen -W "add list ls status pause resume cancel remove rm show feeds help --background --headless --version" -- "$cur"))
        return
    fi
    case $cmd in
        add)
            case $prev in
                -d|--dir) COMPREPLY=($(compgen -d -- "$cur")); return ;;
                -i|--input) COMPREPLY=($(compgen -f -- "$cur")); return ;;
                --checksum|--mirror) return ;;
            esac
            if [[ $cur == -* ]]; then
                COMPREPLY=($(compgen -W "--dir --input --checksum --force --mirror --json" -- "$cur"))
            else
                COMPREPLY=($(compgen -f -X '!*.torrent' -- "$cur") $(compgen -d -- "$cur"))
            fi
            ;;
        pause|resume|cancel|remove|rm)
            local ids
            ids=$(orbit list 2>/dev/null | awk 'NR > 1 && $1 ~ /^[0-9]+$/ {print $1}')
            COMPREPLY=($(compgen -W "$ids --all" -- "$cur"))
            ;;
        feeds)
            COMPREPLY=($(compgen -W "add remove check --filter --dir --json" -- "$cur"))
            ;;
        list|ls|status)
            COMPREPLY=($(compgen -W "--json" -- "$cur"))
            ;;
    esac
}
complete -o filenames -F _orbit orbit
