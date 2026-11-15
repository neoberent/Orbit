# fish completions for orbit (Orbit Download Manager)

set -l commands add list ls status pause resume cancel remove rm show help

function __orbit_download_ids
    # "ID  STATE ..." rows from `orbit list`; offered as "id<TAB>state  name".
    orbit list 2>/dev/null | string match -rv '^(ID |No downloads)' | string replace -r '^(\S+)\s+(\S+)\s+\S+\s+\S+(?:/s)?\s+\S+(?: \S+)?\s+(.*)$' '$1\t$2  $3'
end

complete -c orbit -f
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -a add -d 'Add downloads'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -a list -d 'Show every download'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -a status -d 'Totals and current speeds'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -a pause -d 'Pause downloads'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -a resume -d 'Resume downloads'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -a cancel -d 'Cancel downloads'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -a remove -d 'Remove downloads from the list'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -a show -d 'Open the window'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -a help -d 'Show help'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -l background -d 'Start in the tray'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -l headless -d 'Run without window or tray'
complete -c orbit -n "not __fish_seen_subcommand_from $commands" -l version -d 'Print the version'

complete -c orbit -n '__fish_seen_subcommand_from add' -F
complete -c orbit -n '__fish_seen_subcommand_from add' -s d -l dir -x -a '(__fish_complete_directories)' -d 'Save in this folder'
complete -c orbit -n '__fish_seen_subcommand_from add' -s i -l input -r -F -d 'Add every link in this file'
complete -c orbit -n '__fish_seen_subcommand_from add' -l checksum -x -d 'sha256:<hex> to verify'
complete -c orbit -n '__fish_seen_subcommand_from add' -s f -l force -d 'Add even if downloaded before'
complete -c orbit -n '__fish_seen_subcommand_from add' -l mirror -x -d 'Another link to the same file'
complete -c orbit -n '__fish_seen_subcommand_from pause resume cancel remove rm' -x -a '(__orbit_download_ids)'
complete -c orbit -n '__fish_seen_subcommand_from pause resume cancel' -s a -l all -d 'Every download'
complete -c orbit -n '__fish_seen_subcommand_from add list ls status' -l json -d 'Print JSON'
