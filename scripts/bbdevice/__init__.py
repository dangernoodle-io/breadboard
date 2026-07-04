"""bbdevice — device HTTP/WS test-and-ops CLI framework (skeleton).

Sibling package to scripts/bbtool/, sharing its plugin-bus CLI pattern
(registry.py COMMANDS/RULES + PluginAPI, core.py load_config/load_plugins,
cli.py argparse dispatcher). This package currently ships the framework
shape only; fleet's generic device-testing layer lands in a later PR.
"""
