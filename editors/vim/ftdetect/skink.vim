" File:    ftdetect/skink.vim
" Purpose: Register the .skink extension as Skink source for vim's
"          filetype detection (part of the skink vim plugin).
augroup skink_ftdetect
    autocmd!
    autocmd BufRead,BufNewFile *.skink setfiletype skink
augroup END
