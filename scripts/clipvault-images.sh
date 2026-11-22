#!/bin/bash
set -o pipefail

self=$(realpath -- "$0")
if [ -z "${CLIPVAULT_IMAGE_CACHE:-}" ]; then
    CLIPVAULT_IMAGE_CACHE=$(mktemp -d "${TMPDIR:-/tmp}/clipvault-images.XXXXXX") || exit 1
    export CLIPVAULT_IMAGE_CACHE
    trap 'rm -rf -- "$CLIPVAULT_IMAGE_CACHE"' EXIT
fi

# Decoding is on demand; each browser session owns its temporary preview files.
image_path() {
    local id="$1" temp
    [[ $id =~ ^[0-9]+$ ]] || return 1
    if [ ! -f "$CLIPVAULT_IMAGE_CACHE/$id" ]; then
        temp=$(mktemp "$CLIPVAULT_IMAGE_CACHE/$id.XXXXXX") || return 1
        if ! clipvault get "$id" > "$temp"; then
            rm -f -- "$temp"
            return 1
        fi
        mv -f -- "$temp" "$CLIPVAULT_IMAGE_CACHE/$id" || return 1
    fi
    printf '%s' "$CLIPVAULT_IMAGE_CACHE/$id"
}

image_ext() {
    local mime ext
    mime=$(file --brief --mime-type -- "$1") || return 1
    ext=${mime#image/}
    ext=${ext%%+*}
    case "$ext" in jpeg) ext=jpg ;; ""|*/*) ext=png ;; esac
    printf '%s' "$ext"
}

list_images() {
    # Binary previews contain the MIME and dimensions. Keep them intact even
    # when CLIPVAULT_MAX_PREVIEW_WIDTH is set lower for text previews.
    clipvault list --fields id,size,last-updated,preview --max-preview-width 100 | awk -F '\t' -v now="$(date +%s)" '
        $4 ~ /^\[\[ binary data [^ ]+ image\// {
            description = $4
            sub(/^\[\[ binary data /, "", description)
            sub(/ \]\]$/, "", description)
            split(description, parts, " ")
            ext = parts[2]
            sub(/^image\//, "", ext)
            sub(/\+.*/, "", ext)
            dims = parts[3] ~ /^[0-9]+x[0-9]+$/ ? parts[3] : "?"
            ago = "?"
            if ($3 ~ /^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$/) {
                stamp = $3
                gsub(/[-:TZ]/, " ", stamp)
                copied = mktime(stamp, 1)
                if (copied >= 0) {
                    age = now - copied
                    if (age < 60) ago = "now"
                    else if (age < 3600) ago = int(age / 60) "m ago"
                    else if (age < 86400) ago = int(age / 3600) "h ago"
                    else ago = int(age / 86400) "d ago"
                }
            }
            printf "%s\t%-10s %-14s %s\n", $1, dims, $2 " " ext, ago
        }'
}

# Viewers and drag windows get independent files so closing fzf cannot remove
# their data. An opener can return before the viewer has read its file.
export_images() {
    local id path ext exported_path index=0
    exported_dir=$(mktemp -d "$1/clipvault-export.XXXXXX") || return 1
    shift
    exported_files=()
    for id in "$@"; do
        path=$(image_path "$id") && ext=$(image_ext "$path") || {
            rm -rf -- "$exported_dir"
            return 1
        }
        index=$((index + 1))
        printf -v exported_path '%s/%03d_%s.%s' "$exported_dir" "$index" "$id" "$ext"
        cp -- "$path" "$exported_path" || { rm -rf -- "$exported_dir"; return 1; }
        exported_files+=("$exported_path")
    done
}

case "${1:-}" in
    --list)
        list_images
        ;;
    --preview)
        # Kitty-protocol graphics survive fzf's normal text clearing.
        printf '\033_Ga=d,d=A,q=2;\033\\'
        path=$(image_path "$2") || exit 1
        chafa --view-size "${FZF_PREVIEW_COLUMNS}x${FZF_PREVIEW_LINES}" --scale 1 --margin-right 0 \
            "$path" 2>/dev/null && printf '\n'
        ;;
    --save)
        shift
        for id in "$@"; do
            path=$(image_path "$id") && ext=$(image_ext "$path") || exit 1
            filename="clipboard_$(date +%Y%m%d_%H%M%S_%N).$ext"
            cp -- "$path" "$HOME/Pictures/$filename" || exit 1
        done
        if [ "$#" -eq 1 ]; then msg="Saved $filename"; else msg="Saved $# images"; fi
        { [ "$(notify-send -a osd -i camera-photo \
                -t 5000 -A default="Open folder" "$msg")" = default ] \
                && xdg-open "$HOME/Pictures"; } >/dev/null 2>&1 &
        ;;
    --delete)
        shift
        for id in "$@"; do
            [[ $id =~ ^[0-9]+$ ]] || exit 1
            clipvault delete "$id" || exit 1
            rm -f -- "$CLIPVAULT_IMAGE_CACHE/$id"
        done
        ;;
    --edit)
        path=$(image_path "$2") || exit 1
        swappy -f "$path"
        ;;
    --open)
        shift
        # Successful opens keep their files for the runtime directory's life;
        # xdg-open does not report when a viewer has finished using them.
        export_images "${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}" "$@" || exit 1
        { xdg-open "${exported_files[0]}" || rm -rf -- "$exported_dir"; } >/dev/null 2>&1 &
        ;;
    --drag)
        shift
        export_images "${TMPDIR:-/tmp}" "$@" || exit 1
        { dragon-drop -a -x -i -T "${exported_files[@]}"; rm -rf -- "$exported_dir"; } >/dev/null 2>&1 &
        ;;
    *)
        selection=$(list_images | fzf \
            --preview-window=right:70% \
            --preview-label $' \033[38;2;160;160;160mTAB\033[36m select   \033[38;2;160;160;160mCTRL+S\033[36m save   \033[38;2;160;160;160mCTRL+E\033[36m edit   \033[38;2;160;160;160mCTRL+D\033[36m drag   \033[38;2;160;160;160mRIGHT\033[36m open   \033[38;2;160;160;160mDEL\033[36m delete   \033[38;2;160;160;160mENTER\033[36m copy\033[m ' \
            --preview-label-pos bottom \
            -d '\t' --with-nth 2 \
            --no-sort --multi --reverse --no-hscroll --height=100% \
            --no-input --no-input-border --highlight-line --scroll-off 7 \
            --info=right --border --border-label ' CLIPVAULT IMAGES ' \
            --info-command='printf "%s/%s(%s)" "$FZF_POS" "$FZF_MATCH_COUNT" "$FZF_SELECT_COUNT"' \
            --footer-border none --pointer '>' --gutter '┃' --marker '┃' \
            --ellipsis '  ' --scrollbar '' --separator '' \
            --color fg:242,bg:233,hl:65,fg+:222,bg+:234,hl+:108 \
            --color info:108,prompt:110,spinner:150,pointer:167,marker:65 \
            --preview "'$self' --preview {1}" \
            --bind "ctrl-s:execute-silent('$self' --save {+1})" \
            --bind "ctrl-e:execute-silent('$self' --edit {1})" \
            --bind "ctrl-d:execute-silent('$self' --drag {+1})" \
            --bind "right:execute-silent('$self' --open {+1})" \
            --bind "delete:execute-silent('$self' --delete {+1})+reload('$self' --list)") || exit 1
        [ -n "$selection" ] || exit 1
        id=${selection%%$'\t'*}
        path=$(image_path "$id") || exit 1
        mime=$(file --brief --mime-type -- "$path") || exit 1
        clip-bridge -c --type "$mime" < "$path"
        ;;
esac
