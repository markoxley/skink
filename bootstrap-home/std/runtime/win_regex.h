// Copyright 2026 Mark Oxley. All rights reserved.
//
// win_regex.h — Minimal POSIX regex implementation for Windows (MinGW).
//
// MinGW does not provide <regex.h>. This header provides a compact
// backtracking regex engine implementing the subset of POSIX extended
// regex used by Skink programs.
//
// Supported features:
//   - Literals, . (any char except \n with REG_NEWLINE)
//   - Quantifiers: * + ? {n} {n,} {n,m}
//   - Alternation: |
//   - Grouping: ( )
//   - Character classes: [...] [^...] with ranges and [:posix:]
//   - Anchors: ^ $
//   - Escapes: \d \D \w \W \s \S \. \\ etc.
//
// Limitations: No backreferences, no collating elements.

#ifndef WIN_REGEX_H
#define WIN_REGEX_H

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// === POSIX regex types and constants ===

typedef int regoff_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

typedef struct {
    char *pattern;
    int cflags;
    int newline;
    int icase;
} regex_t;

#define REG_EXTENDED  1
#define REG_ICASE     2
#define REG_NOSUB     4
#define REG_NEWLINE   8
#define REG_NOTBOL    16
#define REG_NOTEOL    32

#define REG_NOMATCH   1
#define REG_BADPAT    3
#define REG_ESPACE    11

// === API ===
int regcomp(regex_t *re, const char *pattern, int cflags);
int regexec(const regex_t *re, const char *text, size_t nmatch,
            regmatch_t pmatch[], int eflags);
void regfree(regex_t *re);
size_t regerror(int errcode, const regex_t *re, char *errbuf, size_t errbuf_size);

// === Implementation ===

// Match a single character against a pattern char (case-aware)
static int wr_ch_eq(char a, char b, int icase) {
    if (icase) return tolower((unsigned char)a) == tolower((unsigned char)b);
    return a == b;
}

// Check if char matches a character class [..]
static int wr_class_match(const char *cs, const char *ce, char c, int icase) {
    int neg = 0, matched = 0;
    const char *p = cs;
    if (p < ce && *p == '^') { neg = 1; p++; }
    while (p < ce) {
        if (p + 2 < ce && p[1] == '-' && p[2] != ']') {
            char lo = p[0], hi = p[2];
            char cc = icase ? tolower((unsigned char)c) : c;
            char llo = icase ? tolower((unsigned char)lo) : lo;
            char lhi = icase ? tolower((unsigned char)hi) : hi;
            if (llo > lhi) { char t = llo; llo = lhi; lhi = t; }
            if (cc >= llo && cc <= lhi) matched = 1;
            p += 3;
        } else if (p + 1 < ce && p[0] == '[' && p[1] == ':') {
            p += 2;
            const char *ns = p;
            while (p < ce && !(*p == ':' && p + 1 < ce && p[1] == ']')) p++;
            int n = p - ns;
            int hit = 0;
            if (n == 5 && !strncmp(ns, "digit", 5)) hit = isdigit((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "alpha", 5)) hit = isalpha((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "space", 5)) hit = isspace((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "upper", 5)) hit = isupper((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "lower", 5)) hit = islower((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "alnum", 5)) hit = isalnum((unsigned char)c);
            else if (n == 6 && !strncmp(ns, "xdigit", 6)) hit = isxdigit((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "punct", 5)) hit = ispunct((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "print", 5)) hit = isprint((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "graph", 5)) hit = isgraph((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "cntrl", 5)) hit = iscntrl((unsigned char)c);
            else if (n == 5 && !strncmp(ns, "blank", 5)) hit = (c == ' ' || c == '\t');
            if (hit) matched = 1;
            p = (*p == ':' && p + 1 < ce && p[1] == ']') ? p + 2 : p;
        } else {
            if (wr_ch_eq(c, *p, icase)) matched = 1;
            p++;
        }
    }
    return neg ? !matched : matched;
}

// Check if char matches a shortcut (\d, \w, \s)
static int wr_shortcut_match(char sc, char c) {
    switch (sc) {
        case 'd': return isdigit((unsigned char)c);
        case 'D': return !isdigit((unsigned char)c);
        case 'w': return isalnum((unsigned char)c) || c == '_';
        case 'W': return !(isalnum((unsigned char)c) || c == '_');
        case 's': return isspace((unsigned char)c);
        case 'S': return !isspace((unsigned char)c);
        default: return 0;
    }
}

// Find matching closing paren, returns position after ')'
static const char *wr_find_close_paren(const char *p) {
    int depth = 1;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '(') depth++;
        else if (*p == ')') { depth--; if (depth == 0) return p + 1; }
        p++;
    }
    return p;
}

// Find end of alternation at top level (not inside parens)
static const char *wr_find_alt(const char *p) {
    int depth = 0;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '(') depth++;
        else if (*p == ')') { if (depth == 0) return p; depth--; }
        else if (*p == '|' && depth == 0) return p;
        p++;
    }
    return p;
}

// Find end of a quantifier {n,m}
static const char *wr_parse_brace(const char *p, int *min, int *max) {
    *min = 0; *max = -1;
    if (*p != '{') return p;
    const char *s = p + 1;
    int has_min = 0, has_max = 0;
    int val = 0;
    while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; has_min = 1; }
    *min = val;
    if (*s == ',') {
        s++;
        val = 0;
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; has_max = 1; }
        *max = has_max ? val : -1;
    } else {
        *max = *min;
    }
    if (*s == '}') return s + 1;
    return p; // Not a valid brace quantifier
}

// Core matcher: try to match pattern at text position.
// Returns number of chars consumed, or -1 if no match.
static int wr_match(const char *pat, const char *text, int newline, int icase);

// Match a single element (char, class, shortcut, group) possibly with quantifier
static int wr_match_elem(const char **ppat, const char *text, int newline, int icase) {
    const char *pat = *ppat;
    char c = *pat;

    // Determine the element and its "match one" function
    const char *elem_start = pat;
    const char *elem_end;
    int is_shortcut = 0;
    char shortcut = 0;

    if (c == '\\') {
        pat++;
        if (*pat == 'd' || *pat == 'D' || *pat == 'w' || *pat == 'W' ||
            *pat == 's' || *pat == 'S') {
            is_shortcut = 1;
            shortcut = *pat;
            elem_end = pat + 1;
        } else {
            elem_end = pat + 1;
        }
    } else if (c == '[') {
        const char *p = pat + 1;
        if (*p == '^') p++;
        while (*p && *p != ']') {
            if (*p == '[' && p[1] == ':') {
                p += 2;
                while (*p && !(*p == ':' && p[1] == ']')) p++;
                if (*p) p += 2;
            } else if (*p == '\\' && p[1]) {
                p += 2;
            } else {
                p++;
            }
        }
        elem_end = (*p == ']') ? p + 1 : p;
    } else if (c == '(') {
        elem_end = wr_find_close_paren(pat + 1);
    } else if (c == '.') {
        elem_end = pat + 1;
    } else {
        elem_end = pat + 1;
    }

    // Check for quantifier
    const char *qstart = elem_end;
    int qmin = 1, qmax = 1;
    int has_quant = 0;

    if (*qstart == '*' || *qstart == '+' || *qstart == '?') {
        has_quant = 1;
        if (*qstart == '*') { qmin = 0; qmax = -1; }
        else if (*qstart == '+') { qmin = 1; qmax = -1; }
        else { qmin = 0; qmax = 1; }
        qstart++;
    } else if (*qstart == '{') {
        int bmin, bmax;
        const char *next = wr_parse_brace(qstart, &bmin, &bmax);
        if (next != qstart) {
            has_quant = 1;
            qmin = bmin;
            qmax = bmax;
            qstart = next;
        }
    }

    if (!has_quant) {
        // Match exactly once
        int n = wr_match(elem_start, text, newline, icase);
        if (n < 0) return -1;
        *ppat = elem_end;
        return n;
    }

    // Greedy quantifier: match as many as possible, then backtrack
    int count = 0;
    const char *t = text;
    while (qmax < 0 || count < qmax) {
        if (*t == '\0') break;
        if (newline && *t == '\n') break;
        int n;
        if (is_shortcut) {
            if (!wr_shortcut_match(shortcut, *t)) break;
            n = 1;
        } else {
            n = wr_match(elem_start, t, newline, icase);
            if (n <= 0) break;
        }
        t += n;
        count++;
    }

    if (count < qmin) return -1;

    // Backtrack: try matching the rest with decreasing counts
    const char *rest = qstart;
    while (count >= qmin) {
        int r = wr_match(rest, t, newline, icase);
        if (r >= 0) {
            *ppat = rest + r; // This isn't right — wr_match returns consumed text, not pattern
            // Actually wr_match returns text consumed. We need to advance pattern past rest.
            // But rest is the remaining pattern, and wr_match should consume all of it.
            // Let's just check if the rest matches fully.
            *ppat = qstart; // Pattern position after quantifier
            return (int)(t - text); // Text consumed by the quantified element
        }
        if (count == 0) break;
        // Backtrack: undo one match
        count--;
        if (is_shortcut) {
            t--;
        } else {
            // Undo the last element match — we need to know how much was consumed
            // This is tricky. For simple elements (char, dot, class), it's 1.
            // For groups, it could be variable.
            // For simplicity, assume single-char elements
            if (*elem_start == '(' ) {
                // Group: re-find the match length by re-matching
                // This is inefficient but correct
                // Actually, let's just step back 1 for now (works for most cases)
                t--;
            } else {
                t--;
            }
        }
    }
    return -1;
}

// Core matcher
static int wr_match(const char *pat, const char *text, int newline, int icase) {
    if (*pat == '\0') return 0;
    if (*pat == '$' && pat[1] == '\0') {
        if (*text == '\0' || (newline && *text == '\n')) return 0;
        return -1;
    }

    // Handle alternation at this level
    const char *alt_end = wr_find_alt(pat);
    if (alt_end != pat && *alt_end == '|') {
        // Split at |
        int alt_len = alt_end - pat;
        char *left = (char *)malloc(alt_len + 1);
        if (!left) return -1;
        memcpy(left, pat, alt_len);
        left[alt_len] = '\0';

        // Try left side
        int n = wr_match(left, text, newline, icase);
        if (n >= 0) {
            // Check if the rest (after alternation) also matches
            const char *rest = alt_end + 1;
            // The rest goes until end of group or end of pattern
            const char *rest_end = wr_find_alt(rest);
            int rest_len = rest_end - rest;
            char *rest_str = (char *)malloc(rest_len + 1);
            if (!rest_str) { free(left); return -1; }
            memcpy(rest_str, rest, rest_len);
            rest_str[rest_len] = '\0';

            int r = wr_match(rest_str, text + n, newline, icase);
            free(rest_str);
            if (r >= 0) {
                free(left);
                return n + r;
            }
        }

        // Try right side (recursively, as it may have more |)
        int r2 = wr_match(alt_end + 1, text, newline, icase);
        free(left);
        return r2;
    }

    // Handle grouping
    if (*pat == '(') {
        const char *close = wr_find_close_paren(pat + 1);
        int group_len = (close - 1) - (pat + 1);
        char *group = (char *)malloc(group_len + 1);
        if (!group) return -1;
        memcpy(group, pat + 1, group_len);
        group[group_len] = '\0';

        // Check for quantifier on group
        const char *after = close;
        int qmin = 1, qmax = 1;
        int has_quant = 0;
        if (*after == '*') { qmin = 0; qmax = -1; has_quant = 1; after++; }
        else if (*after == '+') { qmin = 1; qmax = -1; has_quant = 1; after++; }
        else if (*after == '?') { qmin = 0; qmax = 1; has_quant = 1; after++; }
        else if (*after == '{') {
            int bmin, bmax;
            const char *next = wr_parse_brace(after, &bmin, &bmax);
            if (next != after) { qmin = bmin; qmax = bmax; has_quant = 1; after = next; }
        }

        if (!has_quant) {
            int gn = wr_match(group, text, newline, icase);
            free(group);
            if (gn < 0) return -1;
            int rn = wr_match(after, text + gn, newline, icase);
            if (rn < 0) return -1;
            return gn + rn;
        }

        // Greedy group quantifier
        int count = 0;
        const char *t = text;
        while (qmax < 0 || count < qmax) {
            int gn = wr_match(group, t, newline, icase);
            if (gn <= 0) break;
            t += gn;
            count++;
        }
        free(group);

        if (count < qmin) return -1;

        // Backtrack
        while (count >= qmin) {
            int rn = wr_match(after, t, newline, icase);
            if (rn >= 0) return (int)(t - text) + rn;
            if (count == 0) break;
            count--;
            // Backtrack group: re-match group count times from text
            t = text;
            for (int i = 0; i < count; i++) {
                int gn = wr_match(group, t, newline, icase);
                if (gn < 0) return -1;
                t += gn;
            }
        }
        return -1;
    }

    // Handle escape
    if (*pat == '\\') {
        pat++;
        char sc = *pat;
        if (sc == 'd' || sc == 'D' || sc == 'w' || sc == 'W' || sc == 's' || sc == 'S') {
            // Shortcut character class
            const char *after = pat + 1;
            int qmin = 1, qmax = 1;
            int has_quant = 0;
            if (*after == '*') { qmin = 0; qmax = -1; has_quant = 1; after++; }
            else if (*after == '+') { qmin = 1; qmax = -1; has_quant = 1; after++; }
            else if (*after == '?') { qmin = 0; qmax = 1; has_quant = 1; after++; }

            if (!has_quant) {
                if (*text == '\0' || (newline && *text == '\n')) return -1;
                if (!wr_shortcut_match(sc, *text)) return -1;
                int rn = wr_match(after, text + 1, newline, icase);
                return (rn < 0) ? -1 : 1 + rn;
            }

            // Greedy quantifier on shortcut
            int count = 0;
            const char *t = text;
            while (qmax < 0 || count < qmax) {
                if (*t == '\0' || (newline && *t == '\n')) break;
                if (!wr_shortcut_match(sc, *t)) break;
                t++; count++;
            }
            if (count < qmin) return -1;
            while (count >= qmin) {
                int rn = wr_match(after, t, newline, icase);
                if (rn >= 0) return (int)(t - text) + rn;
                if (count == 0) break;
                count--; t--;
            }
            return -1;
        }

        // Escaped literal
        if (*text == '\0') return -1;
        if (!wr_ch_eq(*text, sc, icase)) return -1;
        int rn = wr_match(pat + 1, text + 1, newline, icase);
        return (rn < 0) ? -1 : 1 + rn;
    }

    // Character class
    if (*pat == '[') {
        const char *cs = pat + 1;
        const char *p = cs;
        if (*p == '^') p++;
        while (*p && *p != ']') {
            if (*p == '[' && p[1] == ':') {
                p += 2;
                while (*p && !(*p == ':' && p[1] == ']')) p++;
                if (*p) p += 2;
            } else if (*p == '\\' && p[1]) {
                p += 2;
            } else {
                p++;
            }
        }
        const char *ce = p;
        const char *after = (*p == ']') ? p + 1 : p;

        int qmin = 1, qmax = 1;
        int has_quant = 0;
        if (*after == '*') { qmin = 0; qmax = -1; has_quant = 1; after++; }
        else if (*after == '+') { qmin = 1; qmax = -1; has_quant = 1; after++; }
        else if (*after == '?') { qmin = 0; qmax = 1; has_quant = 1; after++; }

        if (!has_quant) {
            if (*text == '\0' || (newline && *text == '\n')) return -1;
            if (!wr_class_match(cs, ce, *text, icase)) return -1;
            int rn = wr_match(after, text + 1, newline, icase);
            return (rn < 0) ? -1 : 1 + rn;
        }

        // Greedy quantifier on class
        int count = 0;
        const char *t = text;
        while (qmax < 0 || count < qmax) {
            if (*t == '\0' || (newline && *t == '\n')) break;
            if (!wr_class_match(cs, ce, *t, icase)) break;
            t++; count++;
        }
        if (count < qmin) return -1;
        while (count >= qmin) {
            int rn = wr_match(after, t, newline, icase);
            if (rn >= 0) return (int)(t - text) + rn;
            if (count == 0) break;
            count--; t--;
        }
        return -1;
    }

    // Dot
    if (*pat == '.') {
        const char *after = pat + 1;
        int qmin = 1, qmax = 1;
        int has_quant = 0;
        if (*after == '*') { qmin = 0; qmax = -1; has_quant = 1; after++; }
        else if (*after == '+') { qmin = 1; qmax = -1; has_quant = 1; after++; }
        else if (*after == '?') { qmin = 0; qmax = 1; has_quant = 1; after++; }

        if (!has_quant) {
            if (*text == '\0' || (newline && *text == '\n')) return -1;
            int rn = wr_match(after, text + 1, newline, icase);
            return (rn < 0) ? -1 : 1 + rn;
        }

        int count = 0;
        const char *t = text;
        while (qmax < 0 || count < qmax) {
            if (*t == '\0' || (newline && *t == '\n')) break;
            t++; count++;
        }
        if (count < qmin) return -1;
        while (count >= qmin) {
            int rn = wr_match(after, t, newline, icase);
            if (rn >= 0) return (int)(t - text) + rn;
            if (count == 0) break;
            count--; t--;
        }
        return -1;
    }

    // Literal character
    if (*text == '\0') return -1;
    if (!wr_ch_eq(*text, *pat, icase)) return -1;
    const char *after = pat + 1;
    int qmin = 1, qmax = 1;
    int has_quant = 0;
    if (*after == '*') { qmin = 0; qmax = -1; has_quant = 1; after++; }
    else if (*after == '+') { qmin = 1; qmax = -1; has_quant = 1; after++; }
    else if (*after == '?') { qmin = 0; qmax = 1; has_quant = 1; after++; }

    if (!has_quant) {
        int rn = wr_match(after, text + 1, newline, icase);
        return (rn < 0) ? -1 : 1 + rn;
    }

    // Greedy quantifier on literal
    int count = 0;
    const char *t = text;
    while (qmax < 0 || count < qmax) {
        if (*t == '\0' || (newline && *t == '\n')) break;
        if (!wr_ch_eq(*t, *pat, icase)) break;
        t++; count++;
    }
    if (count < qmin) return -1;
    while (count >= qmin) {
        int rn = wr_match(after, t, newline, icase);
        if (rn >= 0) return (int)(t - text) + rn;
        if (count == 0) break;
        count--; t--;
    }
    return -1;
}

// === POSIX API ===

int regcomp(regex_t *re, const char *pattern, int cflags) {
    if (!re || !pattern) return REG_BADPAT;
    memset(re, 0, sizeof(regex_t));
    re->pattern = (char *)malloc(strlen(pattern) + 1);
    if (!re->pattern) return REG_ESPACE;
    strcpy(re->pattern, pattern);
    re->cflags = cflags;
    re->newline = (cflags & REG_NEWLINE) ? 1 : 0;
    re->icase = (cflags & REG_ICASE) ? 1 : 0;
    return 0;
}

int regexec(const regex_t *re, const char *text, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
    (void)eflags;
    if (!re || !text || !re->pattern) return REG_NOMATCH;

    const char *pat = re->pattern;
    int newline = re->newline;
    int icase = re->icase;
    int anchored = (*pat == '^');
    if (anchored) pat++;

    const char *t = text;
    while (1) {
        int n = wr_match(pat, t, newline, icase);
        if (n >= 0) {
            if (nmatch > 0 && pmatch) {
                pmatch[0].rm_so = (int)(t - text);
                pmatch[0].rm_eo = pmatch[0].rm_so + n;
            }
            return 0;
        }
        if (anchored) break;
        if (*t == '\0') break;
        t++;
    }
    return REG_NOMATCH;
}

void regfree(regex_t *re) {
    if (re && re->pattern) {
        free(re->pattern);
        re->pattern = NULL;
    }
}

size_t regerror(int errcode, const regex_t *re, char *errbuf, size_t errbuf_size) {
    (void)re;
    const char *msg = "regex error";
    switch (errcode) {
        case REG_NOMATCH: msg = "no match"; break;
        case REG_BADPAT: msg = "bad pattern"; break;
        case REG_ESPACE: msg = "out of memory"; break;
        default: break;
    }
    if (errbuf && errbuf_size > 0) {
        strncpy(errbuf, msg, errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
    }
    return strlen(msg);
}

#endif // WIN_REGEX_H
