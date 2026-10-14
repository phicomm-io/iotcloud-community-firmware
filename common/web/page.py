from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
def render_page(path):
    path = Path(path)
    text = path.read_text()
    for name in ("style.css", "diagnostics.js", "wifi.html", "mqtt.html", "auth.html"):
        content = (ROOT / "common/web" / name).read_text()
        for marker in ("/* @include " + name + " */", "<!-- @include " + name + " -->"):
            text = text.replace(marker, content)
    product = path.resolve().relative_to(ROOT).parts[0]
    text = text.replace("{{mqtt_host_max}}", "31" if product == "TC1" else "127")
    return text.encode("utf-8")
if __name__ == "__main__":
    import sys
    sys.stdout.buffer.write(render_page(sys.argv[1]))
