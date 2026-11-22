#!/bin/bash
set -o pipefail

while :; do
    selection=$(clipvault list --fields id,preview |
        awk -F '\t' '$2 !~ /^\[\[ binary data /' |
        rofi -dmenu -multi-select -no-custom -format s \
        -display-columns 2 \
        -display-column-separator $'\t' \
        -matching normal \
        -p ' CLIPVAULT HISTORY ' \
        -ballot-selected-str '┃' \
        -ballot-unselected-str ' ' \
        -kb-row-select "" \
        -kb-move-front "" \
        -kb-element-next "" \
        -kb-accept-alt "Tab" \
        -kb-remove-char-forward "Control+d" \
        -kb-custom-1 "Delete" \
        -kb-custom-2 "!Control+space" \
        -kb-custom-3 "!Control+comma" \
        -kb-custom-4 "!Control+Alt+comma" \
        -kb-custom-5 "!Control+a" \
        -kb-custom-6 "!Control+Alt+a" \
        -kb-custom-7 "!Control+Shift+a" \
        -kb-custom-8 "!Control+o" \
        -kb-custom-9 "!Control+Alt+o" \
        -kb-custom-10 "!Control+Shift+o" \
        -kb-custom-11 "!Control+slash" \
        -kb-custom-12 "!Control+backslash" \
        -kb-custom-13 "!Control+semicolon")
    ret=$?
    [ -z "$selection" ] && exit 1
    case "$ret" in
        0) sep=$'\n' ;;
        10)
            while IFS= read -r row; do
                clipvault delete "$row" || exit 1
            done <<< "$selection"
            continue ;;
        11) sep=' ' ;;
        12) sep=',' ;;
        13) sep=', ' ;;
        14) sep=' and ' ;;
        15) sep=' & ' ;;
        16) sep=' && ' ;;
        17) sep=' or ' ;;
        18) sep=' | ' ;;
        19) sep=' || ' ;;
        20) sep='/' ;;
        21) sep=$'\\\n' ;;
        22) sep='; ' ;;
        *) exit 1 ;;
    esac
    break
done

combined=""
join=""
while IFS= read -r row; do
    # The sentinel preserves trailing newlines through command substitution.
    entry=$(clipvault get "$row" && printf '\001') || exit 1
    entry=${entry%$'\001'}
    combined+="$join$entry"
    join=$sep
done <<< "$selection"
printf '%s' "$combined" | clip-bridge -cp --type "${1:-text/plain;charset=utf-8}"
