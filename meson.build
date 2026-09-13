project('siml', 'c',
  version: '0.1.0',
  default_options: ['c_std=c89', 'warning_level=2']
)

siml_tool = executable('siml-tool', 'siml-tool.c')

# integration tests
py = import('python').find_installation()
test(
  'integration',
  py,
  args: files('tests.py'),
  depends: [siml_tool],
)
