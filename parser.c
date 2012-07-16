
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include "parser.h"
#include "error.h"

typedef struct {
    const char* filename;
    const char* domain;
    ScannerT* scanner;
    NodeT* pending_token;
    BindingT* constraints;
    int primed;
    int allowBreak;
    int allowVar;
} ParserT;


NodeT* parser_parse_user_conf(ParserT* parser);
TemplateT* parser_parse_template(ParserT* parser, BindingT** constraints);

NodeT* parser_parse_conf(ParserT* parser);
NodeT* parser_parse_server_block(ParserT* parser);
NodeT* parser_parse_section_list(ParserT* parser, int alsoSkip);
NodeT* parser_parse_section(ParserT* parser, int alsoSkip);


int valid_suffix(const char* domain, const char* text) {
    int dlen, tlen;
    if (!text) return 0;
    dlen = strlen(domain);
    tlen = strlen(text);
    if (dlen == tlen) {
        return (strcmp(domain, text) == 0);
    }
    else
    if ((dlen < tlen) && (text[tlen - dlen - 1] == '.')) {
        return (strcmp(domain, &text[tlen - dlen]) == 0);
    }
    else {
        return 0;
    }
}

int valid_prefix(const char* sub, const char* text) {
    int slen, tlen, c;
    if (!text) return 0;
    slen = strlen(sub);
    tlen = strlen(text);
    if (slen == tlen) {
        return (strcmp(sub, text) == 0);
    }
    else
    if (slen < tlen) {
        for (c = 0; c < slen; c++) {
            if (sub[c] != text[c]) return 0;
        }
        return 1;
    }
    else {
        return 0;
    }
}

int valid_directive(const char* dir, const char* value, BindingT* constraints) {
    if (!constraints) return 1;
    while (constraints) {
        if (strcmp(dir, constraints->name) == 0) {
            if (value == constraints->value) return 1;
            if ((value == NULL) && (constraints->value != NULL) && (constraints->value[0] == '\0')) return 1;
            if ((value != NULL) && (constraints->value != NULL) && valid_prefix(constraints->value, value)) return 1;
        }
        constraints = constraints->next;
    }
    return 0;
}


TemplateT* init_template() {
    TemplateT* t = (TemplateT*) malloc(sizeof(TemplateT));
    if (!t) error("Out of memory");
    return t;
}

void release_template(TemplateT* t) {
    TemplateT* rest = t;
    while (rest) {
        t = rest;
        rest = rest->next;

        if (t->prefix) free(t->prefix);
        release_node(t->headPattern);
        release_node(t->tailPattern);
        free(t);
    }
}


void release_parser(ParserT* parser) {
    if (parser) {
        release_scanner(parser->scanner);
        release_binding(parser->constraints);
        if (parser->pending_token) release_node(parser->pending_token);
        free(parser);
    }
}

ParserT* init_parser(const char* filename, const char* domain, int allowBreak, BindingT* constraints, BindingT* bindings) {
    ParserT* parser = (ParserT*) malloc(sizeof(ParserT));
    if (!parser) error("Out of memory");

    parser->filename = filename;
    parser->domain = domain;
    parser->scanner = init_scanner(filename, bindings);
    parser->constraints = constraints;
    parser->pending_token = NULL;
    parser->primed = 0;
    parser->allowBreak = allowBreak;
    return parser;
}

NodeT* next_token(ParserT* parser, int allowWord) {
    NodeT* token;
    if (!parser->primed) {
        parser->pending_token = scanner_scan(parser->scanner, allowWord, parser->allowBreak);
        parser->primed = 1;
    }
    token = parser->pending_token;
    return token;
}

NodeT* consume_token(ParserT* parser) {
    NodeT* token = parser->pending_token;
    parser->primed = 0;
    parser->pending_token = NULL;
    return token;
}

NodeT* parse_user_conf(const char* filename, const char* domain, BindingT* constraints) {
    ParserT* parser = init_parser(filename, domain, 0, constraints, NULL);
    NodeT* node = parser_parse_conf(parser);
    release_parser(parser);
    return node;
}

TemplateT* parse_template(const char* filename, const char* domain, BindingT* bindings, BindingT** constraints) {
    ParserT* parser = init_parser(filename, domain, 1, NULL, bindings);
    TemplateT* t = parser_parse_template(parser, constraints);
    release_parser(parser);
    return t;
}

NodeT* init_chain() {
    NodeT* node = (NodeT*) malloc(sizeof(NodeT));
    if (!node) error("Out of memory");

    node->kind = NK_CHAIN;
    node->text = NULL;
    node->children = NULL;
    node->next = NULL;
    node->line = 0;
    return node;
}

NodeT* chain_add(NodeT* chain, NodeT* last, NodeT* item) {
    /* returns the new last item */

    if (last) {
        last->next = item;
    }
    else {
        chain->children = item;
    }
    return item;
}

int chain_take(ParserT* parser, NodeT* chain, NodeT** last, NodeT** token, int kinds, int allowWord) {
    *token = next_token(parser, allowWord);
    if ((*token)->kind & kinds) {
        *last = chain_add(chain, *last, *token);
        consume_token(parser);
        return 1;
    }
    else {
        return 0;
    }
}

int chain_gobble_skip(ParserT* parser, NodeT* chain, NodeT** last, NodeT** token, int alsoSkip, int allowWord, int allowEOL, int continueAfterEOL) {
    int done = 0;
    int stop = 0;
    *token = next_token(parser, allowWord);
    while ((*token)->kind & (NK_SKIP|alsoSkip)) {
        consume_token(parser);
        *last = chain_add(chain, *last, *token);
        if ((*token)->ends_line && !allowEOL) break;
        stop = ((*token)->ends_line && !continueAfterEOL);
        *token = next_token(parser, allowWord);
        if (stop) break;
    }
    return done != 0;
}

NodeT* skip_tokens(ParserT* parser, int allowWord, int kinds, int allowEOL) {
    NodeT* token = next_token(parser, allowWord);
    while ((token->kind & kinds) && (allowEOL || !token->ends_line)) {
        consume_token(parser);
        release_node(token);
        token = next_token(parser, allowWord);
    }
    return token;
}

NodeT* take_token(ParserT* parser, int allowWord, int kinds, int skips, int allowEOL, int failOnError) {
    NodeT* token = next_token(parser, allowWord);
    while ((token->kind & skips) && (allowEOL || !token->ends_line)) {
        consume_token(parser);
        release_node(token);
        token = next_token(parser, allowWord);
    }
    if (!(token->kind & kinds)) {
        if (failOnError) {
            error("%s:%d: template format error", parser->filename, token->line);
        }
        else {
            token = NULL;
        }
    }
    else {
        token = consume_token(parser);
    }
    return token;
}

NodeT* parser_parse_conf(ParserT* parser) {
    NodeT* conf = init_chain();
    NodeT* last = NULL;

    NodeT* item = parser_parse_server_block(parser);
    while (item) {
        last = chain_add(conf, last, item);
        item = parser_parse_server_block(parser);
    }
    return conf;
}

NodeT* parser_parse_server_block(ParserT* parser) {
    int server_line, num_domains;
    NodeT* chain;
    NodeT* last;
    NodeT* token = next_token(parser, 1);

    skip_tokens(parser, 1, NK_SKIP, 1);
    if (token->kind != NK_WORD) return NULL;

    chain = init_chain();
    last = NULL;

    if (!chain_take(parser, chain, &last, &token, NK_WORD, 0) || (strcmp("server", token->text) != 0)) error("%s:%d: server directive expected", parser->filename, token->line);
    server_line = token->line;

    skip_tokens(parser, 0, NK_SKIP, 1);

    num_domains = 0;
    while (chain_take(parser, chain, &last, &token, NK_CONTENT, 0)) {
        if (token->kind & NK_CONTENT) {
            if (!valid_suffix(parser->domain, token->text)) error("%s:%d: domain not valid (%s)", parser->filename, token->line, token->text);
            num_domains++;
        }
        skip_tokens(parser, 0, NK_SKIP, 1);
    }
    if (num_domains == 0) error("%s:%d: domain name expected", parser->filename, server_line);

    if (!chain_take(parser, chain, &last, &token, NK_LB, 0)) error("%s:%d: { expected", parser->filename, token->line);
    if (!token->ends_line) chain_gobble_skip(parser, chain, &last, &token, NK_BREAK, 1, 1, 0);

    last = chain_add(chain, last, parser_parse_section_list(parser, NK_BREAK));

    if (!chain_take(parser, chain, &last, &token, NK_RB, 1)) error("%s:%d: } expected", parser->filename, token->line);
    if (!token->ends_line) chain_gobble_skip(parser, chain, &last, &token, NK_BREAK, 1, 1, 0);

    return chain;
}

NodeT* parser_parse_section_list(ParserT* parser, int alsoSkip) {
    NodeT* chain = init_chain();
    NodeT* last = NULL;

    NodeT* item = parser_parse_section(parser, alsoSkip);
    while (item) {
        last = chain_add(chain, last, item);
        item = parser_parse_section(parser, alsoSkip);
    }
    return chain;
}

NodeT* parser_parse_section(ParserT* parser, int alsoSkip) {
    NodeT* chain;
    NodeT* last;
    NodeT* token = next_token(parser, 1);
    NodeT* dir = NULL;
    int ok = 0;

    if ((token->kind & (NK_SKIP|alsoSkip)) && token->ends_line) return consume_token(parser);
    if ((token->kind == NK_BREAK) && !(NK_BREAK & alsoSkip)) return NULL;
    if (token->kind == NK_EOF) return NULL;

    chain = init_chain();
    last = NULL;

    chain_gobble_skip(parser, chain, &last, &token, alsoSkip, 1, 0, 0);

    if (token->kind == NK_RB) {
        NodeT* first = chain->children;
        chain->children = NULL;
        return first;
    }

    if (!chain_take(parser, chain, &last, &token, NK_WORD, 1)) error("%s:%d: directive expected", parser->filename, token->line);
    dir = token;
    ok = valid_directive(dir->text, NULL, parser->constraints);

    chain_gobble_skip(parser, chain, &last, &token, alsoSkip, 0, 1, 1);

    if (chain_take(parser, chain, &last, &token, NK_CONTENT|NK_STRING|NK_SKIP|alsoSkip, 0)) {
        ok = ok || valid_directive(dir->text, token->text, parser->constraints);
        if (!ok) error("%s:%d: directive and parameter invalid", parser->filename, dir->line);

        while (chain_take(parser, chain, &last, &token, NK_CONTENT|NK_STRING|NK_SKIP|alsoSkip, 0));
    }
    if (!ok) error("%s:%d: directive invalid", parser->filename, dir->line);

    if (token->kind == NK_LB) {
        chain_take(parser, chain, &last, &token, NK_LB, 0);
        if (!token->ends_line) chain_gobble_skip(parser, chain, &last, &token, alsoSkip, 0, 1, 0);

        last = chain_add(chain, last, parser_parse_section_list(parser, alsoSkip));

        if (!chain_take(parser, chain, &last, &token, NK_RB, 1)) error("%s:%d: } expected", parser->filename, token->line);
    }
    else
    if (token->kind == NK_SEMI) {
        chain_take(parser, chain, &last, &token, NK_SEMI, 1);
    }
    else
    if (token->kind != NK_BREAK) {
        error("%s:%d: { or ; expected", parser->filename, token->line);
    }
    if (!token->ends_line) chain_gobble_skip(parser, chain, &last, &token, alsoSkip, 1, 1, 0);

    return chain;
}

BindingT* parser_parse_pair(ParserT* parser) {
    BindingT* binding = NULL;
    NodeT* name;
    NodeT* value;
    name = take_token(parser, 1, NK_WORD, NK_SKIP, 1, 0);
    if (name) {
        value = take_token(parser, 0, NK_CONTENT, NK_SKIP, 0, 0);
        if ((value) && !value->ends_line) skip_tokens(parser, 1, NK_SKIP, 0);
        binding = init_binding(name->text, (value)? value->text : NULL);
        release_node(name);
        release_node(value);
    }
    return binding;
}

char* extract_prefix(const char* text) {
    int start, rest;
    char* result;

    if (text[0] != '=') return NULL;
    for (start = 0; (text[start] != '\0') && ((text[start] == '=') || (text[start] == ' ') || (text[start] == '\t')); start++);
    if (text[start++] != 'h') return NULL;
    if (text[start++] != 'o') return NULL;
    if (text[start++] != 's') return NULL;
    if (text[start++] != 't') return NULL;
    if ((text[start] != ' ') && (text[start] != '\t')) return NULL;
    start++;
    for (; (text[start] != '\0') && ((text[start] == ' ') || (text[start] == '\t')); start++);
    for (rest = start; (text[rest] != '\0') && (text[rest] == ' ') && (text[rest] != '\t') && (text[rest] != '='); rest++);

    result = (char*) malloc(rest - start + 1);
    memcpy(result, &text[start], rest - start);
    result[rest - start] = '\0';
    return result;
}

TemplateT* parser_parse_template(ParserT* parser, BindingT** constraints) {
    BindingT* list = NULL;
    BindingT* item = NULL;
    NodeT* headPattern = NULL;
    NodeT* tailPattern = NULL;
    TemplateT* t = NULL;
    TemplateT* tn = NULL;
    NodeT* token = NULL;
    char* prefix;

    release_node(take_token(parser, 0, NK_BREAK, NK_SKIP, 1, 0));

    item = parser_parse_pair(parser);
    while (item) {
        list = add_known_binding(list, item);
        item = parser_parse_pair(parser);
    }
    *constraints = list;

    token = take_token(parser, 0, NK_BREAK, NK_SKIP, 1, 0);
    while (token) {
        prefix = extract_prefix(token->text);
        if (!prefix) error("%s:%d: host template expecteded", parser->filename, token->line);
        release_node(token);

        headPattern = parser_parse_section_list(parser, 0);

        token = take_token(parser, 0, NK_BREAK, NK_SKIP, 1, 0);
        if ((token) && (token->text[0] != '=')) {
            release_node(token);

            tailPattern = parser_parse_section_list(parser, 0);

            token = take_token(parser, 0, NK_BREAK, NK_SKIP, 1, 0);
        }

        tn = t;
        t = init_template();
        t->prefix = prefix;
        t->headPattern = headPattern;
        t->tailPattern = tailPattern;
        t->next = tn;
    }
    return t;
}

