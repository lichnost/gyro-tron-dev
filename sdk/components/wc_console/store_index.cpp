#include "console/store_index.h"

#include <cstdio>
#include <cstring>

namespace console {

// ---------------------------------------------------------------- a strict JSON reader

namespace {

struct P {
    const char *s;

    void ws()
    {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    }
    bool eat(char c)
    {
        ws();
        if (*s != c) return false;
        s++;
        return true;
    }
    bool peek(char c)
    {
        ws();
        return *s == c;
    }
    bool literal(const char *word)
    {
        ws();
        const size_t n = strlen(word);
        if (strncmp(s, word, n) != 0) return false;
        s += n;
        return true;
    }

    // A quoted string into a fixed buffer: printable ASCII only (catalog strings end up
    // on the screen), and it must fit - the builder's caps are the schema, anything
    // longer is a hostile or mis-built document.
    bool str(char *out, size_t cap)
    {
        ws();
        if (*s != '"') return false;
        s++;
        size_t n = 0;
        while (*s && *s != '"') {
            const unsigned char c = *s;
            if (c < 0x20 || c > 0x7e) return false;
            if (n + 1 >= cap) return false;
            out[n++] = char(c);
            s++;
        }
        if (*s != '"') return false;
        s++;
        out[n] = 0;
        return true;
    }

    bool num(uint32_t &out)
    {
        ws();
        if (*s < '0' || *s > '9') return false;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + uint32_t(*s - '0');
            if (v > 0xFFFFFF) return false;   // nothing in a catalog is anywhere near this
            s++;
        }
        out = v;
        return true;
    }

    // The api pair as the builder writes it: a quoted "1.9".
    bool api(uint8_t &major, uint8_t &minor)
    {
        ws();
        if (*s != '"') return false;
        s++;
        uint32_t a = 0, b = 0;
        if (!num(a) || !eat('.') || !num(b) || a > 255 || b > 255 || *s != '"') return false;
        s++;
        major = uint8_t(a);
        minor = uint8_t(b);
        return true;
    }
    // A string the store validates but does not keep (the icon path: the list renders
    // text rows only, no icon is downloaded for a game that is not installed).
    bool skip_str()
    {
        char scratch[96];
        return str(scratch, sizeof(scratch));
    }
};

// One key of a game object; keys are matched against the compact schema and anything
// unknown is refused, so the schema stays the agreement rather than a suggestion.
bool game_key(P &p, StoreEntry &e)
{
    char key[8];
    if (!p.str(key, sizeof(key))) return false;
    if (!p.eat(':')) return false;
    if (!strcmp(key, "i")) return p.str(e.id, sizeof(e.id)) && e.id[0];
    if (!strcmp(key, "n")) return p.str(e.name, sizeof(e.name)) && e.name[0];
    if (!strcmp(key, "a")) return p.str(e.author, sizeof(e.author));
    if (!strcmp(key, "v")) {
        uint32_t v;
        if (!p.num(v) || v == 0 || v > 65535) return false;
        e.version = uint16_t(v);
        return true;
    }
    if (!strcmp(key, "p")) return p.api(e.api_major, e.api_minor);
    if (!strcmp(key, "c")) {
        char ch[2];
        if (!p.str(ch, sizeof(ch))) return false;
        if (ch[0] != 'o' && ch[0] != 'c') return false;
        e.official = ch[0] == 'o';
        return true;
    }
    if (!strcmp(key, "z")) return p.num(e.size) && e.size > 0;
    if (!strcmp(key, "h")) {
        if (!p.str(e.sha, sizeof(e.sha))) return false;
        if (strlen(e.sha) != 64) return false;
        for (int i = 0; i < 64; i++) {
            const char c = e.sha[i];
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!hex) return false;
        }
        return true;
    }
    if (!strcmp(key, "u")) return p.str(e.url, sizeof(e.url)) && e.url[0];
    if (!strcmp(key, "ic")) return p.skip_str();
    if (!strcmp(key, "s")) return p.str(e.summary, sizeof(e.summary));
    return false;
}

}  // namespace

bool store_index_parse(const char *body, StoreCatalog &out)
{
    out.count = 0;
    out.generated[0] = 0;
    if (!body) return false;

    P p{body};
    char revsha[STORE_MAX_REVOKED][65];
    int revoked_n = 0;

    if (!p.eat('{')) return false;
    bool seen_schema = false, seen_games = false;
    if (!p.peek('}')) {
        do {
            char key[12];
            if (!p.str(key, sizeof(key)) || !p.eat(':')) return false;
            if (!strcmp(key, "schema")) {
                uint32_t v;
                if (!p.num(v) || v != 1) return false;
                seen_schema = true;
            } else if (!strcmp(key, "generated")) {
                if (!p.str(out.generated, sizeof(out.generated))) return false;
            } else if (!strcmp(key, "games")) {
                if (!p.eat('[')) return false;
                if (!p.peek(']')) {
                    do {
                        if (out.count >= STORE_MAX_GAMES) return false;
                        StoreEntry &e = out.entries[out.count];
                        memset(&e, 0, sizeof(e));
                        e.official = false;
                        if (!p.eat('{')) return false;
                        if (!p.peek('}')) {
                            do {
                                if (!game_key(p, e)) return false;
                            } while (p.eat(','));
                        }
                        if (!p.eat('}')) return false;
                        // Every field the store acts on must be there - a half-described
                        // entry is not installable and not shown. The id is also the file
                        // name it will be installed under, so it stays builder-strict:
                        // lowercase [a-z0-9_].
                        if (!e.id[0] || !e.name[0] || !e.sha[0] || !e.url[0] || !e.size || !e.version) return false;
                        for (const char *c = e.id; *c; c++) {
                            const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_';
                            if (!ok) return false;
                        }
                        out.count++;
                    } while (p.eat(','));
                }
                if (!p.eat(']')) return false;
                seen_games = true;
            } else if (!strcmp(key, "revoked")) {
                if (!p.eat('[')) return false;
                if (!p.peek(']')) {
                    do {
                        if (revoked_n >= STORE_MAX_REVOKED) return false;
                        if (!p.str(revsha[revoked_n], 65) || strlen(revsha[revoked_n]) != 64) return false;
                        revoked_n++;
                    } while (p.eat(','));
                }
                if (!p.eat(']')) return false;
            } else {
                return false;
            }
        } while (p.eat(','));
    }
    if (!p.eat('}')) return false;
    p.ws();                       // the builder ends the file with a newline; legal JSON
    if (*p.s) return false;
    if (!seen_schema || !seen_games) return false;

    for (int r = 0; r < revoked_n; r++)
        for (int i = 0; i < out.count; i++)
            if (strcmp(out.entries[i].sha, revsha[r]) == 0) out.entries[i].revoked = true;
    return true;
}

}  // namespace console

