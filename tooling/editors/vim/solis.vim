" Vim 9 or Neovim, Linux and Windows, using the yegappan/lsp plugin
" (https://github.com/yegappan/lsp). Source this file from your vimrc:
"   source /path/to/Solis/tooling/editors/vim/solis.vim
" Windows: put the same lines in ~\_vimrc. `sl` must be on PATH (sl.exe).
"
" With vim-lsp, register the server like this instead:
"   au User lsp_setup call lsp#register_server({'name': 'sl', 'cmd': ['sl', 'lsp'], 'allowlist': ['solis']})
" With coc.nvim, add to coc-settings.json:
"   "languageserver": {"sl": {"command": "sl", "args": ["lsp"], "filetypes": ["solis"], "rootPatterns": ["solis.toml"]}}

autocmd BufRead,BufNewFile *.sl setfiletype solis
autocmd FileType solis setlocal commentstring=//\ %s shiftwidth=4 expandtab

if exists('*LspAddServer')
  call LspAddServer([#{
        \ name: 'sl',
        \ filetype: ['solis'],
        \ path: 'sl',
        \ args: ['lsp'],
        \ rootSearch: ['solis.toml', '.git'],
        \ }])
endif
