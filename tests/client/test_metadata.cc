// JPX association semantics and JPIP metadata partitioning, independent of
// server generation. The normal hv_merge layout is flat asoc(nlst, xml).
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "hvc_cache.h"
#include "hvc_metadata.h"
#include "jpeg2000/hv_rules.h"

using Bytes = std::vector<uint8_t>;
static void check(bool ok, const std::string &message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static Bytes operator+(Bytes a, const Bytes &b) {
    a.insert(a.end(), b.begin(), b.end()); return a;
}
static Bytes box(const char *type, const Bytes &contents) {
    Bytes out;
    for (int n = 4; n--;) out.push_back(static_cast<uint8_t>((contents.size() + 8) >> (8 * n)));
    out.insert(out.end(), type, type + 4);
    return out + contents;
}
static Bytes list(uint8_t frame) { return box("nlst", {1, 0, 0, frame}); }
static Bytes xml(char value) { return box("xml ", {static_cast<uint8_t>(value)}); }
static Bytes association(uint8_t frame, char value) { return box("asoc", list(frame) + xml(value)); }
static Bytes placeholder(uint8_t id, const char *type) {
    Bytes contents = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, id, 0, 0, 0, 8};
    contents.insert(contents.end(), type, type + 4);
    return box("phld", contents);
}
struct Fixture {
    hvc_cache cache;
    hv_metadata metadata = {};
    char error[256] = "";
    Fixture(const Bytes &root, size_t codestream_count = 4) {
        hvc_cache_begin(&cache);
        Bytes bytes;
        for (size_t i = 0; i < codestream_count; i++) bytes = bytes + box("phld", {0, 0, 0, 4});
        add(0, bytes + root);
    }
    ~Fixture() { hv_metadata_close(&metadata); hvc_cache_release(&cache); }
    void add(uint8_t id, const Bytes &bytes, bool complete = true) {
        hvc_jpp_message message = {};
        message.bin_class = HVC_BIN_META_DATA; message.bin_id = id;
        message.data = bytes.data(); message.length = bytes.size(); message.last_byte = complete;
        check(hvc_cache_apply(&cache, &message), hvc_cache_error(&cache));
    }
    void expect(const std::string &expected) {
        check(hvc_metadata_open(&cache, &metadata, error, sizeof error) == 0, error);
        check(metadata.codestream_count == expected.size(), "frame count");
        for (size_t i = 0; i < expected.size(); i++) {
            const uint8_t *data = NULL; size_t size = 0;
            check(hv_metadata_xml(&metadata, i, &data, &size, error, sizeof error) == 0, error);
            check(expected[i] == '-' ? data == NULL : data != NULL && size == 1 && data[0] == expected[i],
                  "XML of frame " + std::to_string(i) + " expected " + expected[i]);
        }
    }
    void reject(const std::string &reason) {
        check(hvc_metadata_open(&cache, &metadata, error, sizeof error) == -1 &&
              std::string(error).find(reason) != std::string::npos, "expected " + reason + ": " + error);
        check(metadata.frames == NULL && metadata.codestream_count == 0, "failure retained index");
    }
};
int main() {
    const Bytes flat = association(1, 'b') + association(0, 'a') + association(0, 'z') +
                       box("asoc", box("nlst", {2, 0, 0, 3}) + xml('d'));
    { Fixture f(xml('f') + flat); f.expect("abfd"); }
    { Fixture f(xml('f') + box("grp ", box("grp ", flat))); f.expect("abfd"); }
    { Fixture f(box("asoc", flat) + xml('f')); f.expect("abfd"); }
    { Fixture f(placeholder(1, "asoc") + xml('f')); f.add(1, list(0) + xml('a')); f.expect("afff"); }
    { Fixture f(placeholder(1, "grp ") + xml('f'));
      f.add(1, placeholder(2, "asoc") + association(1, 'b'));
      f.add(2, placeholder(3, "nlst") + placeholder(4, "xml "));
      f.add(3, {1, 0, 0, 0}); f.add(4, {'a'}); f.expect("abff"); }
    { Fixture f(xml('f') + placeholder(1, "asoc") + placeholder(2, "asoc") +
                placeholder(3, "asoc") + placeholder(4, "asoc"));
      f.add(1, box("nlst", {2, 0, 0, 1}) + xml('b'));
      f.add(2, list(0) + xml('a')); f.add(3, list(1) + xml('z'));
      f.add(4, list(3) + xml('d')); f.expect("abfd"); }
    // Inline headers retain their implicit entities through partitioned XML/asoc.
    { Fixture f(box("jpch", placeholder(1, "xml ")) + box("jplh", placeholder(2, "xml ")) +
                box("jpch", placeholder(3, "asoc")) + box("jplh", xml('U')), 2);
      f.add(1, {'S'}); f.add(2, {'T'});
      f.add(3, box("nlst", {2, 0, 0, 0}) + placeholder(4, "xml ")); f.add(4, {'N'});
      f.expect("SN");
      const uint8_t *data = NULL; size_t size = 0;
      check(hv_metadata_layer_xml(&f.metadata, 0, NULL, &data, &size, f.error, sizeof f.error) == 0 &&
            data != NULL && size == 1 && data[0] == 'T', "partitioned implicit layer XML");
      hv_registration registration = {NULL, 1, 0, 1, 1};
      check(hv_metadata_layer_xml(&f.metadata, 1, &registration, &data, &size, f.error, sizeof f.error) == 0 &&
            data != NULL && size == 1 && data[0] == 'S', "partitioned mapped-codestream XML"); }
    // All entities of the first list share its XML; scopes stop at siblings.
    { Fixture f(box("asoc", box("nlst", {1, 0, 0, 0, 1, 0, 0, 2}) + xml('a')) +
                association(3, 'd') + xml('f')); f.expect("afad"); }
    // A later number list is a sibling, not a new scope for the following XML.
    { Fixture f(box("asoc", list(0) + xml('a') + list(3) + xml('d')) + xml('f'));
      f.expect("afff"); }
    { Fixture f(box("asoc", box("lbl ", {'L'}) + list(3) + xml('d')) + xml('f'));
      f.expect("ffff"); }
    // A label or grouping box below an association retains its numbered scope.
    { Fixture f(box("asoc", list(2) + box("asoc", box("lbl ", {'L'}) +
                         box("grp ", xml('c')))) + xml('f')); f.expect("ffcf"); }
    // XML enclosed in an unnumbered association is not file-level fallback.
    { Fixture f(box("asoc", box("lbl ", {'L'}) + box("grp ", xml('x')))); f.expect("----"); }
    { Fixture f(box("grp ", xml('f') + association(1, 'b'))); f.expect("fbff"); }
    // Later nested scopes still get their first XML after an ancestor got its own.
    { Fixture f(box("asoc", list(0) + xml('a') + association(1, 'b') +
                         box("grp ", association(2, 'c') + xml('z')))); f.expect("abc-"); }
    // A first XML reached through a child applies to both scopes; siblings remain independent.
    { Fixture f(box("asoc", list(0) + association(1, 'b') + xml('z') + association(2, 'c')));
      f.expect("bbc-"); }
    // Large lists with many documents retain the first document for every entity.
    { const size_t count = 8000;
      Bytes names, documents;
      for (size_t i = 0; i < count; i++) {
          names.push_back(1); names.push_back(static_cast<uint8_t>(i >> 16));
          names.push_back(static_cast<uint8_t>(i >> 8)); names.push_back(static_cast<uint8_t>(i));
          Bytes document = xml(i == 0 ? 'a' : 'z');
          documents.insert(documents.end(), document.begin(), document.end());
      }
      Fixture f(box("asoc", box("nlst", names) + documents), count);
      f.expect(std::string(count, 'a')); }
    // Traversal must still reject damaged boxes after the first document was indexed.
    { Fixture f(box("asoc", list(0) + xml('a') + box("grp ", {0, 0, 0, 9, 'x', 'm', 'l', ' '})));
      f.reject("metadata:"); }
    // Distinct associations are the correct way to encode independent pairings.
    { Fixture f(association(0, 'a') + association(3, 'd') + xml('f')); f.expect("affd"); }
    { Fixture f(box("asoc", box("nlst", {1, 0, 0}) + xml('a'))); f.reject("nlst.length"); }
    { Fixture f(box("asoc", list(0))); f.reject("asoc.children"); }
    { Fixture f(box("asoc", box("grp ", association(0, 'a')) + xml('f'))); f.reject("grp.asoc-first"); }
    { Fixture f(placeholder(1, "asoc")); f.reject("is missing"); }
    { Fixture f(placeholder(1, "asoc")); f.add(1, list(0) + xml('a'), false); f.reject("is incomplete"); }
    { Fixture f(placeholder(1, "grp ")); f.add(1, placeholder(1, "grp ")); f.reject("box.depth-limit"); }
    { Bytes nested = association(0, 'a');
      for (int i = 1; i < HV_BOX_DEPTH_MAX; i++) nested = box("grp ", nested);
      Fixture f(nested); f.expect("a---"); }
    { Bytes nested = association(0, 'a');
      for (int i = 0; i < HV_BOX_DEPTH_MAX; i++) nested = box("grp ", nested);
      Fixture f(nested); f.reject("box.depth-limit"); }
    { Fixture f(box("grp ", {0, 0, 0, 9, 'x', 'm', 'l', ' '})); f.reject("metadata:"); }
    std::cout << "JPX metadata semantics and partitioning passed\n";
}
