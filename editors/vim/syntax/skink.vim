" File:    syntax/skink.vim
" Purpose: Syntax highlighting for the Skink programming language.
"          Covers // comments, interpolated strings, keywords, builtin
"          types, numeric literals, and the :=/? operators.

if exists("b:current_syntax")
    finish
endif

" Line comments
syn keyword skinkTodo        contained TODO FIXME XXX NOTE BUG HACK
syn match   skinkComment     "//.*$" contains=skinkTodo

" Strings — double-quoted, with {expr} interpolation highlighted
syn region  skinkString      start='"' skip='\\"' end='"' contains=skinkInterp,skinkEscape
syn region  skinkInterp      contained start='{' end='}' contains=ALLBUT,skinkComment
syn match   skinkEscape      contained '\\["\\nrt0xu]'

" Keywords
syn keyword skinkKeyword     module import pub fn extern type struct const var
syn keyword skinkKeyword     interface impl enum union defer spawn go in
syn keyword skinkKeyword     nil
syn keyword skinkConditional if else switch case default match
syn keyword skinkRepeat      for break continue range
syn keyword skinkStatement   return panic defer
syn keyword skinkBoolean     true false

" Builtin types
syn keyword skinkType        int int64 int32 int16 int8 uint64 float float64
syn keyword skinkType        string bool byte void

" Numbers
syn match   skinkNumber      '\<\d[0-9_]*\>'
syn match   skinkNumber      '\<0x[0-9a-fA-F_]*\>'
syn match   skinkNumber      '\<0b[01_]*\>'
syn match   skinkFloat       '\<\d[0-9_]*\.\d[0-9_]*\([eE][-+]\?\d\+\)\?\>'
syn match   skinkFloat       '\<\d[0-9_]*[eE][-+]\?\d\+\>'

" Operators: := declaration, ? error propagation, [^i] from-end index
syn match   skinkOperator    ':='
syn match   skinkOperator    '?'
syn match   skinkOperator    '\[^'

" Field/type attributes like [db(pk,auto)]
syn match   skinkAttribute   '\[[a-zA-Z_][a-zA-Z0-9_]*([^]]*)\]'

hi def link skinkComment     Comment
hi def link skinkTodo        Todo
hi def link skinkString      String
hi def link skinkEscape      SpecialChar
hi def link skinkInterp      Identifier
hi def link skinkKeyword     Keyword
hi def link skinkConditional Conditional
hi def link skinkRepeat      Repeat
hi def link skinkStatement   Statement
hi def link skinkBoolean     Boolean
hi def link skinkType        Type
hi def link skinkNumber      Number
hi def link skinkFloat       Float
hi def link skinkOperator    Operator
hi def link skinkAttribute   PreProc

let b:current_syntax = "skink"
