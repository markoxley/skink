;;; skink-mode.el --- Major mode for editing Skink source code -*- lexical-binding: t; -*-

;; File:    skink-mode.el
;; Purpose: Emacs major mode for the Skink programming language.
;;          Provides syntax highlighting, // comment handling, and
;;          brace-based indentation.

;;; Code:

(defgroup skink nil
  "Editing support for the Skink programming language."
  :group 'languages)

(defconst skink--keywords
  '("module" "import" "pub" "fn" "extern" "type" "struct" "const" "var"
    "interface" "impl" "enum" "union" "defer" "spawn" "go" "in" "range"
    "if" "else" "switch" "case" "default" "match"
    "for" "break" "continue" "return" "panic" "nil")
  "Skink keywords.")

(defconst skink--types
  '("int" "int64" "int32" "int16" "int8" "uint64"
    "float" "float64" "string" "bool" "byte" "void")
  "Skink builtin types.")

(defconst skink--constants
  '("true" "false" "nil")
  "Skink constants.")

(defvar skink-font-lock-keywords
  `((,(regexp-opt skink--keywords 'words) . font-lock-keyword-face)
    (,(regexp-opt skink--types 'words) . font-lock-type-face)
    (,(regexp-opt skink--constants 'words) . font-lock-constant-face)
    ;; {expr} interpolation inside strings
    ("{[^}\n]*}" . font-lock-variable-name-face)
    ;; field/type attributes like [db(pk,auto)]
    ("\\[[a-zA-Z_][a-zA-Z0-9_]*([^]\n]*)\\]" . font-lock-preprocessor-face)
    ;; declarations: fn names, type names after struct/interface/enum
    ("\\<fn\\s-+\\([a-zA-Z_][a-zA-Z0-9_]*\\)" 1 font-lock-function-name-face)
    ("\\<\\(?:struct\\|interface\\|enum\\|type\\)\\s-+\\([a-zA-Z_][a-zA-Z0-9_]*\\)" 1 font-lock-type-face))
  "Font-lock rules for `skink-mode'.")

(defvar skink-mode-syntax-table
  (let ((st (make-syntax-table)))
    ;; // line comments (like C++)
    (modify-syntax-entry ?/ ". 124b" st)
    (modify-syntax-entry ?\n "> b" st)
    ;; strings
    (modify-syntax-entry ?\" "\"" st)
    ;; { } are parentheses for interpolation/matching
    (modify-syntax-entry ?_ "w" st)
    st)
  "Syntax table for `skink-mode'.")

(defcustom skink-indent-offset 4
  "Indentation step width in `skink-mode'."
  :type 'integer
  :group 'skink)

(defun skink--indent-line ()
  "Indent the current line as Skink code.
An opening {, ( or [ at the end of the previous nonblank line adds a
level; a leading closer or `case'/`default' label removes one."
  (let ((target 0))
    (save-excursion
      (catch 'done
        (while (= (forward-line -1) 0)
          (back-to-indentation)
          (unless (looking-at-p "\\s-*$\\|\\s-*//")
            (let ((base (current-indentation)))
              (end-of-line)
              (setq target
                    (+ base (if (re-search-backward "[{(\[]\\s-*\\(//.*\\)?$"
                                                  (line-beginning-position) t)
                                skink-indent-offset
                              0))))
            (throw 'done nil)))))
    (save-excursion
      (back-to-indentation)
      (when (looking-at-p "[]})]\\|\\(case\\|default\\)\\_>.*:")
        (setq target (max 0 (- target skink-indent-offset)))))
    (indent-line-to target)))

(defun skink-indent-line ()
  "Indent the current line in `skink-mode'."
  (interactive)
  (skink--indent-line))

;;;###autoload
(define-derived-mode skink-mode prog-mode "Skink"
  "Major mode for editing Skink source files."
  :syntax-table skink-mode-syntax-table
  (setq-local comment-start "//"
              comment-end ""
              comment-start-skip "//+ *"
              font-lock-defaults '(skink-font-lock-keywords)
              indent-line-function #'skink-indent-line
              indent-tabs-mode nil
              tab-width skink-indent-offset))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.skink\\'" . skink-mode))

(provide 'skink-mode)
;;; skink-mode.el ends here
