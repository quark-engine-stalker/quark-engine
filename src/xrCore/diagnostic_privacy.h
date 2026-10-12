#pragma once

#include <cstddef>
#include <cstring>

// Text-only diagnostics. Never dereference pointers to inspect user data and
// never include command lines, environment blocks, Lua locals or memory dumps.
// The sink can be a bounded buffer or a streaming file writer (no crash heap).
namespace DiagnosticPrivacy
{
inline bool separator(char c) { return c == '/' || c == '\\'; }
inline bool word(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
        (u >= '0' && u <= '9') || u == '_' || u >= 0x80;
}
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }
inline bool starts(const char* text, const char* key)
{
    while (*key)
        if (!*text || lower(*text++) != lower(*key++)) return false;
    return true;
}
inline bool boundary(char c)
{
    return !c || c == '\r' || c == '\n' || c == '\t' || c == '"' || c == '\'' ||
        c == '<' || c == '>' || c == '|' || c == ';' || c == ',' ||
        c == '(' || c == ')' || c == '[' || c == ']';
}
struct Names
{
    char user[256] = {};
    char computer[256] = {};
    char userAnsi[256] = {};
    char computerAnsi[256] = {};
};

template<class Sink> void literal(Sink& sink, const char* value)
{
    while (*value) sink.put(*value++);
}

// Lua errors use "path/module.script:line: message"; stack frames use
// "path/module.script:line (function)". Keep only the source identifier and
// resume filtering the message, rather than discarding the whole diagnostic.
// Ordinary resource paths still follow the conservative field redaction.
inline const char* lua_source_basename(const char* path, size_t limit)
{
    const char* name = path;
    bool identifier = true;
    for (const char* p = path; size_t(p - path) < limit && *p && *p != '\r' && *p != '\n' && *p != '\t' &&
        *p != '"' && *p != ';' && *p != '|'; ++p)
    {
        if (separator(*p)) { name = p + 1; identifier = true; continue; }
        if (identifier && p > name && p - name <= 160 && starts(p, ".script:"))
        {
            const char* line = p + sizeof(".script:") - 1;
            const char* end = line;
            while (*end >= '0' && *end <= '9' && end - line < 10) ++end;
            if (end != line &&
                ((*end == ':' && (end[1] == ' ' || !end[1])) ||
                 (*end == ' ' && end[1] == '(')))
                return name;
        }
        const unsigned char c = static_cast<unsigned char>(*p);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            identifier = false;
    }
    return nullptr;
}

template<class Sink> void sanitize(const char* text, Sink& sink, const Names& names)
{
    if (!text) return;
    const char* begin = text;
    while (*text)
    {
        // URLs can carry webhook credentials, user info, IPs and query tokens.
        if (starts(text, "https://") || starts(text, "http://") ||
            starts(text, "ftp://") || starts(text, "file://"))
        {
            literal(sink, "<redacted-url>");
            while (*text && *text != '\r' && *text != '\n' && *text != '\t' &&
                *text != ' ' && *text != '"' && *text != '<' && *text != '>') ++text;
            continue;
        }
        // Include drive-relative paths (C:foo), extended/device paths and UNC.
        const bool drive = ((text[0] >= 'A' && text[0] <= 'Z') ||
            (text[0] >= 'a' && text[0] <= 'z')) && text[1] == ':' &&
            (separator(text[2]) || ((text == begin || !word(text[-1])) && text[2] && word(text[2])));
        const bool unc = separator(text[0]) && separator(text[1]) &&
            text[2] && text[2] != ' ';
        const bool rooted = separator(text[0]) && text[1] &&
            (text == begin || text[-1] == ' ' || text[-1] == '"' || text[-1] == '=' || text[-1] == '@') &&
            (starts(text, "/home/") || starts(text, "/Users/") || starts(text, "/mnt/") ||
             starts(text, "\\Users\\") || starts(text, "\\Device\\"));
        // Lua shortens long source paths to "...<tail>". The tail may still
        // contain a private directory, so reduce these locations as well.
        const bool shortenedLua = starts(text, "...") &&
            (text == begin || (!word(text[-1]) && text[-1] != '.'));
        if (drive || unc || rooted || shortenedLua)
        {
            // Bound lookahead: repeated ellipses in a large unrelated message
            // must not cause quadratic scanning in ordinary session logging.
            if (const char* source = lua_source_basename(text, shortenedLua ? 256 : 4096))
            {
                if (source > text) { text = source; continue; }
            }
        }
        if (drive || unc || rooted)
        {
            // An unquoted path may contain spaces. Prefer hiding the rest of
            // this field over leaking a directory after a whitespace boundary.
            const char* end = text;
            // Apostrophes, brackets and parentheses are legal directory names
            // (e.g. .Grok's Modpack Installer), not reliable path terminators.
            while (*end && *end != '\r' && *end != '\n' && *end != '\t' &&
                *end != '"' && *end != ';' && *end != '|') ++end;
            if (end == text) { ++text; continue; }
            literal(sink, "<redacted-path>");
            text = end;
            continue;
        }
        const char* keys[] = {"password", "passwd", "pwd", "token", "access_token",
            "refresh_token", "api_key", "apikey", "secret", "authorization",
            "username", "computername", "user_name", "hostname"};
        bool hidden = false;
        if (text == begin || !word(text[-1]))
        {
            for (const char* key : keys)
            {
                if (!starts(text, key)) continue;
                const char* value = text + std::strlen(key);
                if (*value == '"' || *value == '\'') ++value; // JSON/quoted key
                while (*value == ' ' || *value == '\t') ++value;
                if (*value != ':' && *value != '=') continue;
                ++value;
                while (*value == ' ' || *value == '\t') ++value;
                const char quote = (*value == '"' || *value == '\'') ? *value++ : 0;
                while (text < value) sink.put(*text++);
                literal(sink, "<redacted>");
                if (quote)
                {
                    while (*text && *text != quote && *text != '\n' && *text != '\r')
                    { if (*text == '\\' && text[1]) ++text; ++text; }
                }
                else
                {
                    // Consume value brackets too; they may be part of a secret.
                    // Already filtered input must stay filtered without growing.
                    while (*text && *text != '\r' && *text != '\n' && *text != ';' &&
                        *text != ',' && *text != '|' && *text != '}' && *text != '\"') ++text;
                }
                hidden = true;
                break;
            }
        }
        if (hidden) continue;
        const char* privateNames[] = {names.user, names.computer, names.userAnsi, names.computerAnsi};
        for (const char* name : privateNames)
        {
            if (!*name || !starts(text, name)) continue;
            const size_t length = std::strlen(name);
            if ((text == begin || !word(text[-1])) && !word(text[length]))
            {
                literal(sink, "<redacted-identity>");
                text += length;
                hidden = true;
                break;
            }
        }
        if (hidden) continue;
        // IPv6 (including a numeric interface scope); C++ names containing '::'
        // have non-hex letters and must remain useful in symbolized stacks.
        if ((*text >= '0' && *text <= '9') || (lower(*text) >= 'a' && lower(*text) <= 'f') || *text == ':')
        {
            const char* ip = text;
            unsigned colons = 0, digits = 0;
            bool compressed = false;
            while ((*ip >= '0' && *ip <= '9') || (lower(*ip) >= 'a' && lower(*ip) <= 'f') || *ip == ':')
            {
                if (*ip == ':') { ++colons; if (ip[1] == ':') compressed = true; }
                else ++digits;
                ++ip;
            }
            // HH:MM:SS is not an IPv6 address: an uncompressed address has
            // eight groups, a shorter valid address needs '::' compression.
            if ((compressed || colons == 7) && colons >= 2 && digits && ip - text >= 8 && !word(*ip))
            {
                if (*ip == '%') { ++ip; while (word(*ip)) ++ip; }
                literal(sink, "<redacted-ip>"); text = ip; continue;
            }
        }
        // Recognize email addresses and dotted IPv4 endpoints without depending
        // on a configured installation root or a user's machine language.
        if (word(*text) && (text == begin || !word(text[-1])))
        {
            const char* end = text;
            bool email = false;
            while (*end && !boundary(*end) && *end != ' ' && *end != '=' && *end != ':')
            { if (*end == '@') email = true; ++end; }
            if (email)
            {
                literal(sink, "<redacted-email>"); text = end; continue;
            }
            const char* ip = text;
            unsigned parts = 0;
            while (parts < 4)
            {
                unsigned value = 0, digits = 0;
                while (*ip >= '0' && *ip <= '9' && digits < 4)
                { value = value * 10 + (*ip++ - '0'); ++digits; }
                if (!digits || digits > 3 || value > 255) break;
                ++parts;
                if (parts == 4 || *ip != '.') break;
                ++ip;
            }
            if (parts == 4 && !word(*ip) && *ip != '.')
            { literal(sink, "<redacted-ip>"); text = ip; continue; }
        }
        const unsigned char c = static_cast<unsigned char>(*text++);
        sink.put((c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c == 0x7f ? ' ' : char(c));
    }
}

struct Buffer
{
    char* data;
    size_t capacity;
    size_t length = 0;
    void put(char c)
    {
        if (data && length + 1 < capacity) data[length] = c;
        ++length;
    }
    void finish()
    {
        if (!data || !capacity) return;
        size_t end = length < capacity ? length : capacity - 1;
        if (length >= capacity && capacity >= 16)
        {
            static const char marker[] = "<truncated>";
            end = capacity - sizeof(marker);
            // Do not leave a broken UTF-8 codepoint before the truncation marker.
            while (end && (static_cast<unsigned char>(data[end]) & 0xc0) == 0x80) --end;
            std::memcpy(data + end, marker, sizeof(marker));
        }
        else data[end] = 0;
    }
};
}
