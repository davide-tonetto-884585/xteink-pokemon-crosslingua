#!/usr/bin/env python3
"""Host regression tests for the production SAX detector (requires C++20 and Expat).

Optionally pass an EPUB to compare every spine chapter with its language markup.
Book contents are extracted only into a temporary directory.
"""
import pathlib
import posixpath
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
CASES = [
    ("<p>Original</p><p lang='uk'>Translation</p>", True, False),
    ("<p>Original<br/><span lang='uk'>Translation</span></p>", True, False),
    ("<p>Original<br/> \n<em><span xml:lang='uk-UA'>Translation</span></em></p>", True, False),
    ("<p>Original <span lang='uk'>foreign word</span></p>", False, False),
    ("<p>Original<br/>More original <span lang='uk'>word</span></p>", False, False),
    ("<p>Original<br/></p><p>Text <span lang='uk'>word</span></p>", False, False),
    ("<p>Original<br/><span lang='en-GB'>Same language</span></p>", False, False),
    ("<img src='cover.jpg' alt='Cover'/>", False, True),
    ("<svg><image href='map.jpg'/></svg>", False, True),
    ("<img src='cover.jpg'/><p>Caption</p>", False, False),
    ("<style>body {color:black}</style><script>code</script>", False, True),
    ("<p>Привіт</p>", False, False),
    (" " * 1600 + "<p>Text<br/><span lang='uk'>Translation</span></p>", True, False),
]

with tempfile.TemporaryDirectory(prefix="lingua-detector-") as tmp:
    tmp = pathlib.Path(tmp)
    (tmp / "Logging.h").write_text("#pragma once\n#define LOG_ERR(...)\n#define LOG_DBG(...)\n")
    (tmp / "HalStorage.h").write_text(r'''#pragma once
#include <fstream>
#include <cstdint>
class HalFile {
 public:
  std::ifstream stream;
  size_t size() {
    const auto position = stream.tellg();
    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    stream.seekg(position);
    return length;
  }
  int read(uint8_t* data, size_t count) {
    stream.read(reinterpret_cast<char*>(data), count);
    return static_cast<int>(stream.gcount());
  }
};
struct StorageStub {
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    file.stream.open(path);
    return file.stream.good();
  }
};
inline StorageStub Storage;
''')
    (tmp / "scan.cpp").write_text(r'''#include "modules/lingua/services/TranslatedContentDetector.h"
#include <iostream>
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  bool textless = false;
  const bool translated = lingua::content::htmlHasTranslatedBlock(argv[1], argv[2], &textless);
  std::cout << translated << " " << textless;
}
''')
    binary = tmp / "scan"
    subprocess.run([
        "c++", "-std=c++20", "-I" + str(tmp), "-I" + str(ROOT / "src"),
        "-I" + str(ROOT / "lib/Epub"), "-I" + str(ROOT / "lib/Memory"),
        str(tmp / "scan.cpp"),
        str(ROOT / "src/modules/lingua/services/TranslatedContentDetector.cpp"),
        "-lexpat", "-o", str(binary),
    ], check=True)

    def scan(path, language="en"):
        output = subprocess.check_output([str(binary), str(path), language], text=True)
        return tuple(value == "1" for value in output.split())

    fixture = tmp / "chapter.html"
    for body, translated, textless in CASES:
        fixture.write_text("<html><head><title>Title</title></head><body>" + body + "</body></html>")
        assert scan(fixture) == (translated, textless), body
    fixture.write_text("<html><body><img/>")
    assert scan(fixture) == (False, False), "Malformed HTML must not be classified as textless"
    assert scan(tmp / "missing.html") == (False, False)
    fixture.write_text("<html><body><img/></body></html>")
    assert scan(fixture) == (False, True), "An <img>-only body is textless"
    # Unknown book language answers before opening the file, so it reports neither flag.
    assert scan(fixture, "") == (False, False)
    print(f"{len(CASES) + 4} detector regression cases passed")

    if len(sys.argv) > 1:
        with zipfile.ZipFile(sys.argv[1]) as book:
            container = ET.fromstring(book.read("META-INF/container.xml"))
            opf = next(e.attrib["full-path"] for e in container.iter() if e.tag.endswith("rootfile"))
            package = ET.fromstring(book.read(opf))
            language = package.find(".//{http://purl.org/dc/elements/1.1/}language").text
            manifest = {e.attrib["id"]: e.attrib["href"] for e in package.iter() if e.tag.endswith("}item")}
            count = 0
            for item in package.iter():
                if not item.tag.endswith("}itemref"):
                    continue
                path = posixpath.join(posixpath.dirname(opf), manifest[item.attrib["idref"]])
                content = book.read(path)
                root = ET.fromstring(content)
                expected = any(
                    (e.get("lang") or e.get("{http://www.w3.org/XML/1998/namespace}lang") or "en").lower().split("-")[0]
                    != language.lower().split("-")[0]
                    for e in root.iter() if e.tag.rsplit("}", 1)[-1] not in ("html", "body")
                )
                fixture.write_bytes(content)
                assert scan(fixture, language)[0] == expected, path
                count += 1
            print(f"{count} EPUB spine chapters matched their translation markup")
