;;; solis-mode.el --- Solis major mode with eglot support  -*- lexical-binding: t; -*-

;; Emacs 29 or newer (eglot is built in), Linux and Windows.
;;
;; Install: copy this file somewhere on `load-path' and add to your init file
;;   (require 'solis-mode)
;; `sl' must be on PATH (sl.exe on Windows), or set `solis-server-command'.
;; Then M-x eglot in a .sl buffer, or enable it for every Solis buffer:
;;   (add-hook 'solis-mode-hook #'eglot-ensure)

;;; Code:

(defgroup solis nil "Solis language support." :group 'languages)

(defcustom solis-server-command '("sl" "lsp")
  "Command that starts the Solis language server."
  :type '(repeat string))

(defvar solis-mode-syntax-table
  (let ((st (make-syntax-table)))
    (modify-syntax-entry ?_ "_" st)
    (modify-syntax-entry ?\" "\"" st)
    (modify-syntax-entry ?/ ". 124b" st)
    (modify-syntax-entry ?* ". 23n" st)
    (modify-syntax-entry ?\n "> b" st)
    st))

(defconst solis-keywords
  '("func" "let" "var" "if" "else" "while" "for" "in" "switch" "case" "default"
    "return" "break" "continue" "struct" "enum" "interface" "static" "extern"
    "import" "arena" "weak" "mut" "this"))

(defvar solis-font-lock-keywords
  `((,(regexp-opt solis-keywords 'symbols) . font-lock-keyword-face)
    (,(regexp-opt '("true" "false" "null") 'symbols) . font-lock-constant-face)
    (,(regexp-opt '("int" "float" "bool" "str" "void") 'symbols) . font-lock-type-face)
    ("\\_<\\(?:func\\)\\s-+\\([A-Za-z_][A-Za-z0-9_]*\\)" 1 font-lock-function-name-face)
    ("\\_<\\(?:struct\\|enum\\|interface\\)\\s-+\\([A-Za-z_][A-Za-z0-9_]*\\)" 1 font-lock-type-face)))

;;;###autoload
(define-derived-mode solis-mode prog-mode "Solis"
  "Major mode for Solis source."
  :syntax-table solis-mode-syntax-table
  (setq-local comment-start "// ")
  (setq-local comment-end "")
  (setq-local indent-tabs-mode nil)
  (setq-local tab-width 4)
  (setq-local font-lock-defaults '(solis-font-lock-keywords)))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.sl\\'" . solis-mode))

(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               `(solis-mode . ,(lambda (&rest _) solis-server-command))))

(provide 'solis-mode)
;;; solis-mode.el ends here
