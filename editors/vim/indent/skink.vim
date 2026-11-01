" File:    indent/skink.vim
" Purpose: Simple brace-based indentation for Skink — behaves like
"          cindent for {}, (), [] blocks, comments, and switch cases.

if exists("b:did_indent")
    finish
endif
let b:did_indent = 1

setlocal indentexpr=GetSkinkIndent(v:lnum)
setlocal indentkeys=0{,0},0),0],!^F,o,O,0=case,0=default,0=else,0=for,0=if
setlocal comments=:// commentstring=//\ %s

" Only define once
if exists("*GetSkinkIndent")
    finish
endif

function! GetSkinkIndent(lnum) abort
    let l:prev = prevnonblank(a:lnum - 1)
    if l:prev == 0
        return 0
    endif
    let l:pline = getline(l:prev)
    let l:cline = getline(a:lnum)

    " Previous line opened a block: increase indent
    let l:ind = indent(l:prev)
    if l:pline =~# '[{(\[]\s*\(//.*\)\?$'
        let l:ind += &shiftwidth
    endif
    " case:/default: in a switch adds a level
    if l:pline =~# '^\s*\(case\|default\)\b.*:'
        let l:ind += &shiftwidth
    endif
    " Current line closing a block or a new case: dedent
    if l:cline =~# '^\s*[})\]]'
        let l:ind -= &shiftwidth
    endif
    if l:cline =~# '^\s*\(case\|default\)\b.*:'
        let l:ind -= &shiftwidth
    endif
    return l:ind < 0 ? 0 : l:ind
endfunction
