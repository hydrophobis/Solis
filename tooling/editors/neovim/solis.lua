-- Neovim 0.8 or newer, Linux and Windows.
--
-- Put this file on the runtimepath and require it from init.lua, for example
--   ~/.config/nvim/lua/solis.lua           (Windows: %LOCALAPPDATA%\nvim\lua\solis.lua)
--   require('solis')
-- `sl` must be on PATH (sl.exe on Windows), or set `cmd` below.
--
-- Pairs with nothing else: no plugin is needed, it uses the built-in client.

local cmd = { 'sl', 'lsp' }

vim.filetype.add({ extension = { sl = 'solis' } })

vim.api.nvim_create_autocmd('FileType', {
  pattern = 'solis',
  callback = function(args)
    vim.bo[args.buf].commentstring = '// %s'

    -- the project root is the directory holding solis.toml
    local root = vim.fs.root and vim.fs.root(args.buf, { 'solis.toml', '.git' })
      or vim.fs.dirname(vim.fs.find({ 'solis.toml', '.git' }, { upward = true, path = vim.api.nvim_buf_get_name(args.buf) })[1])

    vim.lsp.start({
      name = 'sl',
      cmd = cmd,
      root_dir = root or vim.fn.getcwd(),
    })
  end,
})

-- Optional keys, set on attach. Delete what you do not want.
vim.api.nvim_create_autocmd('LspAttach', {
  callback = function(args)
    local client = vim.lsp.get_client_by_id(args.data.client_id)
    if not client or client.name ~= 'sl' then return end
    local o = { buffer = args.buf }
    vim.keymap.set('n', 'gd', vim.lsp.buf.definition, o)
    vim.keymap.set('n', 'gr', vim.lsp.buf.references, o)
    vim.keymap.set('n', 'K', vim.lsp.buf.hover, o)
    vim.keymap.set('n', '<leader>o', vim.lsp.buf.document_symbol, o)
  end,
})
