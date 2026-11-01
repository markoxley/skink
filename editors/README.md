# Skink editor plugins

Editor support for the Skink programming language.

## Vim / Neovim

Files under `vim/`:

- `ftdetect/skink.vim` — sets `filetype=skink` for `*.skink`
- `syntax/skink.vim` — highlighting (comments, interpolated strings, keywords, types, `:=`/`?`/`[^` operators)
- `indent/skink.vim` — brace-based indentation with case/default handling

### Install (vim)

Copy or symlink the `vim/` tree into `~/.vim/`, or with a plugin manager:

```vim
" vim-plug — point at the repo
Plug 'markoxley/skink', { 'rtp': 'editors/vim' }
```

For pathogen/native packages:

```sh
mkdir -p ~/.vim/pack/langs/start
ln -s /path/to/skink/editors/vim ~/.vim/pack/langs/start/skink
```

## Emacs

`emacs/skink-mode.el` — a `prog-mode`-derived major mode with
font-lock highlighting, `//` comments, and indentation.

### Install (emacs)

```elisp
(add-to-list 'load-path "/path/to/skink/editors/emacs")
(require 'skink-mode)  ; .skink files auto-load the mode
```

Or with `use-package`:

```elisp
(use-package skink-mode
  :load-path "/path/to/skink/editors/emacs"
  :mode "\\.skink\\'")
```
