/*
 * Copyright 2026 TeamMyonlang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MYON_TOKEN_H
#define MYON_TOKEN_H

/*
 * Token definitions for the Myon lexer.
 * Reference: spec.md section 1 (lexical rules).
 */

typedef enum {
    /* literals / identifiers */
    TOK_INT,        /* integer literal (dec/hex/oct)          */
    TOK_FLOAT,      /* float literal                          */
    TOK_STRING,     /* raw string body between double quotes  */
    TOK_IDENT,      /* identifier                             */

    /* keywords (1.5) */
    TOK_KW_SYSTEM,
    TOK_KW_MODULE,
    TOK_KW_MYON,        /* the bare "myon" namespace token          */
    TOK_KW_RET,
    TOK_KW_STR,
    TOK_KW_CHAR,
    TOK_KW_INT,
    TOK_KW_FLOAT,
    TOK_KW_BOOL,
    TOK_KW_VOID,
    TOK_KW_ERROR,
    TOK_KW_THEN,
    TOK_KW_RANGE,
    TOK_KW_SELF,
    TOK_KW_AS,
    TOK_KW_TRUE,
    TOK_KW_FALSE,

    /*
     * "myon.<name>" compound keywords.  The lexer recognises the
     * "myon" ident followed by "." followed by a known member and
     * folds them into a single token to simplify the parser.
     */
    TOK_MYON_IF,
    TOK_MYON_ELIF,
    TOK_MYON_ELSE,
    TOK_MYON_WHILE,
    TOK_MYON_FOR,
    TOK_MYON_IN,
    TOK_MYON_BREAK,
    TOK_MYON_CONTINUE,
    TOK_MYON_FUNC,
    TOK_MYON_STRUCT,
    TOK_MYON_ARRAY,
    TOK_MYON_MAP,
    TOK_MYON_PRINT,
    TOK_MYON_INPUT,
    TOK_MYON_AND,
    TOK_MYON_OR,
    TOK_MYON_NOT,
    TOK_MYON_NIL,
    TOK_MYON_EXPOSE,
    TOK_MYON_LAMBDA,
    TOK_MYON_EXTENDS,
    TOK_MYON_ASYNC,
    TOK_MYON_AWAIT,

    /* operators / symbols */
    TOK_EQ,         /* ==  */
    TOK_NEQ,        /* !=  */
    TOK_LT,         /* <   */
    TOK_GT,         /* >   */
    TOK_LE,         /* <=  */
    TOK_GE,         /* >=  */
    TOK_PLUS,       /* +   */
    TOK_MINUS,      /* -   */
    TOK_STAR,       /* *   */
    TOK_SLASH,      /* /   */
    TOK_PLUS_EQ,    /* +=  */
    TOK_MINUS_EQ,   /* -=  */
    TOK_STAR_EQ,    /* *=  */
    TOK_SLASH_EQ,   /* /=  */
    TOK_ASSIGN,     /* =   */
    TOK_DOT,        /* .   */
    TOK_COMMA,      /* ,   */
    TOK_COLON,      /* :   */
    TOK_SEMICOLON,  /* ;   */
    TOK_LPAREN,     /* (   */
    TOK_RPAREN,     /* )   */
    TOK_LBRACE,     /* {   */
    TOK_RBRACE,     /* }   */
    TOK_LBRACKET,   /* [   */
    TOK_RBRACKET,   /* ]   */

    TOK_NEWLINE,    /* statement separator */
    TOK_EOF
} TokenType;

typedef struct {
    TokenType type;
    char     *lexeme;  /* owned copy of the token text (may be NULL) */
    int       line;
    int       col;
} Token;

const char *token_type_name(TokenType type);

#endif /* MYON_TOKEN_H */
