// Parsing the watch catalog index (index-watch.json, docs/reference/CATALOG.md). The
// document is machine-made and tiny, so this is a strict one-pass reader rather than a
// JSON library: anything outside the compact schema - an unknown key, an over-long
// string, a bad hash - fails the whole parse, the way the catalog builder fails a whole
// deploy on one bad entry. The parsed catalog is a fixed-size array, so the watch's
// memory for it is known before the first byte of JSON is read.
#pragma once

#include <cstdint>

namespace console {

// Same cap as the builder's (tools/make_catalog.py MAX_GAMES) - the builder refuses to
// emit more, and this parser refuses to hold more.
constexpr int STORE_MAX_GAMES = 200;
// The revoked list is bounded the same way; a hostile or mis-built index past it fails.
constexpr int STORE_MAX_REVOKED = 64;

struct StoreEntry {
    char id[16];
    char name[24];
    char author[24];
    char summary[64];
    char url[96];
    char sha[65];        // sha256 of the package, lower-case hex
    uint32_t size;
    uint16_t version;
    uint8_t api_major, api_minor;
    bool official;       // channel: official vs community
    bool revoked;        // this package's sha256 is on the index's revoked list
};

struct StoreCatalog {
    StoreEntry entries[STORE_MAX_GAMES];
    int count = 0;
    char generated[24];  // the index's timestamp, shown as a freshness hint
};

// Parse the body of index-watch.json. False on anything malformed; out is left usable
// (count 0) on failure.
bool store_index_parse(const char *body, StoreCatalog &out);

}  // namespace console
