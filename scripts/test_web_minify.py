"""Check generated portal scripts and inline event-handler names after minification."""
import gzip
from html.parser import HTMLParser
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.in_script = False
        self.scripts = []
        self.handlers = []
    def handle_starttag(self, tag, attrs):
        self.in_script = tag == "script" or self.in_script
        for name, value in attrs:
            if name.startswith("on") and value: self.handlers.append(value)
    def handle_endtag(self, tag):
        if tag == "script": self.in_script = False
    def handle_data(self, data):
        if self.in_script: self.scripts.append(data)

class WebMinifyTest(unittest.TestCase):
    def test_scripts_parse_and_inline_handlers_keep_names(self):
        checked = 0
        for path in (ROOT / "src/network/html").glob("*.generated.h"):
            text = path.read_text()
            if "CompressedSize" not in text: continue
            data = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", text))
            html = gzip.decompress(data).decode()
            if "<html" not in html: continue
            page = Page()
            page.feed(html)
            scripts = "\n".join(page.scripts)
            subprocess.run(["node", "--check"], input=scripts, text=True, check=True, capture_output=True)
            names = set(re.findall(r"\bfunction\s+([\w$]+)\s*\(", scripts))
            # Include handlers in HTML strings built by JavaScript as well.
            handlers = page.handlers + re.findall(r'\bon\w+=["\']([^"\']+)', scripts)
            for handler in handlers:
                for call in re.findall(r"(?<![\w.$])([A-Za-z_$][\w$]*)\s*\(", handler):
                    if call in {"if", "confirm", "alert", "parseInt", "parseFloat", "Number", "String"}: continue
                    self.assertIn(call, names, (path.name, handler))
            checked += 1
        self.assertEqual(checked, len(list((ROOT / "web/pages").glob("*.html"))))

if __name__ == "__main__": unittest.main()
